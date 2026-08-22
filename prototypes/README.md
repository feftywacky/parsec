# Prototypes

Verified, runnable artifacts produced during design research. These are not the
implementation — they are proof that the two hardest-to-debug parts of the project already
work, and they are where Phases 0 and 3 should start.

## `signing-vectors/`

```
cargo run --release
```

Proves four things:

1. **Reproduces both official Hyperliquid signature test vectors** (mainnet `source="a"` and
   testnet `source="b"`) byte-for-byte, using `k256` + `sha3` + `rmp-serde` with no SDK.
2. **Cross-checks `action_hash` against the Python SDK** on a real order action —
   both produce `0xb8ed9e5a4ea7b2aebdb09fd794221c2599487f6dc8d3bf8f38656ab295f36120`.
3. **Demonstrates the msgpack key-ordering hazard.** Serializing the same action through
   `serde_json::Value` sorts keys alphabetically (`grouping, orders, ..., type`) and yields a
   completely different hash — `0x356ed851...` — which the venue would reject with a message
   about a nonexistent wallet. Use explicit structs whose declaration order is the wire order.
4. **Measures signing cost**: ~43 µs median for `action_hash` + sign on Apple Silicon.

`cross_check_python.py` is the other half of #2 — run it against the Python SDK to regenerate
the fixture:

```
python3 -m venv venv && ./venv/bin/pip install hyperliquid-python-sdk
./venv/bin/python cross_check_python.py
```

## `ffi-demo/`

Working CMake + Corrosion + tokio staticlib linked into a C++ binary, with callbacks firing
from tokio worker threads and clean shutdown. Start Phase 0 from this.

```
cmake -S . -B build && cmake --build build && ./build/app
```
