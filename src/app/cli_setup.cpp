// `parsec setup` (docs/06 §2) -- the one-time flow that connects an account.
//
// parsec never stores the MetaMask master key. It approves an *agent wallet* (docs/06 §1),
// which can trade the account but cannot withdraw from it, and stores only that agent's key,
// encrypted. So this command's whole job is: generate an agent, get the master key to approve
// it exactly once, and seal the result.
//
// One path: prompt for the master key, sign in-process, zero it immediately. The key exists
// here for the duration of one signature and is never written, logged, or passed as an
// argument (where it would land in shell history and `ps` output).
//
// Every secret read here goes into a fixed caller-owned buffer, never a std::string (which
// offers no zeroing guarantee and may leave copies behind on reallocation), and is zeroed
// through a volatile pointer -- docs/06 §4 measured clang -O2 deleting a plain memset on a
// buffer about to go out of scope. The master-key and passphrase buffers are additionally
// zeroed by the Rust side before pc_setup_sign_with_master returns, so the secret dies even
// if this file's own zeroing were ever removed.
#include "app/cli_setup.hpp"

#include <termios.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "app/config.hpp"
#include "parsec/parsec.h"

namespace pc::app::cli {
namespace {

// Overwrites `buf` through a volatile pointer so the store cannot be optimized away as a dead
// write (docs/06 §4 measured exactly this happening with a plain `memset` under clang -O2).
void secure_zero(char* buf, size_t len) noexcept {
    volatile char* p = buf;
    for (size_t i = 0; i < len; ++i)
        p[i] = 0;
}

// RAII wipe for a secret buffer, so an early `return` on an error path cannot skip the zeroing.
// Every secret in this file is owned by one of these.
class ScopedSecret {
public:
    ScopedSecret(char* buf, size_t len) noexcept : buf_(buf), len_(len) {}
    ~ScopedSecret() { secure_zero(buf_, len_); }
    ScopedSecret(const ScopedSecret&) = delete;
    ScopedSecret& operator=(const ScopedSecret&) = delete;

private:
    char* buf_;
    size_t len_;
};

// Reads one line from stdin with terminal echo disabled, into a fixed caller-owned buffer --
// deliberately not std::string, which offers no zeroing guarantee and may reallocate/copy the
// secret to more than one heap location. Falls back to visible input only if stdin is not a
// TTY at all (e.g. piped in a test harness), since there is then no echo to suppress anyway.
bool read_secret_line(const char* prompt, char* out, size_t cap) noexcept {
    std::fputs(prompt, stdout);
    std::fflush(stdout);

    termios oldt{};
    const bool have_tty = ::isatty(STDIN_FILENO) != 0 && ::tcgetattr(STDIN_FILENO, &oldt) == 0;
    if (have_tty) {
        termios newt = oldt;
        newt.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        ::tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    }

    size_t n = 0;
    bool saw_any = false;
    while (n + 1 < cap) {
        char c{};
        const ssize_t r = ::read(STDIN_FILENO, &c, 1);
        if (r <= 0)
            break;
        saw_any = true;
        if (c == '\n')
            break;
        out[n++] = c;
    }
    out[n] = '\0';

    if (have_tty) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }
    return saw_any || n > 0;
}

void print_usage() {
    std::fputs(
        "usage: parsec setup [--testnet] [--agent-name=NAME] [--keystore=PATH]\n"
        "\n"
        "Approves an agent wallet and writes an encrypted keystore. Prompts for the MetaMask\n"
        "private key, uses it for one signature, and zeroes it immediately.\n"
        "\n"
        "Runs against mainnet by default; pass --testnet for testnet.\n"
        "\n"
        "You normally do not need this command -- just run `parsec` and it will offer to\n"
        "connect an account on first launch.\n"
        , stdout);
}

// Prints the handle's own error message. Never contains key material: the Rust side builds
// every one of these from fixed strings and non-secret values (docs/06 §5 rule 2).
void report(pc_setup* setup, const char* what) {
    char err[256]{};
    if (pc_setup_last_error(setup, err, sizeof(err)) > 0 && err[0] != '\0')
        std::fprintf(stderr, "parsec setup: %s: %s\n", what, err);
    else
        std::fprintf(stderr, "parsec setup: %s\n", what);
}

// Prompt for a passphrase twice, seal, and write. Split out because both approval paths
// (pasted master key, and signed-elsewhere) finish identically once the agent is approved.
int seal_and_write(pc_setup* setup, const std::string& keystore_path) {
    std::fputs(
        "\nChoose a passphrase for the local keystore. This encrypts the agent key on disk;\n"
        "it is asked for once per session and is never stored anywhere.\n",
        stdout);

    char pass1[256]{};
    char pass2[256]{};
    ScopedSecret wipe1(pass1, sizeof(pass1));
    ScopedSecret wipe2(pass2, sizeof(pass2));

    const bool ok1 = read_secret_line("Keystore passphrase: ", pass1, sizeof(pass1));
    const bool ok2 = ok1 && read_secret_line("Confirm keystore passphrase: ", pass2, sizeof(pass2));
    if (!ok1 || !ok2 || std::strcmp(pass1, pass2) != 0) {
        std::fputs("parsec setup: passphrases did not match (or could not be read)\n", stderr);
        return 1;
    }

    // Argon2id at the SENSITIVE tier (docs/06 §3) -- ~3.5s, deliberately. Say so, or it looks
    // like a hang.
    std::fputs("\nEncrypting keystore (Argon2id, ~3.5s)...\n", stdout);
    std::fflush(stdout);
    if (pc_setup_write_keystore(setup, pass1, keystore_path.c_str()) != 0) {
        report(setup, "could not write the keystore");
        std::fputs(
            "\nThe agent WAS approved at the venue even though the keystore was not written.\n"
            "Re-run `parsec setup` to approve a fresh agent -- the un-stored one is\n"
            "unreachable (its key existed only in this process) and expires on its own.\n",
            stderr);
        return 1;
    }

    char agent[PC_ADDR_STR_CAP]{};
    char master[PC_ADDR_STR_CAP]{};
    pc_setup_agent_address(setup, agent, sizeof(agent));
    pc_setup_master_address(setup, master, sizeof(master));
    std::fprintf(stdout,
                 "\nDone.\n"
                 "  account (master): %s\n"
                 "  agent (signs):    %s\n"
                 "  keystore:         %s\n"
                 "  agent expires:    %llu (epoch ms)\n"
                 "\n"
                 "The agent can trade this account but cannot withdraw from it. To revoke it,\n"
                 "approve a new agent with the same name -- from MetaMask on a phone if need\n"
                 "be; that is faster than getting back to a laptop.\n",
                 master, agent, keystore_path.c_str(),
                 static_cast<unsigned long long>(pc_setup_valid_until_ms(setup)));
    return 0;
}

}  // namespace

