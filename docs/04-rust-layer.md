# Parsec — the Rust layer

The network and crypto edge. Compiles to a `staticlib`, links into the C++ binary, exposes a
C ABI. Owns TLS, HTTP, WebSocket, JSON/msgpack codecs, and signing. Owns no trading logic.

---

## 1. Client library evaluation

**D6 — parsec implements its own Hyperliquid client in ~1500 lines of Rust.** Every
alternative was evaluated, including the Python SDK, and two of them were **measured**, not
argued about.

### 1.1 The Python SDK — tested, and it is the best of the existing options

`hyperliquid-python-sdk` 0.24.0 was installed and run. It is the official, most complete, and
best-maintained client, and it is the reference implementation this entire doc set was built
from. Results:

- ✅ **It reproduces our Rust signatures exactly** — both the mainnet and testnet vectors, and
  the `action_hash` of a real order action (`0xb8ed9e5a4ea7...`). Two independent
  implementations agreeing is the strongest correctness evidence available here.
- ⏱️ **Signing latency: median 3271 µs, p99 4095 µs.** The Rust path measured **43 µs median,
  52 µs p99** on the same machine — **~76x faster**.
- 📦 Import cost 498 ms; pulls `eth-account`, `ckzg`, `pycryptodome`, `websocket-client`.

**Latency is not the reason to reject it.** 3.3 ms is a few percent of a round trip to Tokyo,
and a human clicking Buy has a ~200 ms reaction time. Anyone claiming Python is "too slow" for
a manual terminal is hand-waving.

The real reasons are architectural, and they are about the *shape* of the process, not its
speed:

| | Consequence |
|---|---|
| **Embed CPython in the C++ binary** | A GIL and an unbounded GC pause inside the same process as a 60 fps render loop and a book that must never tear. ~30 MB runtime, no single-binary deploy, and the no-allocation discipline in 02 §1 becomes unenforceable. |
| **Run it as a sidecar process + IPC** | Works, and is the honest fallback. Costs an extra hop, a serialization format, process lifecycle management, and a second failure domain — for a component whose whole job is 1500 lines of msgpack and HTTP. |

Neither is fatal. But parsec's premise is a C++ core with no runtime, no GC, and POD structs
across a typed boundary. Bolting a Python interpreter to that trades away the thing being
built for a dependency we have already shown we can replace in a day.

> **Where the Python SDK earns its place: as a test oracle.** It has already caught nothing —
> because it *agreed* — and that agreement is exactly the value. `08-testing.md` §T1 makes
> cross-checking against it a standing CI-adjacent practice, not a one-off.

### 1.2 The official Rust SDK — measured, and disqualified

| | Official SDK stack | Thin layer (rustls) | Signing only |
|---|---|---|---|
| Crates in graph | **411** | **180** | 51 |
| Clean release build | **2 m 10 s** | **44 s** | 10.5 s |
| staticlib size | 21 MB | 18 MB | — |
| macOS link flags | `-framework Security -framework SystemConfiguration -framework CoreFoundation -liconv` | **none beyond `-lSystem -lc -lm`** | — |

Choosing **rustls over native-tls eliminates all three macOS frameworks** (verified via
`rustc --print native-static-libs`).

Build weight is an annoyance. These are the disqualifying findings:

- **`market_open` / `market_close` construct a brand-new `InfoClient` per call** — fresh
  `reqwest::Client`, fresh pool, fresh TLS handshake — and perform a **full `meta()` HTTP
  round trip before every market order.** `market_close` adds `user_state()` on top. One to
  two extra RTTs plus a handshake on the critical path of every market order.
- **`L2BookData` is `Vec<Vec<BookLevel>>` with `px`/`sz` as `String`** — a nested Vec plus two
  Strings per level, per tick. Directly incompatible with 02 §1.
- **Every action is serialized twice** — `rmp_serde` for the hash, then an allocating
  `serde_json::Value` tree for the wire.
- **`ExchangeClient::new` makes two blocking HTTP calls**, a hard network dependency at
  construction.
- **WS auto-reconnect is opt-in and naive**: `new` sets `reconnect = false`; even
  `with_reconnect` uses a **fixed 1 s sleep with no backoff** — the source comments admit it.
  Issues #94/#27 report the ws manager spinning on `Reader data not found`.

