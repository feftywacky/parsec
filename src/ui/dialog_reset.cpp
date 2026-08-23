#include "ui/dialog_reset.hpp"

#include <imgui.h>

#include <cstring>

#include "ui/theme.hpp"

namespace pc::ui {

void ResetDialog::open(Reason reason) noexcept {
    reason_ = reason;
    open_ = true;
    confirm_[0] = '\0';
    error_.clear();
}

bool ResetDialog::draw(bool mainnet) noexcept {
    if (!open_)
        return false;

    const bool expired = reason_ == Reason::AgentExpired;
    const char* title = expired ? "Agent Approval Expired" : "Reset All Accounts";

    bool did_reset = false;
    ImGui::OpenPopup(title);
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(title, nullptr,
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(mainnet ? kColorAsk : kColorWarning, "%s",
                           mainnet ? "MAINNET -- real funds" : "TESTNET");

        if (expired) {
            ImGui::TextWrapped(
                "This keystore's agent wallet has passed its approval deadline. The venue no "
                "longer accepts orders signed by it, and no passphrase will change that -- "
                "the deadline is enforced at the venue, not in the file.");
            ImGui::Spacing();
            ImGui::TextWrapped(
                "To trade again, approve a fresh agent wallet. That needs your MetaMask key "
                "once more, for one signature, and a passphrase for the new keystore.");
        } else {
            ImGui::TextWrapped(
                "This deletes every keystore on this machine -- both mainnet and testnet -- "
                "and returns parsec to its first-run state.");
            ImGui::Spacing();
            ImGui::TextColored(kColorAsk, "%s",
                               "The agent keys in them are the only copies. They cannot be "
                               "recovered from a passphrase, a seed phrase, or MetaMask.");
        }

        ImGui::Spacing();
        // Said plainly in both cases: a dialog that deletes an approval's local half while
        // the venue still honours the other half must not let the user believe otherwise.
        ImGui::TextDisabled(
            "Your funds are untouched. An agent key can trade but never withdraw, and it is\n"
            "separate from your MetaMask key, which parsec has never stored.");
        ImGui::TextDisabled(
            "This clears parsec's local copy only. The approval itself stays on file at the\n"
            "venue until it lapses; revoking it needs the master key parsec does not keep.");

        ImGui::Separator();

        // A working keystore is destroyed only on an unambiguous signal. An expired one is
        // already inert, so demanding the same ceremony there would be friction with nothing
        // behind it -- the user has no choice to weigh.
        bool confirmed = true;
        if (!expired) {
            ImGui::TextColored(kColorWarning, "Type RESET to confirm:");
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::InputText("##reset_confirm", confirm_, sizeof(confirm_));
            confirmed = std::strncmp(confirm_, "RESET", sizeof(confirm_)) == 0;
        }

        if (!error_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kColorAsk);
            ImGui::PushTextWrapPos(0.0F);
            ImGui::TextUnformatted(error_.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        const char* confirm_label = expired ? "Approve a new agent" : "Delete keystores";
        ImGui::BeginDisabled(!confirmed || !reset_);
        if (ImGui::Button(confirm_label, ImVec2(180, 0))) {
            const int removed = reset_();
            if (removed < 0) {
                // Refusals are real and actionable -- resetting is blocked while a session is
                // unlocked -- so show the engine's reason rather than a generic failure.
                error_ = last_error_ ? last_error_() : std::string{};
                if (error_.empty())
                    error_ = "Could not delete the keystores.";
            } else {
                open_ = false;
                did_reset = true;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button(expired ? "Later" : "Cancel", ImVec2(120, 0))) {
            open_ = false;
            confirm_[0] = '\0';
            ImGui::CloseCurrentPopup();
        }

        ImGui::TextDisabled("Dismissing leaves parsec read-only: live market data, no trading.");

        ImGui::EndPopup();
    }

    return did_reset;
}

}  // namespace pc::ui
