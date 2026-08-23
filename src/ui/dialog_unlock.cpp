#include "ui/dialog_unlock.hpp"

#include <imgui.h>
#include <imgui_internal.h>  // ImGuiContext::InputTextState -- see scrub_imgui_text_state()

#include <chrono>
#include <cstring>

#include "parsec/parsec.h"
#include "ui/theme.hpp"

namespace pc::ui {
namespace {

// A plain memset/`= 0` on a dying buffer can be optimized away as a dead store (docs/06 §4,
// reproduced there with clang -O2 on arm64: zero store instructions emitted). Writing through
// a volatile pointer is observable behaviour the compiler cannot elide.
void volatile_zero(char* buf, size_t len) noexcept {
    volatile char* p = buf;
    for (size_t i = 0; i < len; ++i)
        p[i] = 0;
}

// ImGui keeps its OWN heap copies of whatever is typed into an InputText -- the live buffer,
// a revert-to-on-Escape backup taken at focus time, a callback backup, and a further copy
// stashed on deactivation -- and deliberately does not clear them ("we otherwise prefer to
// keep/amortize the allocation", imgui_widgets.cpp). Zeroing only our own char[] would
// therefore leave the secret sitting in ImGui's allocations for the rest of the process's
// life, which defeats the point of docs/06 §4. There is exactly one InputText state in an
// ImGui context, so scrubbing it unconditionally covers whichever field was last edited.
//
// Residual, deliberately not papered over: stb_textedit's undo ring (state->Stb->undostate.
// undo_char) can retain characters that were *deleted* while editing. It is not reachable
// from imgui_internal.h alone (ImStbTexteditState is an opaque typedef there), so a secret
// that was typed and then partially erased may still leave fragments. Pasting and submitting
// -- the normal path -- performs no deletions and leaves nothing there.
void scrub_imgui_text_state() noexcept {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx == nullptr)
        return;
    auto wipe = [](ImVector<char>& v) {
        if (v.Data != nullptr && v.Size > 0)
            volatile_zero(v.Data, static_cast<size_t>(v.Size));
    };
    wipe(ctx->InputTextState.TextA);
    wipe(ctx->InputTextState.TextToRevertTo);
    wipe(ctx->InputTextState.CallbackTextBackup);
    ctx->InputTextState.TextLen = 0;
    wipe(ctx->InputTextDeactivatedState.TextA);
}

void set_error(char* out, size_t cap, const char* message) noexcept {
    std::strncpy(out, message, cap - 1);
    out[cap - 1] = '\0';
}

uint64_t now_ms() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

// Agent approvals lapse on a deadline the venue enforces, and the failure mode is orders
// being refused mid-session rather than anything visible beforehand -- so the countdown is
// shown while there is still time to act on it (docs/06 §1).
constexpr uint64_t kWarnWindowMs = 14ULL * 24 * 60 * 60 * 1000;

}  // namespace

void UnlockDialog::reset() noexcept {
    volatile_zero(passphrase_, sizeof(passphrase_));
    scrub_imgui_text_state();
    error_[0] = '\0';
}

void UnlockDialog::rearm() noexcept {
    reset();
    peeked_ = false;
    master_[0] = '\0';
    valid_until_ms_ = 0;
    reset_requested_ = false;
    dismissed_ = false;
    unlocked_ = false;
}

void UnlockDialog::peek(const std::string& keystore_path) noexcept {
    peeked_ = true;
    master_[0] = '\0';
    valid_until_ms_ = 0;
    if (keystore_path.empty())
        return;
    // Every field here is cleartext in the file and AAD-bound, so this needs no passphrase
    // and cannot be spoofed without making the keystore undecryptable.
    if (pc_keystore_peek(keystore_path.c_str(), master_, sizeof(master_), nullptr, 0,
                         &valid_until_ms_, nullptr) != 0) {
        master_[0] = '\0';
        valid_until_ms_ = 0;
    }
}

void UnlockDialog::try_unlock() noexcept {
    if (!submit_) {
        set_error(error_, sizeof(error_), "No keystore configured for this build.");
        volatile_zero(passphrase_, sizeof(passphrase_));
        return;
    }

    error_[0] = '\0';
    const bool started = submit_(passphrase_);
    // The passphrase is never held past this call: zero it immediately regardless of outcome,
    // and never copy it anywhere else (docs/06 §5.1/§5.2 -- it must not cross into any
    // longer-lived structure, and must never be logged or formatted). pc_unlock zeroes it too;
    // this is the belt to that braces, since the buffer is ours.
    volatile_zero(passphrase_, sizeof(passphrase_));
    scrub_imgui_text_state();

    if (started)
        pending_ = true;
    else
        set_error(error_, sizeof(error_), "Could not start the unlock.");
}

