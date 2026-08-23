#include <sys/resource.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <string>

#include "app/cli_setup.hpp"
#include "app/config.hpp"
#include "app/engine.hpp"
#include "ui/app_window.hpp"

namespace {

void print_usage() {
    std::fputs(
        "usage: parsec [--testnet] [--config=PATH]\n"
        "\n"
        "Runs against mainnet by default; pass --testnet for testnet. Signing in happens in\n"
        "the app: it prompts to connect an account on first run, and for the keystore\n"
        "passphrase after that.\n",
        stderr);
}

}  // namespace

namespace {

// docs/06 §4: a core dump would contain the agent key -- and, during `setup`, the master key.
// Debuggability is deliberately traded away for a secret that does not outlive the process.
// Set PARSEC_ALLOW_CORE=1 to opt back in when actually debugging a crash.
void suppress_core_dumps() {
    if (const char* allow = std::getenv("PARSEC_ALLOW_CORE"); allow != nullptr && allow[0] == '1')
        return;
    rlimit limit{0, 0};
    ::setrlimit(RLIMIT_CORE, &limit);  // best effort: nothing useful to do if it fails
}

}  // namespace

int main(int argc, char** argv) {
    // Before anything else, and before either CLI mode: `setup` handles the master key, and
    // the terminal holds the agent key for the whole session.
    suppress_core_dumps();

    // `parsec setup` is a distinct CLI mode (docs/06 §2), not a flag on the trading terminal --
    // dispatch before any of the terminal's own option parsing runs.
    if (argc > 1 && std::strcmp(argv[1], "setup") == 0)
        return pc::app::cli::run_setup(argc, argv);

    // Mainnet is the default; --testnet is the only network switch. The old
    // --confirm-mainnet handshake is gone: the typed MAINNET confirmation now lives in the
    // connect dialog, where it is asked at the moment it actually matters -- authorising an
    // agent against real funds -- rather than as a flag you have to know about before the app
    // will even offer to sign you in.
    bool want_mainnet = true;
    const char* config_path_override = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--testnet") == 0) {
            want_mainnet = false;
        } else if (std::strncmp(argv[i], "--config=", 9) == 0) {
            config_path_override = argv[i] + 9;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "parsec: unrecognized argument '%s'\n", argv[i]);
            print_usage();
            return 2;
        }
    }

    const std::string config_path =
        config_path_override ? config_path_override : pc::app::Config::default_path();
    pc::app::Config cfg = pc::app::Config::load(config_path);
    const bool using_default_keystore =
        cfg.keystore_path == pc::app::Config::default_keystore_path(true) ||
        cfg.keystore_path == pc::app::Config::default_keystore_path(false);

    // Network selection is an operational CLI choice: mainnet is the default and --testnet is
    // the explicit override. The keystore is always handed to the engine now -- withholding it
    // on mainnet only produced an app with no way to sign in. Mainnet's second signal (docs/06
    // §5.7) is the typed MAINNET confirmation in the connect dialog; a later unlock of an
    // already-approved keystore is passphrase-only, and shows the network in its header.
    cfg.mainnet = want_mainnet;
    if (using_default_keystore)
        cfg.keystore_path = pc::app::Config::default_keystore_path(want_mainnet);

    // Engine (and the market snapshot it holds) is heap-allocated deliberately: it embeds
    // several fixed-size 512-asset snapshots and is far too large to live on the default
    // thread stack (~700 MB — a stack-allocated Engine segfaults on startup).
    auto engine = std::make_unique<pc::app::Engine>(cfg);
    if (!engine->start()) {
        std::fputs("Could not start Parsec network engine.\n", stderr);
        return 1;
    }

    return pc::ui::AppWindow(*engine).run();
}
