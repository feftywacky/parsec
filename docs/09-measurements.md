# Parsec — measurements from this deployment's vantage point

`07-implementation-plan.md` Phase 1 requires re-measuring the default `l2Book` cadence
locally, because "the 5.25 s figure in 03 came from a different network vantage point and the
design's staleness thresholds depend on it." This file is that record.

Everything below was captured with a raw RFC 6455 client against
`wss://api.hyperliquid-testnet.xyz/ws`, coin `BTC`, one connection per run, no client-side
filtering. `03-hyperliquid-api.md` §W3 remains the reference figures; where the two disagree,
**the numbers here govern parsec's thresholds** and the divergence is explained.

---

## 1. WebSocket feed cadence

| Feed | msgs | rate/s | bytes/msg | median gap | p95 gap | max gap | identical payloads |
|---|---|---|---|---|---|---|---|
| `l2Book` default (20 lv) | 13 | 0.21 | 1591 | **5.32 s** | 5.52 s | 5.52 s | 0 |
| `l2Book` `fast:true` (5 lv) | 343 | **1.90** | 467 | 0.515 s | 0.636 s | 1.098 s | 0 |
| `bbo` | 97–149 | **0.54–0.62** | 143 | 0.68–0.77 s | 5.3–7.8 s | 14.4 s | 0 |
| `activeAssetCtx` | 233 | 0.97 | 292 | 1.03 s | 1.18 s | 1.98 s | **57 / 233** |
| `trades` | 30 | 0.12 | 687 | 7.8 s | 16.6 s | 22.8 s | 0 |

Runs: 60 s, 240 s and 180 s captures. `l2Book` variants were separated by level count, since
both arrive on the same `channel` name and are otherwise indistinguishable.

### 1.1 The default `l2Book` cadence replicates — 5.32 s vs the doc's 5.25 s

Phase 1's open question is closed: the coalescing window is the same from here. Staleness
thresholds derived from the 5.25 s figure carry over unchanged, and §W6's "3x the feed's
expected cadence" rule stays at ~16 s for the default book.

### 1.2 `l2Book fast:true` also replicates — 1.90/s @ 467 B vs the doc's 1.8/s @ 470 B

Close enough to be the same measurement. The fast book is the right choice for the active
instrument's book panel exactly as designed.

### 1.3 `bbo` does NOT replicate, and this one matters

`03 §W3` records `bbo` at **7.4/s**; it measures **0.54–0.62/s** here — an order of magnitude
slower, with a p95 gap of 5–8 s and an observed max of 14.4 s.

This is not a routing artifact. `bbo` only emits when the touch actually changes, so its rate
is a function of how busy the book is, and **testnet BTC is far quieter than mainnet BTC**.
The doc's 7.4/s was a mainnet capture. Two consequences:

1. **Any staleness threshold hardcoded to a 7.4/s `bbo` will false-alarm continuously on
   testnet.** Thresholds must derive from an observed rolling cadence per feed, not from a
   compiled-in constant. This is implemented in `md/staleness`.
2. **On testnet the ordering in `02 §6.1` inverts.** The architecture designates `bbo` as
   "execution truth" because it is ~360x more touch-information-per-byte than the default
   `l2Book`. That reasoning is sound and still holds against the *default* book. But against
   `fast:true`, on testnet, `bbo` at 0.54/s is **3.5x slower** than the 1.90/s fast book —
   so the feed the design calls freshest is, here, the staler of the two.

   parsec keeps `bbo` as the execution feed, because the property that matters is that it is
   a *pure touch* feed and because the ordering reverses again on mainnet. But the ticket must
   display the **observed age** of the price it is about to trade on rather than assuming
   `bbo` is fresh — a 14 s stale touch is reachable on testnet and is exactly the case where a
   slippage estimate silently stops being trustworthy.

### 1.4 `activeAssetCtx` repeats byte-identical payloads as normal behaviour

54–57 of every ~233 messages are byte-identical to their predecessor, at a steady ~1/s.

`03 §W6` lists "repeated byte-identical payloads" as one of the four staleness signals. Applied
to `activeAssetCtx` it would fire roughly a quarter of the time on a perfectly healthy feed.
That signal is therefore scoped to the feeds where a repeat is genuinely anomalous, and
explicitly disabled for `activeAssetCtx`.

### 1.5 No legacy greeting frame observed

The non-JSON `"Websocket connection established."` frame that `04 §4` tells the WS manager to
tolerate did not appear on any run. The guard stays in — absence over five connections is not
evidence it cannot happen — but it is not on the normal path here.

