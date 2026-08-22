#include "ui/dialog_unlock.hpp"

#include <imgui.h>

#include <cstring>

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

}  // namespace

void UnlockDialog::reset() noexcept {
    volatile_zero(passphrase_, sizeof(passphrase_));
    volatile_zero(mainnet_confirm_, sizeof(mainnet_confirm_));
    error_[0] = '\0';
}

void UnlockDialog::try_unlock() noexcept {
    if (!verifier_) {
        std::strncpy(error_, "No keystore configured for this build.", sizeof(error_) - 1);
        error_[sizeof(error_) - 1] = '\0';
        volatile_zero(passphrase_, sizeof(passphrase_));
        return;
    }

    error_[0] = '\0';
    const bool ok = verifier_(passphrase_, error_, sizeof(error_));
    // The passphrase is never held past this call: zero it immediately regardless of outcome,
    // and never copy it anywhere else (docs/06 §5.1/§5.2 -- it must not cross into any
    // longer-lived structure, and must never be logged or formatted).
    volatile_zero(passphrase_, sizeof(passphrase_));

    if (ok) {
        unlocked_ = true;
    } else if (error_[0] == '\0') {
        std::strncpy(error_, "Incorrect passphrase.", sizeof(error_) - 1);
        error_[sizeof(error_) - 1] = '\0';
    }
}

bool UnlockDialog::draw(bool mainnet) noexcept {
    if (unlocked_)
        return false;  // already reported success on the frame it happened; nothing left to do

    bool just_unlocked = false;
    ImGui::OpenPopup("Unlock Keystore");
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Unlock Keystore", nullptr,
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(mainnet ? kColorAsk : kColorWarning, "%s",
                           mainnet ? "MAINNET keystore" : "TESTNET keystore");
        ImGui::TextWrapped(
            "Enter the passphrase for this keystore. It is held only for the duration of this "
            "call and is never written to disk or logged.");
        ImGui::Separator();

        ImGui::InputText("Passphrase", passphrase_, sizeof(passphrase_),
                         ImGuiInputTextFlags_Password);

        bool confirmed = true;
        if (mainnet) {
            // Second, independent signal for mainnet (docs/06 §5.7): a typed confirmation in
            // addition to the config flag that got us here at all.
            ImGui::TextColored(kColorWarning,
                               "Type MAINNET to confirm you intend to trade real funds:");
            ImGui::InputText("##mainnet_confirm", mainnet_confirm_, sizeof(mainnet_confirm_));
            confirmed = std::strncmp(mainnet_confirm_, "MAINNET", sizeof(mainnet_confirm_)) == 0;
        }

        if (error_[0] != '\0')
            ImGui::TextColored(kColorAsk, "%s", error_);

        ImGui::BeginDisabled(passphrase_[0] == '\0' || !confirmed);
        if (ImGui::Button("Unlock", ImVec2(120, 0))) {
            try_unlock();
            if (unlocked_) {
                just_unlocked = true;
                // CloseCurrentPopup() must be called while this popup is still the current one
                // -- i.e. inside this Begin/End block, not after EndPopup() below.
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            reset();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if (just_unlocked) {
        // mainnet_confirm_/error_ are cleared here; try_unlock() already zeroed passphrase_
        // regardless of outcome.
        reset();
        return true;
    }
    return false;
}

}  // namespace pc::ui