bool UnlockDialog::draw(bool mainnet, const std::string& keystore_path) noexcept {
    if (unlocked_ || dismissed_)
        return false;

    if (!peeked_)
        peek(keystore_path);

    // Poll the outcome of an in-flight attempt. Argon2id runs on a Rust blocking task, so the
    // render loop keeps running at full rate throughout -- this is the whole reason the dialog
    // is a state machine rather than a blocking call.
    if (pending_ && status_) {
        switch (status_()) {
            case PC_AUTH_UNLOCKED:
                pending_ = false;
                unlocked_ = true;
                break;
            case PC_AUTH_FAILED:
                pending_ = false;
                // Wrong passphrase and a corrupted keystore are the same AEAD tag failure and
                // are deliberately not distinguished (docs/06 §3) -- and never hint at how
                // close the passphrase was, because with an AEAD tag there is no such thing.
                set_error(error_, sizeof(error_),
                          "Wrong passphrase, or the keystore is corrupted.");
                break;
            default:
                break;  // still PC_AUTH_UNLOCKING
        }
    }

    bool just_unlocked = false;
    ImGui::OpenPopup("Unlock Keystore");
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Unlock Keystore", nullptr,
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(mainnet ? kColorAsk : kColorWarning, "%s",
                           mainnet ? "MAINNET keystore" : "TESTNET keystore");
        ImGui::TextWrapped(
            "Enter the passphrase for this keystore. It unlocks the agent key that signs "
            "orders; the key stays in this process and is never written anywhere.");

        // Naming the account turns a bare passphrase box into a check the user can make:
        // with two networks and possibly two machines, "which keystore is this?" is a real
        // question, and the answer is already cleartext in the file.
        if (master_[0] != '\0')
            ImGui::TextDisabled("Account: %s", master_);
        if (valid_until_ms_ != 0) {
            const uint64_t now = now_ms();
            if (now < valid_until_ms_ && valid_until_ms_ - now <= kWarnWindowMs) {
                // Integer days, rounded down, so "1 day left" never means "a few hours ago
                // it was 2 and it is really 1.9" -- the pessimistic read is the useful one.
                const uint64_t days = (valid_until_ms_ - now) / (24ULL * 60 * 60 * 1000);
                ImGui::TextColored(kColorWarning,
                                   "Agent approval expires in %llu day%s. Start fresh before "
                                   "then to avoid refused orders.",
                                   static_cast<unsigned long long>(days),
                                   days == 1 ? "" : "s");
            }
        }
        ImGui::Separator();

        ImGui::BeginDisabled(pending_);
        const bool submitted_by_enter =
            ImGui::InputText("Passphrase", passphrase_, sizeof(passphrase_),
                             ImGuiInputTextFlags_Password |
                                 ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::EndDisabled();

        if (pending_) {
            // Argon2id at the SENSITIVE tier is ~3.5s and that is intentional (docs/06 §3);
            // saying so turns a hang into a wait.
            ImGui::TextColored(kColorWarning, "Unlocking... (Argon2id, ~3.5s -- deliberately)");
        } else if (error_[0] != '\0') {
            ImGui::TextColored(kColorAsk, "%s", error_);
        }

        const bool can_submit = !pending_ && passphrase_[0] != '\0';
        ImGui::BeginDisabled(!can_submit);
        const bool clicked = ImGui::Button("Unlock", ImVec2(120, 0));
        ImGui::EndDisabled();
        if (can_submit && (clicked || submitted_by_enter))
            try_unlock();

        ImGui::SameLine();
        ImGui::BeginDisabled(pending_);
        if (ImGui::Button("Trade later", ImVec2(120, 0))) {
            reset();
            dismissed_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();

        // The escape hatch for the case this dialog cannot otherwise resolve: a passphrase
        // that is genuinely lost. Without it the only way out of a keystore nobody can open
        // is deleting a file by hand, which is not something a trading terminal should make
        // its users do. Deliberately last and unemphasised -- it destroys an agent key.
        ImGui::SameLine();
        ImGui::BeginDisabled(pending_);
        if (ImGui::Button("Start fresh...", ImVec2(120, 0)))
            reset_requested_ = true;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Forgot the passphrase? Delete this keystore and approve a new agent wallet.");

        ImGui::TextDisabled("Dismissing leaves parsec read-only: live market data, no trading.");

        if (unlocked_) {
            just_unlocked = true;
            // CloseCurrentPopup() must be called while this popup is still the current one --
            // i.e. inside this Begin/End block, not after EndPopup() below.
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if (just_unlocked) {
        // error_ is cleared here; try_unlock() already zeroed passphrase_ regardless of
        // outcome.
        reset();
        return true;
    }
    return false;
}

}  // namespace pc::ui
