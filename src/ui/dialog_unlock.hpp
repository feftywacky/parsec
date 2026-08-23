#pragma once
// Passphrase-entry dialog for the local keystore (docs/06-security.md §2 onboarding, §4
// secrets-in-memory, §5 the enforceable rules).
//
// **Asynchronous, not a blocking verifier.** An earlier version of this header exposed a
// synchronous `bool verify(passphrase)` callback on the assumption that wiring it up was a
// one-liner. It is not: unlocking runs Argon2id at libsodium's SENSITIVE tier, ~3.5 seconds
// by design (docs/06 §3), and a synchronous verifier would freeze the render loop for that
// whole time -- on a trading terminal, with a live book on screen behind the modal. So the
// dialog submits an attempt and then polls: `Submit` starts the unlock (pc_unlock returns
// immediately), `Status` reports PC_AUTH_* each frame, and the dialog renders a progress
// state in between.
//
// The dialog still never holds a secret across frames beyond the input buffer itself, and
// zeroes that buffer the instant it is handed to Submit.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace pc::ui {

class UnlockDialog {
public:
    // Starts an unlock attempt. `passphrase` is a writable buffer the callee is expected to
    // zero (pc_unlock does); the dialog zeroes it again on return regardless. Returns false if
    // the attempt could not even be started (no keystore configured, one already in flight).
    using Submit = std::function<bool(char* passphrase)>;
    // Current PC_AUTH_* value, polled once per frame while the dialog is up.
    using Status = std::function<int()>;

    void configure(Submit submit, Status status) noexcept {
        submit_ = std::move(submit);
        status_ = std::move(status);
    }

    // Call once per frame while locked. `mainnet` only labels the keystore in the dialog
    // header; unlocking a mainnet keystore takes no extra typed confirmation (the config flag
    // that selected the network is the signal). `keystore_path` is read once, without a
    // passphrase, to name the account being unlocked and to warn about an agent nearing its
    // deadline. Returns true on the single frame the unlock succeeds.
    bool draw(bool mainnet, const std::string& keystore_path) noexcept;

    // True on the frame the user asks to wipe this keystore and start over. The caller owns
    // that decision -- it opens ResetDialog -- because deleting keystores is engine state,
    // not dialog state. Consuming the flag clears it.
    [[nodiscard]] bool take_reset_request() noexcept {
        const bool requested = reset_requested_;
        reset_requested_ = false;
        return requested;
    }

    // True once the user has dismissed the dialog without unlocking. The caller should stop
    // drawing it and run read-only; there is no way back other than restarting, which is
    // deliberate -- a dialog that can be reopened mid-session invites leaving it half-filled.
    [[nodiscard]] bool dismissed() const noexcept { return dismissed_; }

    // Zeroes the passphrase buffer immediately, e.g. if the caller wants to abandon the dialog
    // (window closed, app shutting down) without waiting for the destructor.
    void reset() noexcept;

    // Drops everything remembered about the keystore that *was* there: the cached cleartext
    // header, a stale error, and an earlier "Trade later". Called after the keystore files are
    // deleted, since the header cache is read once and would otherwise keep naming an account
    // whose file no longer exists, and a prior dismissal would suppress the prompt for the
    // keystore the user is about to create.
    void rearm() noexcept;

    ~UnlockDialog() { reset(); }

private:
    void try_unlock() noexcept;

    static constexpr size_t kPassCap = 256;    // matches pc_config::passphrase

    // Reads the keystore's cleartext header (pc_keystore_peek) the first time it is needed.
    // Cheap, but it is a file read, and draw() runs every frame -- so it happens once.
    void peek(const std::string& keystore_path) noexcept;

    char passphrase_[kPassCap]{};
    char error_[192]{};
    // Cleartext keystore header, for context above the passphrase box. `master_` is empty
    // and `valid_until_ms_` is 0 when the file could not be read -- in which case the dialog
    // simply shows no context rather than an alarming failure for a file the user may still
    // be able to unlock.
    char master_[64]{};
    uint64_t valid_until_ms_{0};
    bool peeked_{false};
    bool reset_requested_{false};
    // True between handing a passphrase to Submit and seeing a terminal PC_AUTH_* status.
    // Drives the "Unlocking..." state and blocks a second submission, which would just
    // contend for the 1 GiB Argon2id working buffer.
    bool pending_{false};
    bool unlocked_{false};
    bool dismissed_{false};
    Submit submit_{};
    Status status_{};
};

}  // namespace pc::ui
