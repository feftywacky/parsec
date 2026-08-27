# Parsec

A fast, minimal native trading terminal for Hyperliquid perpetuals.

C++20 trading core, Rust network/signing edge, Dear ImGui interface. One binary, no browser,
no runtime.

## Build and run

Requires a C++20 compiler and Rust (stable).

```sh
cmake -S . -B build
cmake --build build
./build/parsec              # mainnet by default
./build/parsec --testnet    # testnet
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

## Signing in

On first launch, parsec offers to **connect an account**. You give it your MetaMask private
key and choose a keystore passphrase. It generates a fresh agent wallet, uses your key for one
signature to approve that agent, zeroes the key, and writes `~/.parsec/keystore-<network>.json`
(mode 0600) holding only the encrypted agent key.

Your MetaMask key is never stored, logged, or passed as an argument. It exists in the
process only for that one signature. Hyperliquid has no usernames or passwords; an account
*is* a keypair, so that signature is the only proof the account is yours and the only way to
authorize the agent to trade for it.
</content>
