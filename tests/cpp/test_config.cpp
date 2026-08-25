#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "app/config.hpp"
#include "core/units.hpp"

using namespace pc;
using namespace pc::app;

namespace {

// RAII scratch file under the system temp directory, unique per test case (doctest's built-in
// current-test-name is the simplest unique-enough key -- these tests never run concurrently
// with each other within one process). Removed on destruction so a crashed run does not leave
// stale fixtures behind for the next one.
struct ScratchFile {
    std::filesystem::path path;

    explicit ScratchFile(const char* tag) {
        path = std::filesystem::temp_directory_path() /
               (std::string("parsec_test_config_") + tag + ".json");
        std::filesystem::remove(path);
    }
    ~ScratchFile() { std::filesystem::remove(path); }

    void write(const std::string& contents) const {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << contents;
    }
};

}  // namespace

TEST_CASE("Config::load: missing file falls back to defaults, never blocks startup") {
    ScratchFile scratch("missing");  // never written -- the file genuinely does not exist
    const Config cfg = Config::load(scratch.path.string());
    const Config defaults = Config::defaults();

    CHECK(cfg.mainnet == defaults.mainnet);
    CHECK(cfg.mainnet == true);
    CHECK(cfg.order_ack_timeout_ms == defaults.order_ack_timeout_ms);
}

TEST_CASE("Config: save/load round-trips every field") {
    ScratchFile scratch("roundtrip");

    Config cfg{};
    cfg.mainnet = true;
    cfg.keystore_path = "/home/tester/.parsec/keystore-mainnet.json";
    cfg.master_address = "0x1234567890abcdef1234567890abcdef12345678";
    cfg.order_ack_timeout_ms = 9'999;
    cfg.dms_heartbeat_interval_ms = 12'345;
    cfg.default_slippage_bps = 321;

    REQUIRE(cfg.save(scratch.path.string()));
    const Config loaded = Config::load(scratch.path.string());

    CHECK(loaded.mainnet == cfg.mainnet);
    CHECK(loaded.keystore_path == cfg.keystore_path);
    CHECK(loaded.master_address == cfg.master_address);
    CHECK(loaded.order_ack_timeout_ms == cfg.order_ack_timeout_ms);
    CHECK(loaded.dms_heartbeat_interval_ms == cfg.dms_heartbeat_interval_ms);
    CHECK(loaded.default_slippage_bps == cfg.default_slippage_bps);
}

TEST_CASE("Config::load: malformed JSON is handled without throwing, falls back to defaults") {
    ScratchFile scratch("malformed");
    scratch.write("{ \"mainnet\": true, \"keystore_path\": totally not json ");

    Config cfg{};
    CHECK_NOTHROW(cfg = Config::load(scratch.path.string()));
    CHECK(cfg.mainnet == Config::defaults().mainnet);
    CHECK(cfg.order_ack_timeout_ms == Config::defaults().order_ack_timeout_ms);
}

TEST_CASE("Config::load: a JSON value that isn't an object falls back to defaults") {
    ScratchFile scratch("not_object");
    scratch.write("[1, 2, 3]");

    Config cfg{};
    CHECK_NOTHROW(cfg = Config::load(scratch.path.string()));
    CHECK(cfg.mainnet == Config::defaults().mainnet);
}

TEST_CASE("Config::load: wrong-typed field falls back to that field's default, not a hard fail") {
    ScratchFile scratch("wrong_type");
    // `mainnet` should be a bool; a string here must not corrupt or crash the whole parse.
    scratch.write(R"({ "mainnet": "yes", "order_ack_timeout_ms": 4242 })");

    const Config cfg = Config::load(scratch.path.string());
    CHECK(cfg.mainnet == Config::defaults().mainnet);  // untouched, wrong type ignored
    CHECK(cfg.order_ack_timeout_ms == 4242);           // sibling field still parses correctly
}

