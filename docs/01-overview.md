# Parsec — a fast, minimal Hyperliquid perps terminal

Native trading client for Hyperliquid perpetuals. C++ trading core, Rust network/signing
edge, Dear ImGui interface. One binary, no browser, no runtime.

---

## Document set

| # | Document | What it answers |
|---|---|---|
| 01 | **this file** | scope, requirements, decisions, stack |
| 02 | [`02-architecture.md`](02-architecture.md) | process/thread topology, FFI contract, C++ module design |
| 03 | [`03-hyperliquid-api.md`](03-hyperliquid-api.md) | verified exchange facts: endpoints, payloads, signing, limits |
| 04 | [`04-rust-layer.md`](04-rust-layer.md) | the Rust crate: transport, codec, signer, FFI implementation |
| 05 | [`05-ui.md`](05-ui.md) | ImGui layout, chart and book rendering, interactions |
| 06 | [`06-security.md`](06-security.md) | key custody, agent wallets, keystore format, threat model |
| 07 | [`07-implementation-plan.md`](07-implementation-plan.md) | phased build order, file-by-file, acceptance criteria |
| 08 | [`08-testing.md`](08-testing.md) | unit, signing-vector, replay, and testnet test strategy |

Build order is 07. Everything else is reference for it.

---

## Requirements → design

| # | Requirement | Where it is met |
|---|---|---|
| 1 | Trade Hyperliquid perps, majors first (BTC-USDC etc.) | `md::MarketStore` keyed by asset index from `meta`; the in-app coin picker exposes the live perp universe and subscribes the selected asset. Spot/vaults are out of scope (02 §9). |
| 2 | Market and limit orders, leverage, stop losses | `exec::OrderRouter` + `exec::Rounder`. Market = IOC at a slippage-adjusted price (the venue has no market type — 03). TP/SL via trigger orders, both standalone and attached to a parent (`grouping`). Leverage via `updateLeverage`. |
| 3 | Cross or isolated | Per-asset margin mode in the ticket; `updateLeverage{isCross}` and `updateIsolatedMargin`. Liquidation price math differs per mode — `portfolio/` models both. |
| 4 | Live positions and portfolio | `portfolio::PositionBook` + `AccountState`, fed by the `clearinghouseState` + `openOrders` WS subscriptions and reconciled against optimistic local fills (02 §6.4). `webData2` is dead — see 03 §W2.3. |
| 5 | Sign in via private key, stored safely | **Agent wallet + encrypted keystore** — see Decisions below and doc 06. |
| 6 | Order book and candle chart with timeframes | `ui/panel_book.cpp` over `md::L2Book`; `ui/panel_chart.cpp` over `md::CandleSeries`, backfilled by `candleSnapshot` and kept live by the `candle` subscription. |
| 7 | Match the reference UI | Layout in 02 §7 and 05; single ImGui dockspace, dark theme, same information density. |

---

## Decisions

**D1 — This repo becomes the trading client.** The existing `src/order_book/` (per-order
price levels with FIFO queues) is dropped. Hyperliquid publishes *aggregated L2 snapshots*;
there is no per-order feed to reconstruct, so a matching-engine-shaped book has no role in a
client. The one idea that carries forward is fixed-point units, generalised in
`core/units.hpp` (02 §2). The old code stays recoverable in git history.

**D2 — Agent wallet, encrypted keystore, passphrase at startup.** The MetaMask key is used
exactly once, to approve a Hyperliquid *agent wallet* (API wallet). Only the agent key is
stored, encrypted with argon2id + XChaCha20-Poly1305. Agent keys can place and cancel orders
but **cannot withdraw or transfer**, so total compromise of parsec costs you open orders, not
your balance. Details, file format, and threat model in doc 06.

**D3 — macOS and Linux from day one.** GLFW + OpenGL3 backend, one code path. This rules out
"macOS Keychain only" for key storage (hence the portable keystore in D2) and accepts
OpenGL's deprecated-but-functional status on macOS. A Metal backend is an additive change to
one file if it ever matters.

