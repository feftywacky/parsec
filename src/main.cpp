#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "app/cli_setup.hpp"
#include "app/config.hpp"
#include "app/engine.hpp"
#include "ui/app_window.hpp"

namespace {

void print_usage() {
    std::fputs(
        "usage: parsec [--testnet|--mainnet] [--confirm-mainnet=MAINNET] "
        "[--config=PATH]\n"
        "       parsec setup [--testnet|--mainnet] [--print-approval] [--signature=0x...]\n",
        stderr);
}

}  // namespace

int main(int argc, char** argv) {
    // `parsec setup` is a distinct CLI mode (docs/06 §2), not a flag on the trading terminal --
    // dispatch before any of the terminal's own option parsing runs.
    if (argc > 1 && std::strcmp(argv[1], "setup") == 0)
        return pc::app::cli::run_setup(argc, argv);

    bool want_mainnet = true;
    const char* confirm_mainnet = "";
    const char* config_path_override = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mainnet") == 0) {
            want_mainnet = true;
        } else if (std::strcmp(argv[i], "--testnet") == 0) {
            want_mainnet = false;
        } else if (std::strncmp(argv[i], "--confirm-mainnet=", 18) == 0) {
            confirm_mainnet = argv[i] + 18;
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
    // the explicit override. The typed confirmation remains a separate safety signal for an
    // authenticated mainnet session; without it, public mainnet market data still opens but an
    // existing keystore is deliberately not handed to the engine.
    cfg.mainnet = want_mainnet;
    if (using_default_keystore)
        cfg.keystore_path = pc::app::Config::default_keystore_path(want_mainnet);
    if (want_mainnet && !pc::app::MainnetGate::allowed(cfg, confirm_mainnet))
        cfg.keystore_path.clear();

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