int run_setup(int argc, char** argv) noexcept {
    bool mainnet = true;
    std::string agent_name;
    std::string keystore_override;

    for (int i = 2; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--testnet") {
            mainnet = false;
        } else if (a.rfind("--agent-name=", 0) == 0) {
            agent_name = std::string(a.substr(std::strlen("--agent-name=")));
        } else if (a.rfind("--keystore=", 0) == 0) {
            keystore_override = std::string(a.substr(std::strlen("--keystore=")));
        } else if (a == "--help" || a == "-h") {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "parsec setup: unrecognized argument '%s'\n", argv[i]);
            print_usage();
            return 2;
        }
    }

    const std::string keystore_path =
        keystore_override.empty() ? Config::default_keystore_path(mainnet) : keystore_override;

    // Step 1: a fresh agent keypair, always. Its private key stays inside `setup` and is never
    // readable from here (docs/06 §5 rule 1).
    pc_setup* setup =
        pc_setup_begin(mainnet, agent_name.empty() ? nullptr : agent_name.c_str(), 0);
    if (setup == nullptr) {
        std::fputs("parsec setup: could not start onboarding\n", stderr);
        return 1;
    }
    struct Free {
        pc_setup* s;
        ~Free() { pc_setup_free(s); }
    } free_setup{setup};

    char agent_address[PC_ADDR_STR_CAP]{};
    char signed_name[192]{};
    pc_setup_agent_address(setup, agent_address, sizeof(agent_address));
    pc_setup_agent_name(setup, signed_name, sizeof(signed_name));

    std::fprintf(stdout,
                 "parsec setup -- network: %s\n"
                 "  new agent address: %s\n"
                 "  agent name:        %s\n"
                 "  keystore:          %s\n\n",
                 mainnet ? "mainnet" : "testnet", agent_address, signed_name,
                 keystore_path.c_str());

    // Steps 2-5 of docs/06 §2. The master key lives in this process for the duration of one
    // signature. Read into a fixed buffer -- never a CLI argument, where it would land in
    // shell history and `ps` output.
    {
        std::fputs(
            "The master private key is used once, to approve the agent, and is never stored.\n"
            "It is not echoed, not logged, and zeroed the instant the signature is produced.\n",
            stdout);
        char master_key_hex[160]{};
        ScopedSecret wipe(master_key_hex, sizeof(master_key_hex));
        if (!read_secret_line("Master private key (hex): ", master_key_hex,
                              sizeof(master_key_hex))) {
            std::fputs("parsec setup: failed to read master private key\n", stderr);
            return 1;
        }
        // Zeroes `master_key_hex` itself before returning, on success and on every failure.
        if (pc_setup_sign_with_master(setup, master_key_hex) != 0) {
            report(setup, "could not sign the approval");
            return 1;
        }
    }

    char master_address[PC_ADDR_STR_CAP]{};
    pc_setup_master_address(setup, master_address, sizeof(master_address));
    std::fprintf(stdout, "\nApproval signed by %s. Submitting...\n", master_address);
    std::fflush(stdout);

    if (pc_setup_submit(setup) != 0) {
        report(setup, "approveAgent was rejected");
        std::fputs(
            "\nNothing was stored. Common causes: the account has never deposited on this\n"
            "network, or the master key signed for the other network (mainnet vs testnet).\n",
            stderr);
        return 1;
    }
    std::fputs("Agent approved.\n", stdout);

    return seal_and_write(setup, keystore_path);
}

}  // namespace pc::app::cli
