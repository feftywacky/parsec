// `parsec setup` (docs/06 §2). The onboarding flow, in the order the master key's exposure
// window is minimized:
//
//   1. generate a fresh agent keypair                    -- needs pc_agent_generate
//   2. EITHER prompt for the master private key and sign  -- needs pc_agent_approve_with_master
//      locally, zeroing it the instant signing returns,
//      OR (--print-approval) print the digest to sign     -- needs pc_agent_approve_digest
//      offline and stop, OR (--signature) submit an        -- needs pc_agent_approve_with_sig
//      already-obtained signature -- the master key never touches this process in either of
//      the last two paths, which is the entire point of §2's "alternative for the paranoid".
//   3. prompt for a keystore passphrase (twice, must match)
//   4. seal + write ~/.parsec/keystore-<network>.json, mode 0600 -- needs pc_agent_seal_keystore
//
// None of the four `pc_agent_*` entry points above exist in include/parsec/parsec.h today.
// That header is frozen and out of scope for this pass, and rust/ is being actively changed by
// another agent concurrently -- inventing ABI additions here would be guessing at a contract
// someone else is mid-negotiation on. So: everything that does NOT require crossing the FFI
// boundary (argument parsing, prompting, passphrase confirmation, secure zeroing, the ordering
// of steps, never letting a secret touch a log line or a CLI argument) is fully implemented and
// exercised below. Every point that needs a Rust call stops with an explicit, named list of
// what is missing rather than fabricating a call that would silently do nothing or crash.
#include "app/cli_setup.hpp"

#include <termios.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace pc::app::cli {
namespace {

// Overwrites `buf` through a volatile pointer so the store cannot be optimized away as a dead
// write (docs/06 §4 measured exactly this happening with a plain `memset` under clang -O2).
void secure_zero(char* buf, size_t len) noexcept {
    volatile char* p = buf;
    for (size_t i = 0; i < len; ++i)
        p[i] = 0;
}

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
        "usage: parsec setup [--testnet|--mainnet] [--print-approval] [--signature=0x...] "
        "[--agent-name=NAME]\n"
        "\n"
        "  (no flags)          interactive: prompts for the MetaMask master private key,\n"
        "                      signs approveAgent locally, zeroes the key immediately.\n"
        "  --print-approval    prints the approveAgent EIP-712 digest and stops -- sign it\n"
        "                      offline (hardware wallet, air-gapped machine, MetaMask) and\n"
        "                      hand the signature back via --signature. The master key never\n"
        "                      touches this process in this path.\n"
        "  --signature=0x...   submits a signature obtained offline via --print-approval.\n"
        "  --testnet           target testnet instead of mainnet.\n",
        stdout);
}

// Named exactly for what is missing, not a generic "not implemented" -- see this file's header
// comment and the report from this pass for the intended signature of each.
void report_missing_abi() {
    std::fputs(
        "\nparsec setup cannot complete yet: the agent-wallet and keystore primitives that\n"
        "already exist in rust/src/signer/{agent,keystore}.rs are not reachable from C++ --\n"
        "include/parsec/parsec.h has no pc_agent_*/pc_keystore_* entry points. Needed:\n"
        "\n"
        "  pc_agent_generate(pc_engine*, uint8_t agent_addr_out[20])\n"
        "      -- wraps agent::generate()/derive_address(); the private key stays inside the\n"
        "         Rust engine handle (never crosses the FFI boundary, docs/06 §5 rule 1) until\n"
        "         one of the approve_* calls below consumes it.\n"
        "\n"
        "  pc_agent_approve_digest(pc_engine*, const char* agent_name, uint64_t "
        "valid_until_ms,\n"
        "                           uint8_t digest_out[32])\n"
        "      -- wraps ApproveAgentRequest::digest() for the --print-approval path.\n"
        "\n"
        "  pc_agent_approve_with_master(pc_engine*, const uint8_t master_sk[32],\n"
        "                                const char* agent_name, uint64_t valid_until_ms)\n"
        "      -- wraps ApproveAgentRequest::sign() + POST /exchange; zeroes master_sk\n"
        "         internally the instant signing returns.\n"
        "\n"
        "  pc_agent_approve_with_signature(pc_engine*, const uint8_t signature[65],\n"
        "                                   const char* agent_name, uint64_t valid_until_ms)\n"
        "      -- submits the approveAgent action with a signature obtained offline.\n"
        "\n"
        "  pc_agent_seal_keystore(pc_engine*, const char* passphrase, const char* out_path)\n"
        "      -- wraps keystore::Keystore::seal() for the just-approved agent key and writes\n"
        "         it to `out_path` with mode 0600.\n"
        "\n"
        "This binary already prompts for every input in the right order, never echoes a\n"
        "passphrase, and zeroes every secret buffer before returning -- wiring the calls above\n"
        "in is a small, mechanical change once they exist; nothing here needs to change shape.\n",
        stderr);
}

}  // namespace

