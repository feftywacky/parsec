# Parsec

A fast, minimal native trading terminal for Hyperliquid perpetuals.

C++20 trading core, Rust network/signing edge, Dear ImGui interface. One binary, no browser,
no runtime.

> **Status: foundation implemented; public market data only.** The application opens a native
> GLFW window and streams public market data through the Rust/C++ ABI. Authenticated order
> actions remain deliberately disabled until the agent-wallet flow and safety gates are complete.

## Build and run

```sh
cmake -S . -B build
cmake --build build
./build/parsec
# Use testnet explicitly when needed:
./build/parsec --testnet
```

Run the checks with `cargo test` from `rust/` and `ctest --test-dir build`.

## Documents

| # | Document | What it answers |
|---|---|---|
| 01 | [Overview](docs/01-overview.md) | scope, requirements, decisions, stack |
| 02 | [Architecture](docs/02-architecture.md) | process/thread topology, FFI contract, C++ modules |
| 03 | [Hyperliquid API](docs/03-hyperliquid-api.md) | verified exchange facts: REST, WebSocket, signing, limits |
| 04 | [Rust layer](docs/04-rust-layer.md) | transport, codec, signer, the C ABI header |
| 05 | [UI](docs/05-ui.md) | ImGui/ImPlot layout, chart and book rendering |
| 06 | [Security](docs/06-security.md) | key custody, agent wallets, keystore, threat model |
| 07 | [Implementation plan](docs/07-implementation-plan.md) | phased build order with acceptance criteria |
| 08 | [Testing](docs/08-testing.md) | unit, signing-vector, replay, and testnet strategy |

## Prototypes

Verified, runnable starting points produced during design research:

- `prototypes/signing-vectors/` — reproduces Hyperliquid's official signature test vectors
  byte-for-byte (`cargo run --release`).
- `prototypes/ffi-demo/` — working CMake + Corrosion + tokio staticlib linked into a C++ binary.

## Scope

Hyperliquid **perpetuals only**. Market and limit orders, leverage, TP/SL, cross and isolated
margin, live positions and portfolio, order book and candle charts. Mainnet is the default;
pass `--testnet` to use testnet. Select the active coin from the in-app coin picker.

Trading keys are **agent wallets** — they can trade but cannot withdraw. The master key is
used once, to approve the agent, and is never stored. See [`docs/06-security.md`](docs/06-security.md).
