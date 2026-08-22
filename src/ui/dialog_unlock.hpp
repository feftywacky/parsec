#pragma once
// Passphrase-entry dialog for the local keystore (docs/06-security.md §2 onboarding, §4
// secrets-in-memory, §5 the enforceable rules). This component is deliberately self-contained
// and knows nothing about pc_engine_create/pc_config or the keystore file format -- there is
// currently no C++-visible keystore verification entry point (rust/src/signer/keystore.rs is
// Rust-only and nothing in include/parsec/parsec.h exposes "try this passphrase" as its own
// call; pc_engine_create takes a passphrase as part of construction instead). Wiring this
// dialog to something real is therefore one `set_verifier()` call away, made by whoever ends
// up owning the point in main.cpp / a future bootstrap step where the keystore path and
// pc_config are actually available -- not this panel's files (see this pass's report).
#include <cstddef>
#include <functional>

namespace pc::ui {

class UnlockDialog {
public:
    // Runs synchronously on the UI thread when the user submits. Returning false means "wrong
    // passphrase" (or any other verification failure) and should write a short, safe-to-display
    // message into err_out (never anything that could contain the passphrase itself).
    using Verifier = std::function<bool(const char* passphrase, char* err_out, size_t err_cap)>;

    void set_verifier(Verifier v) noexcept { verifier_ = std::move(v); }

    // Call once per frame while the keystore is locked. `mainnet` gates the extra typed
    // confirmation step (docs/06 §5.7: "mainnet requires two independent signals -- a config
    // flag and a typed confirmation at startup"). Returns true the one frame unlock succeeds;
    // the caller should stop calling draw() once it does.
    bool draw(bool mainnet) noexcept;

    // Zeroes the passphrase buffer immediately, e.g. if the caller wants to abandon the dialog
    // (window closed, app shutting down) without waiting for the destructor.
    void reset() noexcept;

    ~UnlockDialog() { reset(); }

private:
    void try_unlock() noexcept;

    static constexpr size_t kPassCap = 256;    // matches pc_config::passphrase
    static constexpr size_t kConfirmCap = 16;  // just needs to hold "MAINNET"

    char passphrase_[kPassCap]{};
    char mainnet_confirm_[kConfirmCap]{};
    char error_[192]{};
    bool unlocked_{false};
    Verifier verifier_{};
};

}  // namespace pc::ui
