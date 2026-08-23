#pragma once
// In-app account connection (docs/06 §2) -- the same agent-wallet onboarding as
// `parsec setup`, driven from the UI so a first run does not have to drop to a terminal.
//
// parsec never stores your MetaMask key. This flow generates a fresh agent wallet, has your
// master key sign one `approveAgent`, posts it, and writes an encrypted keystore holding only
// the agent key. The agent can trade the account but cannot withdraw from it.
//
// **Runs on a worker thread.** Every step here blocks: the approval is a network round trip
// and sealing the keystore is ~3.5s of Argon2id by design (docs/06 §3). Doing that inline
// would freeze the render loop, so the dialog starts a thread and polls it, exactly like
// UnlockDialog does for pc_unlock.
#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>

namespace pc::ui {

class ConnectDialog {
public:
    ConnectDialog() = default;
    ~ConnectDialog();
    ConnectDialog(const ConnectDialog&) = delete;
    ConnectDialog& operator=(const ConnectDialog&) = delete;

    // Call once per frame while the engine reports PC_AUTH_NO_KEYSTORE. Returns true on the
    // single frame the account is connected and the keystore has been written; the caller
    // should then point the engine at keystore_path() and unlock with take_passphrase().
    bool draw(bool mainnet, const std::string& keystore_path) noexcept;

    [[nodiscard]] bool dismissed() const noexcept { return dismissed_; }

    // Re-arms a dialog that was dismissed with "Later" or that already completed once, so a
    // reset can drop straight back into onboarding without restarting the app. Safe to call
    // on a fresh dialog; never called while a worker is running, since draw() only reports
    // Done after joining and reset is refused while an unlock is in flight.
    void reopen(const std::string& keystore_path) noexcept;
    [[nodiscard]] const std::string& keystore_path() const noexcept { return written_path_; }

    // Hands the just-chosen passphrase to the caller so the new keystore can be unlocked
    // without asking for it a second time. The buffer is the dialog's own and is zeroed as
    // soon as the caller returns it via done_with_passphrase().
    [[nodiscard]] char* passphrase_for_unlock() noexcept { return passphrase_; }
    void done_with_passphrase() noexcept;

private:
    enum class Stage { Form, Working, Failed, Done };

    void start() noexcept;

    static constexpr size_t kKeyCap = 160;   // 0x + 64 hex + slack
    static constexpr size_t kPassCap = 256;  // matches pc_config::passphrase

    char master_key_[kKeyCap]{};
    char passphrase_[kPassCap]{};
    char confirm_[kPassCap]{};
    char mainnet_confirm_[16]{};

    std::thread worker_{};
    // Written by the worker, read by the UI thread each frame.
    std::atomic<Stage> stage_{Stage::Form};
    std::mutex message_mutex_{};
    std::string message_{};  // progress line while working, error text when failed

    std::string written_path_{};
    bool mainnet_{};
    bool dismissed_{false};
};

}  // namespace pc::ui
