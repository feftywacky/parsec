#include "ui/dialog_connect.hpp"

#include <imgui.h>
#include <imgui_internal.h>  // ImGuiContext::InputTextState -- see scrub_imgui_text_state()

#include <cstring>

#include "parsec/parsec.h"
#include "ui/theme.hpp"

namespace pc::ui {
namespace {

// A plain memset on a dying buffer can be optimized away as a dead store (docs/06 §4).
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

}  // namespace

ConnectDialog::~ConnectDialog() {
    if (worker_.joinable())
        worker_.join();
    volatile_zero(master_key_, sizeof(master_key_));
    volatile_zero(passphrase_, sizeof(passphrase_));
    volatile_zero(confirm_, sizeof(confirm_));
}

void ConnectDialog::done_with_passphrase() noexcept {
    volatile_zero(passphrase_, sizeof(passphrase_));
}

void ConnectDialog::reopen(const std::string& keystore_path) noexcept {
    if (worker_.joinable())
        worker_.join();
    volatile_zero(master_key_, sizeof(master_key_));
    volatile_zero(passphrase_, sizeof(passphrase_));
    volatile_zero(confirm_, sizeof(confirm_));
    mainnet_confirm_[0] = '\0';
    {
        std::lock_guard<std::mutex> lock(message_mutex_);
        message_.clear();
    }
    // Re-taken from the caller rather than kept: after a reset the engine has forgotten its
    // keystore path, and the previous run's written_path_ points at a file that is now gone.
    written_path_ = keystore_path;
    stage_.store(Stage::Form, std::memory_order_release);
    dismissed_ = false;
}

void ConnectDialog::start() noexcept {
    {
        std::lock_guard<std::mutex> lock(message_mutex_);
        message_ = "Generating agent wallet...";
    }
    stage_.store(Stage::Working, std::memory_order_release);
    // The master key has now been handed to the worker; drop ImGui's own copies of it
    // immediately rather than at the end of the flow, which is seconds of network and KDF
    // away.
    scrub_imgui_text_state();

    // The worker owns the master key from here. It is captured by pointer into this
    // dialog's own buffer (not copied into a std::string, which offers no zeroing
    // guarantee) and pc_setup_sign_with_master zeroes it the instant signing returns.
    worker_ = std::thread([this] {
        auto fail = [this](const char* what, pc_setup* setup) {
            char err[256]{};
            std::string text = what;
            if (setup != nullptr && pc_setup_last_error(setup, err, sizeof(err)) > 0 &&
                err[0] != '\0') {
                text += ": ";
                text += err;
            }
            {
                std::lock_guard<std::mutex> lock(message_mutex_);
                message_ = std::move(text);
            }
            stage_.store(Stage::Failed, std::memory_order_release);
        };

        pc_setup* setup = pc_setup_begin(mainnet_, nullptr, 0);
        if (setup == nullptr) {
            volatile_zero(master_key_, sizeof(master_key_));
            fail("Could not start onboarding", nullptr);
            return;
        }

        // Zeroes master_key_ itself before returning, on success and on failure alike.
        if (pc_setup_sign_with_master(setup, master_key_) != 0) {
            fail("Could not sign the approval", setup);
            pc_setup_free(setup);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(message_mutex_);
            message_ = "Approving agent wallet at the venue...";
        }
        if (pc_setup_submit(setup) != 0) {
            fail("The venue rejected the approval", setup);
            pc_setup_free(setup);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(message_mutex_);
            message_ = "Encrypting keystore (Argon2id)...";
        }
        if (pc_setup_write_keystore(setup, passphrase_, written_path_.c_str()) != 0) {
            fail("Approved, but could not write the keystore", setup);
            pc_setup_free(setup);
            return;
        }

        pc_setup_free(setup);
        stage_.store(Stage::Done, std::memory_order_release);
    });
}

bool ConnectDialog::draw(bool mainnet, const std::string& keystore_path) noexcept {
    if (dismissed_)
        return false;

    mainnet_ = mainnet;
    if (written_path_.empty())
        written_path_ = keystore_path;

    const Stage stage = stage_.load(std::memory_order_acquire);

    if (stage == Stage::Done) {
        if (worker_.joinable())
            worker_.join();
        // The caller unlocks with passphrase_ and then calls done_with_passphrase().
        volatile_zero(master_key_, sizeof(master_key_));
        volatile_zero(confirm_, sizeof(confirm_));
        scrub_imgui_text_state();
        dismissed_ = true;
        return true;
    }

    ImGui::OpenPopup("Connect Account");
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Connect Account", nullptr,
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(mainnet ? kColorAsk : kColorWarning, "%s",
                           mainnet ? "MAINNET -- real funds" : "TESTNET");
        ImGui::TextWrapped(
            "parsec does not store your MetaMask key. It creates a separate agent wallet, "
            "uses your key once to approve it, and stores only the agent key, encrypted. "
            "The agent can trade this account but cannot withdraw from it.");
        ImGui::Separator();

        const bool working = stage == Stage::Working;
        ImGui::BeginDisabled(working);

        ImGui::TextUnformatted("1. MetaMask private key");
        ImGui::TextDisabled("   Signs one approval, then wiped. Never stored, never sent.");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputText("##master", master_key_, sizeof(master_key_),
                         ImGuiInputTextFlags_Password);

        ImGui::Spacing();
        ImGui::TextUnformatted("2. Choose a keystore passphrase");
        ImGui::TextDisabled("   New password you invent now. Encrypts the agent key on this");
        ImGui::TextDisabled("   machine; you will be asked for it each time parsec starts.");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputText("##pass", passphrase_, sizeof(passphrase_),
                         ImGuiInputTextFlags_Password);

        // Labelled explicitly: two adjacent unlabelled password boxes are indistinguishable,
        // and guessing wrong here means a keystore sealed with a passphrase you did not mean.
        ImGui::TextUnformatted("3. Confirm that passphrase");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputText("##confirm", confirm_, sizeof(confirm_), ImGuiInputTextFlags_Password);

        bool confirmed = true;
        if (mainnet) {
            // Second, independent signal for mainnet (docs/06 §5.7).
            ImGui::TextColored(kColorWarning, "Type MAINNET to confirm:");
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::InputText("##mainnet_confirm", mainnet_confirm_, sizeof(mainnet_confirm_));
            confirmed = std::strncmp(mainnet_confirm_, "MAINNET", sizeof(mainnet_confirm_)) == 0;
        }
        ImGui::EndDisabled();

        const bool key_ok = master_key_[0] != '\0';
        const bool pass_ok = passphrase_[0] != '\0';
        const bool match = std::strcmp(passphrase_, confirm_) == 0;

        // Wrapped, not a single line: a venue rejection is a full sentence of explanation
        // ("Extra agent name must be between 1 and 16 characters long.") and running it off
        // the edge of the popup hides the half that says what to do about it.
        if (working || stage == Stage::Failed) {
            std::lock_guard<std::mutex> lock(message_mutex_);
            ImGui::PushStyleColor(ImGuiCol_Text, working ? kColorWarning : kColorAsk);
            ImGui::PushTextWrapPos(0.0F);
            ImGui::TextUnformatted(message_.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        } else if (pass_ok && !match) {
            ImGui::TextColored(kColorAsk, "Passphrases do not match.");
        } else {
            ImGui::TextDisabled("Keystore: %s", written_path_.c_str());
        }

        const bool can_start = !working && key_ok && pass_ok && match && confirmed;
        ImGui::BeginDisabled(!can_start);
        if (ImGui::Button("Connect account", ImVec2(160, 0))) {
            // A retry after a failure needs the old thread reaped first.
            if (worker_.joinable())
                worker_.join();
            start();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(working);
        if (ImGui::Button("Later", ImVec2(120, 0))) {
            volatile_zero(master_key_, sizeof(master_key_));
            volatile_zero(passphrase_, sizeof(passphrase_));
            volatile_zero(confirm_, sizeof(confirm_));
            scrub_imgui_text_state();
            dismissed_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();


        ImGui::EndPopup();
    }

    // Success is reported from the Stage::Done branch at the top of the next frame, once the
    // worker has been joined -- never from here, where the thread may still be running.
    return false;
}

}  // namespace pc::ui
