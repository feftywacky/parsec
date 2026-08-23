#pragma once
// "Start fresh" -- delete every local keystore and go back to the connect flow (docs/06 §2).
//
// Two very different situations funnel through this one dialog, because the remedy for both
// is the same file deletion:
//
//   AgentExpired  -- the keystore is intact but the venue stopped honouring its agent key at
//                    `valid_until_ms`. There is nothing to lose and no passphrase that helps,
//                    so this asks for no confirmation beyond the button itself.
//   UserRequested -- the user chose to wipe a keystore that still works. That destroys the
//                    only copy of an agent key, so it takes a typed confirmation.
//
// The deletion is local. The agent approval it forgets still stands at the venue until it
// lapses on its own; parsec cannot revoke it, because revoking needs the master key parsec
// deliberately never stores. Nothing is at risk either way -- an agent key is fundless and
// cannot withdraw -- but this dialog says so rather than implying a revocation it did not do.
//
// Unlike the unlock and connect dialogs, nothing here blocks: pc_reset_accounts is a couple
// of unlink() calls, so it runs inline on the UI thread with no worker.
#include <cstddef>
#include <functional>
#include <string>

namespace pc::ui {

class ResetDialog {
public:
    enum class Reason {
        AgentExpired,   // the keystore's agent approval lapsed; no passphrase can open it
        UserRequested,  // the user asked to wipe a working keystore
    };

    // Deletes every local keystore. Returns the number of files removed, or -1 on refusal.
    using Reset = std::function<int()>;
    // Reason a refused reset was refused, for display.
    using LastError = std::function<std::string()>;

    void configure(Reset reset, LastError last_error) noexcept {
        reset_ = std::move(reset);
        last_error_ = std::move(last_error);
    }

    // Puts the dialog up. Re-opening after a dismissal is allowed and clears any stale
    // error -- unlike the unlock dialog, this one is reachable on purpose from a button.
    void open(Reason reason) noexcept;

    [[nodiscard]] bool is_open() const noexcept { return open_; }

    // Call once per frame while is_open(). Returns true on the single frame the keystores
    // were deleted; the caller should then drop any dismissal state on its connect dialog,
    // since the engine now reports PC_AUTH_NO_KEYSTORE and onboarding should reappear.
    bool draw(bool mainnet) noexcept;

private:
    static constexpr size_t kConfirmCap = 16;  // just needs to hold "RESET"

    char confirm_[kConfirmCap]{};
    std::string error_{};
    Reason reason_{Reason::AgentExpired};
    bool open_{false};
    Reset reset_{};
    LastError last_error_{};
};

}  // namespace pc::ui
