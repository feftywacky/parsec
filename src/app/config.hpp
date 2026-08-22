#pragma once
// Configuration layer (docs/07 Phase 3 file list, docs/02 §6.5: "all [risk limits] are config
// values"). Loaded once at startup from ~/.parsec/config.json. A missing or malformed file must
// never prevent parsec from starting in read-only market-data mode -- load() always returns a
// usable Config, falling back field-by-field to defaults() rather than failing the whole parse.
#include <cstdint>
#include <string>

#include "risk/pre_trade.hpp"

namespace pc::app {

struct Config {
    // -- network / identity --
    // Legacy persisted network preference. The terminal's CLI selection is authoritative; a
    // missing CLI override defaults to mainnet, while this field remains for config compatibility
    // and for the typed mainnet-session gate below.
    bool mainnet{true};
    std::string keystore_path;  // default depends on `mainnet`; see defaults()
    // 0x-prefixed hex, informational only (display + keystore header verification). Never a
    // secret -- the master key itself is never stored anywhere (docs/06 §2).
    std::string master_address;

    // -- risk limits (docs/02 §6.5) --
    risk::Limits limits{};

    // -- timers --
    uint64_t order_ack_timeout_ms{5'000};
    // How often the routine scheduleCancel heartbeat refreshes (risk::DeadMansSwitch's own
    // refresh/deadline/lead constants are the venue-mandated ones and are not user-tunable;
    // this is a separate, smaller "how chatty is the heartbeat" knob layered on top).
    uint64_t dms_heartbeat_interval_ms{10'000};

    // -- execution --
    // Matches exec::Rounder::kDefaultSlippageBps (500 = 5%, docs/02 §6.3) -- kept as a plain
    // int here rather than including exec/rounder.hpp, since config.hpp has no other reason to
    // depend on exec/.
    int32_t default_slippage_bps{500};

    [[nodiscard]] static Config defaults() noexcept;

    // Per-network keystore default used when the CLI selects a network explicitly.
    [[nodiscard]] static std::string default_keystore_path(bool mainnet) noexcept;

    // Loads from `path`. Any failure -- missing file, unreadable, malformed JSON, a field with
    // the wrong type -- is handled per-field: a bad or absent field falls back to its default
    // rather than aborting the whole load, and nothing here ever throws past this call.
    [[nodiscard]] static Config load(const std::string& path) noexcept;

    // ~/.parsec/config.json, or "./.parsec/config.json" if $HOME is unset (never crashes on a
    // missing environment).
    [[nodiscard]] static std::string default_path() noexcept;

    // Writes this config back out as JSON, creating ~/.parsec (mode 0700, matching the keystore
    // directory rule in docs/06 §5.4) if needed. Returns false on any I/O failure; never throws.
    [[nodiscard]] bool save(const std::string& path) const noexcept;
};

// Mainnet session gate (docs/06 §5.7 rule 7, docs/07 Phase 7 #5): authenticated mainnet use
// still requires two independent signals -- the resolved network flag and a typed confirmation.
// Public market data can open by default without a keystore or confirmation.
struct MainnetGate {
    // The exact string the user must type. Case-sensitive and exact-match on purpose: accepting
    // "mainnet" or a prefix would make the second signal a formality instead of a deliberate act.
    static constexpr const char* kRequiredConfirmation = "MAINNET";

    [[nodiscard]] static bool allowed(const Config& cfg, const char* typed_confirmation) noexcept;
};

}  // namespace pc::app