int run_setup(int argc, char** argv) noexcept {
    bool print_approval = false;
    bool mainnet = true;
    std::string signature_hex;
    std::string agent_name;

    for (int i = 2; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--print-approval") {
            print_approval = true;
        } else if (a == "--mainnet") {
            mainnet = true;
        } else if (a == "--testnet") {
            mainnet = false;
        } else if (a.rfind("--signature=", 0) == 0) {
            signature_hex = std::string(a.substr(std::strlen("--signature=")));
        } else if (a.rfind("--agent-name=", 0) == 0) {
            agent_name = std::string(a.substr(std::strlen("--agent-name=")));
        } else if (a == "--help" || a == "-h") {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "parsec setup: unrecognized argument '%s'\n", argv[i]);
            print_usage();
            return 2;
        }
    }

    std::fprintf(stdout, "parsec setup -- network: %s\n", mainnet ? "mainnet" : "testnet");
    if (!agent_name.empty())
        std::fprintf(stdout, "agent name override: %s\n", agent_name.c_str());

    if (print_approval) {
        // Step 1 (pc_agent_generate) then step 2's digest (pc_agent_approve_digest) would run
        // here; the master key is never involved in this branch at all.
        std::fputs(
            "--print-approval: would generate a fresh agent key and print the\n"
            "approveAgent EIP-712 digest to sign offline.\n",
            stdout);
        report_missing_abi();
        return 3;
    }

    if (!signature_hex.empty()) {
        // Step 1 then submitting the caller-provided signature (pc_agent_approve_with_signature)
        // would run here. Still no master key touches this process.
        std::fprintf(stdout,
                     "--signature: would submit the provided signature (%zu hex chars) "
                     "via approveAgent.\n",
                     signature_hex.size());
        report_missing_abi();
        return 3;
    }

    // Interactive path (docs/06 §2 steps 1-5): the master key lives in this process for the
    // duration of one signature. It is read into a fixed stack buffer (never a CLI argument,
    // which would land in shell history and `ps`), never logged, and zeroed via secure_zero()
    // the moment it is no longer needed -- which today is immediately, since there is no ABI
    // call yet to actually sign with it.
    char master_key_hex[128]{};
    const bool got_master =
        read_secret_line("Master private key (hex, used once, never stored): ", master_key_hex,
                         sizeof(master_key_hex));
    secure_zero(master_key_hex, sizeof(master_key_hex));
    if (!got_master) {
        std::fputs("parsec setup: failed to read master private key\n", stderr);
        return 1;
    }

    // Step 6: keystore passphrase, twice, must match, never echoed, never logged.
    char pass1[256]{};
    char pass2[256]{};
    const bool ok1 = read_secret_line("Keystore passphrase: ", pass1, sizeof(pass1));
    const bool ok2 = ok1 && read_secret_line("Confirm keystore passphrase: ", pass2, sizeof(pass2));
    const bool match = ok1 && ok2 && std::strcmp(pass1, pass2) == 0;
    secure_zero(pass1, sizeof(pass1));
    secure_zero(pass2, sizeof(pass2));
    if (!match) {
        std::fputs("parsec setup: passphrases did not match (or could not be read)\n", stderr);
        return 1;
    }

    std::fputs(
        "\nMaster key and passphrase collected and handled correctly (never logged, "
        "zeroed after use).\n",
        stdout);
    report_missing_abi();
    return 3;
}

}  // namespace pc::app::cli
