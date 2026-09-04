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

---

## 5. Margin accounting — what `withdrawable` actually is, and what a resting order costs

Captured 2026-09-03 against mainnet `POST /info`, read-only, over ~470 public accounts drawn
from the public leaderboard (`https://stats-data.hyperliquid.xyz/Mainnet/leaderboard`). No
orders were placed. Each account was pulled as `clearinghouseState` + `frontendOpenOrders`
(+ `spotClearinghouseState` / `activeAssetData` where noted) back to back.

### 5.1 `withdrawable` floors effective leverage at 10x — it is NOT `accountValue - totalMarginUsed`

    withdrawable = accountValue - SUM_i positionValue_i / min(leverage_i, 10)

Exact on every account tested with a cross position above 10x and no resting orders — 7/7 in
a targeted scan at leverages 15/20/25/40, plus 4 found earlier:

| account | leverages | `withdrawable` | `AV - totalMarginUsed` | formula above |
|---|---|---|---|---|
| `0x30b16c4e` | 40, 10, 6 | 1,104.09 | 2,363.24 | **1,104.09** |
| `0x5e3b1ec0` | 20, 15, 20 | 2,002.10 | 15,570.86 | **2,002.10** |
| `0x7662b2c3` | 20 | 506.25 | 21,089.52 | **506.25** |
| `0x812ee6a9` | 40, 25, 10, 10 | 188.12 | 5,365.50 | **188.12** |
| `0x683ca63e` | 20, 10, 5, 10 | 2,317.94 | 2,899.97 | **2,317.94** |
| `0x8be2bc17` | 20, 10 | 70,604.87 | 71,106.23 | **70,604.87** |
| `0xbe494a5e` | 20, 10 | 211.76 | 211.97 | **211.76** |

`totalMarginUsed` is `positionValue / leverage` and tracks the leverage the position was
opened at; `withdrawable` charges the same position again at 10x whenever the account is
levered past that. At or below 10x the two coincide, which is why §03's original measurement
(a **10x** short) agreed and this was never visible.

### 5.2 The venue's own order capacity uses the real leverage

`activeAssetData.availableToTrade` on the **opening** side, against accounts with no resting
orders — exact to the cent, and it is the `AV - totalMarginUsed` column, not `withdrawable`:

| account | lev | venue `availableToTrade` | `withdrawable + idle` | `AV - totalMarginUsed + idle` |
|---|---|---|---|---|
| `0xefc1aaf1` | 15 | 22,682.78 | 16,181.63 | **22,682.80** |
| `0x553f4589` | 27 | 48,352.26 | 36,515.86 | **48,352.29** |
| `0x8fbd8f15` | 20 | 2.71 | 2.09 | **2.74** |

**Consequence for parsec:** `free_collateral()` is built on `withdrawable`, so it under-states
buying power by `SUM_i positionValue_i * (1/10 - 1/leverage_i)` for any position above 10x —
29% of the venue's own answer on `0xefc1aaf1`, 24% on `0x553f4589`. This is the same class of
bug the function was written to fix, one level down.

**Not yet safe to correct.** Adding that term back reproduces `availableToTrade` on accounts
with no resting orders, but on the two accounts found carrying both >10x leverage *and* live
orders the venue's figure sits *between* `withdrawable + idle` and the corrected value
(`0x725c9df2`: venue 1,737.09 vs 863.59 and 2,469.08; `0x815bbac9`: venue 79,658.99 vs
57,211.52 and 84,158.82). `availableToTrade` evidently nets open-order margin **per side**,
and that rule is not pinned down here. Fixing the 10x floor without it would trade a
conservative error for an unbounded one.

### 5.3 A resting non-reduce-only order that only SHRINKS a position costs nothing

Reading the order hold as `accountValue - SUM_i positionValue_i / min(leverage_i, 10) -
withdrawable` (§5.1), on accounts whose whole live order set is known:

- **`0x8469d67d`** — 1.12023 BTC cross short at 10x, one live order: a **non**-reduce-only
  resting BUY of 0.1 @ 79,234. Hold: **$0.00**. Gross would be $792.34.
- **`0x639c8d87`** — 96.95 HYPE long at 2x; 7 buys (opening) and 3 sells (reducing), none
  reduce-only. Hold **$1,750.1476**, which is the buy side alone, summed at each order's own
  limit price, over leverage — to four decimals. The three sells cost nothing.
- **`0xc984e1f0`** — 35,000 LIT short at 5x; one buy (reducing) 1,000 @ 4.0536, one sell
  (opening) 1,000 @ 4.9752. Hold **$995.04** = the sell alone. The buy's $810.72 is not charged.
- **`0x230ff1ab`**, **`0x5233199f`** — same shape across 3 and 5 coins; hold equals the opening
  side of every pair, to the cent.

Two further properties fall out of the same fits:

1. **Order margin is priced at the order's own limit price, not the mark.** `0x639c8d87`'s
   $1,750.1476 is exact at the limit prices (78.8–85.0); the mark would need to be 80.5
   against a mid of 85.496.
2. **It is the MAX of the two sides, not the sum.** `0xb7e09a94` quotes both sides on 7 coins
   while essentially flat: max-per-side fits its $1,159,964.115 hold with **zero** error and
   recovers plausible per-coin leverages — including PUMP at 10x, which its open position
   independently confirms. Summing both sides has no consistent solution.

parsec charges nothing for a reducing resting order (`panel_ticket.cpp`), which §5.3
confirms, and prices resting orders at `final_px`, which the first property confirms. It does
not model existing resting orders at all, so a ticket priced while other orders rest
over-states what is available by their hold.