Maintenance: 0.6.0 (2025-05-16) is current on crates.io and still uses **`ethers 2.0.14`**,
itself deprecated in favour of alloy. The alloy migration landed on master 2025-07-28 and was
**never released**. No repo activity since 2025-10-21; 57 open issues, including features
parsec needs — TP/SL on orders (#149), `expiresAfter` (#128), cloid on modify (#136), order
grouping (#43), WS order submission (#131, #160).

Also surveyed: `hyperliquid` (dennohpeter, abandoned), `hyperliquid_rust_sdk-abrkn` (fork,
still ethers), `hyperliquid_rust_sdk_extended`, `hl_ranger`, `quicknode-hyperliquid-sdk` 0.1.9
(**the only 2026 release**; alloy + rustls + gRPC — revisit if the thin layer becomes a
burden), `hyperliquid-rs` (**GPL-3.0**, license-incompatible).

### 1.3 What we keep from both SDKs

The Python SDK is authoritative for wire format and is our signing oracle. From the Rust SDK,
consult but do not depend on:
- `src/exchange/actions.rs` — EIP-712 type strings for user-signed actions
- `src/helpers.rs` — `float_to_string_for_hashing`
- `src/exchange/order.rs` — single-letter field renames
- `src/signature/create_signature.rs` **tests** — our regression vectors

### 1.4 Honest summary of D6

If parsec were a Python tool, the Python SDK would be the correct and obvious choice. The
decision to write our own follows from the C++/ImGui/no-runtime architecture the project is
built on — and it is cheap precisely *because* the SDKs exist to check our work against. The
signing core is 120 lines and is already verified against two independent implementations.

---


## 2. Signing — verified working

The signing core is ~120 lines and has been **implemented and verified against the official
SDK's own test vectors**, reproducing both byte-for-byte:

```
mainnet=true  got=0xfa8a41f6...aec7841c  MATCH: true
mainnet=false got=0x1713c0fc...a881431c  MATCH: true
```

Runnable at `prototypes/signing-vectors/` — `cargo run --release`. **Start Phase 3 from this
file.** It converts the hardest-to-debug part of the project (03 §8: a bad signature reports a
nonexistent wallet and tells you nothing) into something already proven.

```toml
[dependencies]
k256       = { version = "0.13", default-features = false, features = ["ecdsa", "std"] }
sha3       = "0.10"
rmp-serde  = "1.3"
serde      = { version = "1", features = ["derive"] }
serde_json = "1"
hex        = "0.4"
```

```rust
fn keccak(data: &[u8]) -> [u8; 32] {
    let mut h = Keccak256::new(); h.update(data); h.finalize().into()
}

/// L1 domain: name="Exchange", version="1", chainId=1337, verifyingContract=0x0.
/// Constant — compute once into a OnceLock.
fn l1_domain_separator() -> [u8; 32] {
    let type_hash = keccak(
        b"EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)");
    let mut enc = Vec::with_capacity(160);
    enc.extend_from_slice(&type_hash);
    enc.extend_from_slice(&keccak(b"Exchange"));
    enc.extend_from_slice(&keccak(b"1"));
    enc.extend_from_slice(&word(&1337u64.to_be_bytes()));
    enc.extend_from_slice(&[0u8; 32]);              // address(0)
    keccak(&enc)
}

fn agent_struct_hash(source: &str, connection_id: &[u8; 32]) -> [u8; 32] {
    let type_hash = keccak(b"Agent(string source,bytes32 connectionId)");
    let mut enc = Vec::with_capacity(96);
    enc.extend_from_slice(&type_hash);
    enc.extend_from_slice(&keccak(source.as_bytes()));
    enc.extend_from_slice(connection_id);
    keccak(&enc)
}

fn eip712_digest(domain_sep: &[u8; 32], struct_hash: &[u8; 32]) -> [u8; 32] {
    let mut buf = [0u8; 66];
    buf[0] = 0x19; buf[1] = 0x01;
    buf[2..34].copy_from_slice(domain_sep);
    buf[34..66].copy_from_slice(struct_hash);
    keccak(&buf)
}

/// connectionId = keccak256( msgpack(action) || nonce_be_u64 || vault_flag [|| vault_addr]
///                           [|| 0x00 || expires_after_be_u64] )
pub fn action_hash<T: Serialize>(
    action: &T, nonce: u64, vault: Option<[u8; 20]>, expires_after: Option<u64>,
) -> [u8; 32] {
    let mut bytes = rmp_serde::to_vec_named(action).expect("msgpack");
    bytes.extend_from_slice(&nonce.to_be_bytes());
    match vault {
        Some(v) => { bytes.push(1); bytes.extend_from_slice(&v); }
        None    => bytes.push(0),
    }
    if let Some(e) = expires_after {
        bytes.push(0);                                  // separator, NOT a presence flag
        bytes.extend_from_slice(&e.to_be_bytes());
    }
    keccak(&bytes)
}

/// 65-byte r||s||v with v in {27,28}. k256 enforces low-s normalization.
pub fn sign_l1_action(sk: &SigningKey, connection_id: &[u8; 32], is_mainnet: bool) -> [u8; 65] {
    let source = if is_mainnet { "a" } else { "b" };
    let digest = eip712_digest(&l1_domain_separator(),
                               &agent_struct_hash(source, connection_id));
    let (sig, recid) = sk.sign_prehash_recoverable(&digest).expect("sign");
    let mut out = [0u8; 65];
    out[..64].copy_from_slice(&sig.to_bytes());
    out[64] = 27 + recid.to_byte();
    out
}
```

> The `expires_after` branch above is **not** in the verified prototype (which predates that
> requirement) — it is transcribed from 03 §4a. Extend the vector test to cover it before
> relying on it.

**Non-negotiable wire details**, all of which change the hash if violated:
- `rmp_serde::to_vec_named` — **named** msgpack maps, not positional.
- Field order is the struct's declaration order. Use explicit ordered structs, never
  `HashMap`/`serde_json::Value`. **This was measured, not assumed.** The same order action
  hashed two ways:

  | Serialization path | msgpack key order | `action_hash` |
  |---|---|---|
  | `serde_json::Value` | `grouping, orders, …, type` (alphabetical) | `0x356ed851…` ❌ |
  | explicit `#[derive(Serialize)]` structs | `type, orders, grouping` | `0xb8ed9e5a…` ✅ |

  `serde_json::Value` uses a `BTreeMap` and silently sorts keys. The resulting signature
  recovers a different address, and the venue replies `User or API Wallet 0x… does not exist` —
  a message that points nowhere near the actual bug.
- `f: false` and `a: false` must be **omitted**, not serialized — use
  `#[serde(skip_serializing_if = "is_false")]`.
- Numbers are strings with trailing zeros stripped, exactly:
```rust
pub const WIRE_DECIMALS: u8 = 8;
pub fn float_to_string_for_hashing(x: f64) -> String {
    let mut x = format!("{:.*}", WIRE_DECIMALS as usize, x);
    while x.ends_with('0') { x.pop(); }
    if x.ends_with('.') { x.pop(); }
    if x == "-0" { "0".to_string() } else { x }
}
```
  parsec's own values are already fixed-point `i64` at 1e8, so implement the integer
  equivalent (`i64` → decimal string, strip zeros) and **test it against this function's
  output** over a wide range. The float path exists only to compare against.
- Addresses lowercase before signing.
- `source` derived from the resolved base URL, then **asserted** against the configured
  network so a proxy cannot silently flip mainnet/testnet signing.

---

## 3. Crate manifest

```toml
[lib]
crate-type = ["staticlib"]      # add "cdylib" to support BUILD_SHARED_LIBS

[dependencies]
# signing
k256       = { version = "0.13", default-features = false, features = ["ecdsa","std"] }
sha3       = "0.10"
rmp-serde  = "1.3"
serde      = { version = "1", features = ["derive"] }
serde_json = "1"
hex        = "0.4"

# transport — rustls, NOT native-tls (drops all macOS frameworks)
tokio             = { version = "1", features = ["rt-multi-thread","net","time","sync","macros"] }
reqwest           = { version = "0.12", default-features = false, features = ["json","rustls-tls"] }
tokio-tungstenite = { version = "0.24", default-features = false,
                      features = ["rustls-tls-webpki-roots","connect"] }
futures-util      = "0.3"

# keystore (06)
argon2            = "0.5"
chacha20poly1305  = "0.10"
zeroize           = { version = "1", features = ["derive"] }
rand_core         = { version = "0.6", features = ["getrandom"] }

# fastAssetCtxs is raw DEFLATE (03 §W2.16)
flate2            = "1"

crossbeam-queue   = "0.3"       # ArrayQueue for the event ring
libc              = "0.2"       # mlock

[profile.release]
lto = "thin"
codegen-units = 1
# panic = "abort" is DELIBERATELY ABSENT — see §5.2
```

Alternative if you prefer alloy's primitives: use `alloy-primitives` + `alloy-sol-types` +
`alloy-signer-local` as **separate crates**, never the `alloy` meta-crate — the meta-crate
pulls `alloy-network`, `alloy-consensus`, `alloy-rpc-types-eth`, `alloy-trie`, `alloy-eip7702`
and the rest of an Ethereum stack parsec will never touch. That tree is most of the 411 crates.

---

## 4. Module layout

```
rust/src/
├── lib.rs
├── ffi/{mod,types,queue}.rs      # C ABI, catch_unwind wrappers, event ring
├── transport/
│   ├── http.rs                   # POST /info, POST /exchange; pooled, HTTP/2, pre-warmed
│   ├── ws.rs                     # subscribe/unsubscribe, 50s ping, reconnect+resubscribe
│   └── backoff.rs                # jittered exponential, 0.5s -> 30s
├── codec/
│   ├── info.rs                   # /info response models
│   ├── exchange.rs               # action structs, ORDERED, with skip_serializing_if
│   ├── ws_msg.rs                 # subscription + data message models
│   └── decimal.rs                # decimal-string <-> i64@1e8, exact, no float
├── signer/{l1,user_signed,keystore,agent}.rs
└── session.rs                    # nonce allocator, rate budget, master-vs-agent addressing
```

**`codec/decimal.rs` deserves disproportionate care.** Every price, size, and USD amount on
this venue is a decimal string (03 §W8), and it is the only place where wire data becomes
numbers. Requirements: exact, no float intermediate, rejects >8 decimal places rather than
truncating, handles `"0"`, `"-0"`, `"0.00001"`, and large integers. Fuzz it.

**WS manager responsibilities**, all learned from 03 Part II:
- 50 s ping timer; the 60 s idle clock is on **server→client** traffic, so ping regardless of
  outbound activity.
- Tear down after two unanswered pongs rather than waiting for the server close.
- Full resubscribe on reconnect — there is no session resumption.
- Dedupe the **30-trade backfill** that `trades` sends on every subscribe.
- Tolerate the legacy non-JSON greeting frame `"Websocket connection established."`.
- Track inflight `post` requests with a **client-side timeout** — a malformed request produces
  a bare `error` frame with **no `id`**, so an id-keyed map without timeouts leaks forever.
- Cap inflight posts below 100 (the limit is across *all* connections, not per-connection).

---

## 5. FFI

### 5.1 `include/parsec/parsec.h` — the contract

Hand-written, ~200 lines, not generated. cbindgen (0.29.4) is fine software but adds a codegen
step for a surface this small, and a generated header that silently changes layout is exactly
the failure mode a trading client cannot afford. Revisit if the surface passes ~30 types.

```c
#ifndef PARSEC_H
#define PARSEC_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PC_ABI_VERSION 1u
#define PC_COIN_LEN     16
#define PC_ERR_LEN     192
#define PC_MAX_LEVELS   24
#define PC_ASSET_NONE   0xFFFFFFFFu

typedef struct pc_engine pc_engine;
typedef uint64_t pc_req_id;

/* ---- scaled integers: price, size, usd are all x 1e8 ---- */
typedef int64_t pc_px;
typedef int64_t pc_qty;
typedef int64_t pc_usd;

/* ---- config ---- */
typedef struct {
    uint32_t abi_version;          /* must equal PC_ABI_VERSION */
    bool     mainnet;
    char     keystore_path[512];
    char     passphrase[256];      /* zeroed by pc_engine_create before it returns */
    uint32_t event_queue_capacity; /* default 8192 */
    uint32_t io_worker_threads;    /* default 2 */
} pc_config;

/* ---- streams ---- */
enum {
    PC_STREAM_BBO       = 1u << 0,
    PC_STREAM_L2        = 1u << 1,
    PC_STREAM_L2_FAST   = 1u << 2,
    PC_STREAM_TRADES    = 1u << 3,
    PC_STREAM_ASSET_CTX = 1u << 4,
    PC_STREAM_ASSET_DATA= 1u << 5,
    PC_STREAM_CANDLE    = 1u << 6,   /* interval in pc_subscribe's interval arg */
};

/* ---- events ---- */
enum {
    PC_EV_L2_BOOK = 1, PC_EV_BBO, PC_EV_TRADE, PC_EV_CANDLE, PC_EV_ASSET_CTX,
    PC_EV_ASSET_DATA, PC_EV_ORDER_UPDATE, PC_EV_FILL, PC_EV_POSITION, PC_EV_ACCOUNT,
    PC_EV_ORDER_ACK, PC_EV_CONN, PC_EV_RATE, PC_EV_ERROR,
};
enum {
    PC_F_SNAPSHOT       = 1u << 0,
    PC_F_SNAPSHOT_BEGIN = 1u << 1,
    PC_F_SNAPSHOT_END   = 1u << 2,
    PC_F_STALE          = 1u << 3,
    PC_F_CLOSED         = 1u << 4,   /* candle bucket closed */
};

typedef struct { pc_px px; pc_qty sz; uint32_t n; uint32_t _pad; } pc_level;

typedef struct {
    uint8_t  n_bid, n_ask; uint16_t _pad0; uint32_t _pad1;
    pc_level bids[PC_MAX_LEVELS];
    pc_level asks[PC_MAX_LEVELS];
} pc_l2;

typedef struct { pc_level bid, ask; bool has_bid, has_ask; } pc_bbo;
typedef struct { pc_px px; pc_qty sz; uint64_t tid; uint8_t is_buy; } pc_trade;
typedef struct {
    uint64_t open_ms, close_ms; pc_px o, h, l, c; pc_qty v;
    uint32_t n; uint8_t interval;
} pc_candle;
typedef struct {
    pc_px mark, oracle, mid, prev_day; pc_usd day_ntl_vlm, open_interest;
    int64_t funding_1e8;          /* hourly rate x 1e8 */
} pc_asset_ctx;
typedef struct {
    pc_qty max_trade_buy, max_trade_sell; pc_usd avail_buy, avail_sell;
    pc_px mark; uint32_t leverage; uint8_t is_cross;
} pc_asset_data;
typedef struct {
    uint64_t oid; uint8_t cloid[16]; uint16_t status; /* pc_order_status */
    pc_px px; pc_qty sz, orig_sz; uint8_t is_buy, reduce_only;
} pc_order_update;
typedef struct {
    uint64_t oid, tid; uint8_t cloid[16];
    pc_px px; pc_qty qty; pc_usd fee, closed_pnl;
    uint8_t is_buy, is_taker;
} pc_fill;
typedef struct {
    pc_qty szi;                    /* signed: >0 long, <0 short */
    pc_px entry_px, liq_px;        /* liq_px == 0 means none */
    pc_usd position_value, unrealized_pnl, margin_used, cum_funding;
    int32_t roe_bps; uint32_t leverage; uint8_t is_cross;
} pc_position;
typedef struct {
    pc_usd account_value, total_margin_used, total_ntl_pos, withdrawable,
           cross_maintenance_margin;
} pc_account;
typedef struct {
    uint16_t status;               /* pc_ack_status */
    uint64_t oid; pc_qty filled_sz; pc_px avg_px;
    char err[PC_ERR_LEN];
} pc_order_ack;
typedef struct { uint8_t socket, state; uint32_t reconnects; } pc_conn;
typedef struct { int64_t remaining; uint64_t reset_ms; } pc_rate;
typedef struct { int32_t code; char msg[PC_ERR_LEN]; } pc_error;

typedef struct {
    uint16_t kind;                 /* PC_EV_* */
    uint16_t flags;                /* PC_F_*  */
    uint32_t asset;                /* dense asset index, or PC_ASSET_NONE */
    pc_req_id req_id;              /* correlates to a command, else 0 */
    uint64_t exch_time_ms;         /* venue block clock */
    uint64_t recv_time_ns;         /* local monotonic, stamped on byte arrival */
    union {
        pc_l2           l2;
        pc_bbo          bbo;
        pc_trade        trade;
        pc_candle       candle;
        pc_asset_ctx    asset_ctx;
        pc_asset_data   asset_data;
        pc_order_update order_update;
        pc_fill         fill;
        pc_position     position;
        pc_account      account;
        pc_order_ack    ack;
        pc_conn         conn;
        pc_rate         rate;
        pc_error        error;
    } u;
} pc_event;

/* ---- order request ---- */
enum { PC_TIF_GTC = 0, PC_TIF_IOC, PC_TIF_ALO, PC_TIF_FRONTEND_MARKET };
enum { PC_TPSL_NONE = 0, PC_TPSL_TP, PC_TPSL_SL };
enum { PC_GROUP_NA = 0, PC_GROUP_NORMAL_TPSL, PC_GROUP_POSITION_TPSL };

typedef struct {
    uint32_t asset; uint8_t is_buy, reduce_only, tif, tpsl;
    pc_px    limit_px;             /* already rounded by exec::Rounder */
    pc_qty   sz;
    pc_px    trigger_px;           /* 0 when not a trigger order */
    uint8_t  is_market_trigger;
    uint8_t  cloid[16];
} pc_order;

typedef struct {
    uint8_t   grouping;            /* PC_GROUP_* */
    uint8_t   n_orders;            /* parent first, then children */
    pc_order  orders[4];
} pc_order_req;

/* ---- lifecycle ---- */
pc_engine* pc_engine_create(const pc_config* cfg);
void       pc_engine_destroy(pc_engine*);
uint32_t   pc_abi_version(void);
int32_t    pc_last_error(pc_engine*, char* out, uint32_t cap);

/* ---- drain: blocks up to timeout_ns, 0 = non-blocking. Returns event count. ---- */
int32_t    pc_poll(pc_engine*, pc_event* out, uint32_t max, uint64_t timeout_ns);

/* ---- commands: all non-blocking; outcome returns as an event with the same req_id ---- */
pc_req_id pc_subscribe      (pc_engine*, const char* coin, uint32_t mask, uint8_t interval);
pc_req_id pc_unsubscribe    (pc_engine*, const char* coin, uint32_t mask, uint8_t interval);
pc_req_id pc_place_order    (pc_engine*, const pc_order_req*);
pc_req_id pc_cancel_order   (pc_engine*, uint32_t asset, uint64_t oid);
pc_req_id pc_cancel_by_cloid(pc_engine*, uint32_t asset, const uint8_t cloid[16]);
pc_req_id pc_modify_order   (pc_engine*, uint64_t oid, const pc_order*);
pc_req_id pc_cancel_all     (pc_engine*);
pc_req_id pc_set_leverage   (pc_engine*, uint32_t asset, bool is_cross, uint32_t leverage);
pc_req_id pc_set_iso_margin (pc_engine*, uint32_t asset, pc_usd usd_delta);
pc_req_id pc_schedule_cancel(pc_engine*, uint64_t deadline_ms);
pc_req_id pc_fetch          (pc_engine*, uint32_t what, const char* coin);
pc_req_id pc_fetch_candle_snapshot(pc_engine*, const char* coin, uint8_t interval);

/* ---- asset metadata, resolved from `meta` at startup ---- */
int32_t   pc_asset_count(pc_engine*);
int32_t   pc_asset_info (pc_engine*, uint32_t asset, char* name_out, uint32_t cap,
                         uint8_t* sz_decimals, uint32_t* max_leverage, uint8_t* only_isolated);

#ifdef __cplusplus
}
#endif
#endif /* PARSEC_H */
```

Note the header carries **no field capable of holding key material** (06 §5).

### 5.2 Panic safety — measured, and the result is counterintuitive

Three configurations were tested empirically:

| Configuration | Result |
|---|---|
| `panic=unwind` + `catch_unwind` at the boundary | returns `-1`, **process survives**, exit 0 |
| `panic=unwind`, no `catch_unwind` in `extern "C"` | `panic in a function that cannot unwind` → **SIGABRT** |
| **`panic="abort"` + `catch_unwind`** | **SIGABRT — `catch_unwind` does not save you** |

> **Do not set `panic = "abort"`.** It looks like a reasonable size/speed optimization and it
> silently makes every `catch_unwind` inert, converting any Rust panic into an immediate
> process abort that takes the C++ core — and any resting orders' owner — down with it.

Since Rust 1.81 the compiler auto-inserts an abort shim at `extern "C"` boundaries, so an
unguarded panic is *always* fatal. Wrapping every entry point is mandatory, not defensive:

```rust
#[no_mangle]
pub extern "C" fn pc_place_order(eng: *mut PcEngine, req: *const PcOrderReq) -> u64 {
    catch_unwind(AssertUnwindSafe(|| {
        if eng.is_null() || req.is_null() { return 0; }
        unsafe { &*eng }.place_order(unsafe { &*req })
    })).unwrap_or(0)
}
```

### 5.3 Drain, not callbacks

Three transports were considered for the high-rate path:

1. **SPSC ring + drain function** — Rust writes POD frames into a preallocated ring;
   C++ calls `pc_poll` from its own loop. Zero callbacks, zero reentrancy hazard, zero
   steady-state allocation, and the C++ side controls when it does work. **This is what parsec
   uses.**
2. **Callback function pointers** — lowest latency, but the callback runs *on a tokio worker
   thread*. The C++ handler would have to be reentrancy-safe: no locks the main loop also
   holds, no allocation, no calls back into Rust that could deadlock. Reserved for nothing
   currently; the drain covers all cases because `pc_poll` blocks with a timeout and so has no
   polling-interval latency penalty.
3. Shared-memory SPSC across processes — pointless here, we are in one address space.

A working end-to-end demo (Rust tokio staticlib → C++ binary, callbacks firing from tokio
worker threads, clean shutdown) is preserved at `prototypes/ffi-demo/`.

### 5.4 CMake integration

```cmake
include(FetchContent)
FetchContent_Declare(Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.5.2)             # v0.6.1 is current; v0.5.2 verified working here
FetchContent_MakeAvailable(Corrosion)

corrosion_import_crate(
    MANIFEST_PATH ${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml
    CRATE_TYPES staticlib
    PROFILE release
    LOCKED)                     # CI must not silently bump the lockfile

target_link_libraries(parsec PRIVATE parsec_rs)   # target name == crate name
# With rustls there are NO extra macOS frameworks to link.
```

`corrosion_import_crate` also accepts `ALL_FEATURES`, `NO_DEFAULT_FEATURES`, `FEATURES`,
`FLAGS`, `FROZEN`, `IMPORTED_CRATES <var>`, `OVERRIDE_CRATE_TYPE`. Companions:
`corrosion_set_env_vars()`, `corrosion_add_target_local_rustflags()`,
`corrosion_link_libraries()`. Declaring both `staticlib` and `cdylib` makes the standard
`BUILD_SHARED_LIBS` switch work. Corrosion is MIT, healthy (last pushed 2026-05-16).

### 5.5 Crossing the boundary safely

- `#[repr(C)]`, `Copy`, fixed-size arrays only. No `String`, `Vec`, `Option<T>`, or
  enum-with-payload ever crosses.
- Opaque handles via `Box::into_raw` / `Box::from_raw`, paired with `pc_engine_destroy`. The
  tokio `Runtime` lives inside the boxed struct so dropping the handle shuts it down cleanly.
- Raw pointers are not `Send`; wrap in a newtype with an explicit `unsafe impl Send` when
  moving into a task, and document why it is sound.
- Null-check every pointer argument at the boundary; return an error code rather than trusting
  the caller.
- Layout assertions on both sides (02 §5.5), running in CI.