---

## 2. REST `/info` — decimal precision on the wire

Captured from `POST https://api.hyperliquid-testnet.xyz/info` (`meta`, `metaAndAssetCtxs`,
`candleSnapshot`, `l2Book`) and saved as offline fixtures in `tests/fixtures/info/`.

Maximum fractional digits observed per field:

| Field | max decimals | |
|---|---|---|
| `funding`, `openInterest`, `dayNtlVlm`, `premium`, `dayBaseVlm` | **10** | exceeds 1e8 |
| `markPx`, `oraclePx`, `midPx`, `prevDayPx`, `impactPxs` | 6 | |
| `sz`, `v` | 5 | |
| `px`, candle `o`/`h`/`l`/`c` | 1 | |

### 2.1 This breaks the strict decimal parser

`codec/decimal.rs::parse_scaled` rejects anything with more than 8 fractional places
(`DecimalError::Precision`) rather than truncating — which is **correct and deliberate** for
prices and sizes, where silent truncation would change an order. `04 §4` calls for exactly
that behaviour.

But five *statistics* fields arrive with 10 decimals, and `transport/ws.rs::number()` maps the
resulting `Err` to `0`. Funding rate, open interest and 24-hour volume therefore parse as zero
— silently, with no error surfaced. The entire instrument strip described in `02 §7` would
render zeros.

The fix is not to relax `parse_scaled`. It is to split the two cases: strict parsing for
anything that can reach an order (`px`, `sz`), and an explicitly-named rounding parse for
statistics fields, which are display-only and whose 9th and 10th decimal places carry no
meaning at 1e8 scale.

`03 §W8`'s general rule — "assume every numeric-looking field is a decimal string" — is
correct but incomplete. The stronger rule: *assume every numeric-looking field can carry more
precision than your scale, and decide per field whether losing it is an error or a rounding.*

### 2.2 A second, larger bug found in the same place: `activeAssetCtx` is nested

Chasing the decimal issue on a live feed surfaced something bigger. The `activeAssetCtx`
payload is:

```json
{"channel":"activeAssetCtx","data":{"coin":"BTC","ctx":{"funding":"0.0000125",
  "openInterest":"57.31118","prevDayPx":"69188.0","dayNtlVlm":"2798900.0911900005",
  "premium":"0.0","oraclePx":"74552.5","markPx":"74756.0","midPx":"74793.0",
  "impactPxs":["74414.1","75802.5"],"dayBaseVlm":"38.81409"}}}
```

Every numeric lives one level down, under `ctx` — exactly as `03 §W2.12` documents. The
parser was reading them off `data` directly, so **every** field resolved to zero: mark,
oracle, mid and previous-day close as well as the five 10-decimal statistics. The entire
instrument strip was blank, and the decimal bug in §2.1 was only the half of it that had an
interesting explanation.

Both are fixed, and `rust/examples/verify_phase1.rs` now asserts against a live feed that
mark, funding, open interest and 24-hour volume all parse non-zero. Sample output:

```
mark          74709.0000
oracle        74493.0000
funding       0.0000125000
openInterest  57.311180
dayNtlVlm     2798944.9808
```

The general lesson is worth more than the fix: **a parse that silently yields zero is
indistinguishable from a market that happens to be quiet.** Neither bug produced an error,
a log line, or a crash — they produced plausible-looking zeros. Any field whose absence is
survivable should still be *loud* when it fails to parse, which is why the strict parser
returns an error and why the display-only rounding parser is a separate, deliberately-named
call rather than a relaxation of the strict one.

---

## 3. `meta` universe

210 perp assets on testnet. Entry keys observed: `name`, `szDecimals`, `maxLeverage`,
`marginTableId`, `onlyIsolated`, `isDelisted`, `marginMode`. The last three are absent from
most entries, so they must be optional in the deserializer.

`md::MarketStore`'s flat `kMaxAssets = 512` array comfortably covers this, and the dense asset
index from `meta`'s array position is the correct key — note that `l2Book` snapshots return 20
levels against `PC_MAX_LEVELS = 24`, so the header's capacity is not the binding constraint.

---

## 4. How to reproduce

The capture client is a dependency-free raw WebSocket implementation; no `websockets` or
`websocket-client` package is installed on this machine and none is needed. See
`tests/fixtures/info/` for the REST captures. Re-run the cadence measurement whenever the
deployment's network vantage point changes — per §1.3, the numbers are a property of the
*venue's activity as seen from here*, not of the venue alone.