**D4 — C++ owns `main()`; Rust is a static library.** Not a sidecar process, not the other
way round. Rationale in 02 §1.

**D5 — Testnet is the default.** Mainnet requires an explicit config flag *and* a typed
confirmation at startup. Every phase in doc 07 is validated on testnet before mainnet.

**D6 — parsec implements its own Hyperliquid client rather than depending on
`hyperliquid_rust_sdk`.** The official SDK's `market_open` builds a fresh HTTP client and makes
a full `meta()` round trip *before every market order*, its book type allocates a nested `Vec`
plus two `String`s per level per tick, and its crates.io release still uses deprecated
ethers-rs. The signing core is ~120 lines and has been **implemented and verified against the
SDK's own test vectors** (`prototypes/signing-vectors/`). Full evidence and measurements in
doc 04 §1.

**D8 — v1 talks only to the public Hyperliquid endpoints.** REST `/info` and `/exchange`, plus
the public WebSocket. No non-validating node, no `order_book_server`, no third-party gateway.
Those exist and are documented in 03 §W7, but every one of them requires running node
infrastructure (~100 GB of logs per day and publicly reachable gossip ports) to buy depth and
tick resolution that a human-driven terminal cannot use. Evaluation in 02 §6.1.1.

**D7 — No `panic = "abort"` in the Rust release profile.** Measured: with `panic = "abort"`,
`catch_unwind` at the FFI boundary becomes inert and any Rust panic aborts the whole process —
taking down a C++ core that may be holding resting orders. Doc 04 §5.2.

---

## Stack

| Layer | Choice | Verified version |
|---|---|---|
| Core language | C++20, Apple clang 21 / gcc 13+ | `-Wall -Wextra -pedantic` |
| Network/crypto | Rust `staticlib`, tokio + **rustls** | rustc 1.96 — rustls drops all three macOS frameworks from the link line |
| Signing | `k256` + `sha3` + `rmp-serde` | 51 crates, 10 s builds, vectors verified |
| UI | Dear ImGui **docking branch** | `v1.92.9b-docking` (`1.93.0 WIP`) — docking is not in master |
| Charts | hand-rolled on `ImDrawList`, bar-index axis | no charting library — 05 §2.1 for why ImPlot could not express this |
| Windowing | GLFW + OpenGL3 backend | `3.5.1` |
| Build | CMake 4.1 + FetchContent + Corrosion | ✅ configures and builds end-to-end |
| Tests | doctest (C++), `cargo test` (Rust), replay harness | see 08 |

Measured build weight (macOS arm64): thin Rust layer = **180 crates, 44 s** clean release,
versus **411 crates, 2 m 10 s** for the official-SDK stack.

Already present locally: clang 21, cmake 4.1, rustc/cargo 1.96, glfw, nlohmann-json, boost,
openssl@3. Missing: ninja (optional); imgui/doctest are fetched by CMake.

### Verified prototypes in this repo

| Path | What it proves |
|---|---|
| `prototypes/signing-vectors/` | `cargo run --release` reproduces both official Hyperliquid signature test vectors byte-for-byte. **Start Phase 3 here.** |
| `prototypes/ffi-demo/` | Working CMake + Corrosion + tokio staticlib → C++ binary, with callbacks firing from tokio worker threads and clean shutdown. **Start Phase 0 here.** |

---

## What "fast and minimal" means here, concretely

These are the properties the design is accountable for, not aspirations:

- **No allocation on the market-data path.** From `pc_poll` to the seqlock publish, every
  buffer is fixed-size and preallocated.
- **No floating point in trading logic.** Prices and sizes are `int64` at 1e8 (02 §2).
- **No locks in the engine.** Engine state is single-threaded; the only cross-thread
  structures are one seqlock and two SPSC rings.
- **No string keys on hot paths.** Coins become dense `uint32_t` asset indices at the edge.
- **Idle cost near zero.** Event-driven redraw, blocking event drain with timeout — an
  open-but-untouched terminal should not spin a core.
- **Cold start to live book in under two seconds**, dominated by TLS handshake.
