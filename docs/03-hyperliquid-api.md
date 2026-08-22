# Hyperliquid API — verified reference

Ground truth: official docs (fetched as Markdown via `<page-url>.md`),
`hyperliquid-dex/hyperliquid-python-sdk@master`, and `nktkas/hyperliquid@main`
(schema-validated TS SDK). Doc index: `https://hyperliquid.gitbook.io/hyperliquid-docs/llms.txt`

Anything marked **UNVERIFIED** must be confirmed on testnet before it is relied on.

---

## 1. Endpoints & environments

Doc: `.../for-developers/api.md`, `.../api/websocket.md`

| | Mainnet | Testnet |
|---|---|---|
| REST base | `https://api.hyperliquid.xyz` | `https://api.hyperliquid-testnet.xyz` |
| Info | `POST /info` | `POST /info` |
| Exchange | `POST /exchange` | `POST /exchange` |
| Explorer | `POST /explorer` | `POST /explorer` |
| WebSocket | `wss://api.hyperliquid.xyz/ws` | `wss://api.hyperliquid-testnet.xyz/ws` |
| EVM JSON-RPC | `https://rpc.hyperliquid.xyz/evm` | — |

All requests are `POST` with `Content-Type: application/json`.

```python
MAINNET_API_URL = "https://api.hyperliquid.xyz"
TESTNET_API_URL = "https://api.hyperliquid-testnet.xyz"
LOCAL_API_URL   = "http://localhost:3001"
```

No separate regional/colocated API hosts are documented. The documented low-latency path is
running a non-validating node (`.../api/optimizing-latency.md`), not an alternate REST host.

**WebSocket as RPC transport** (`.../api/websocket/post-requests.md`) — any `/info` or
`/exchange` payload can be tunneled:

```json
{"method":"post","id":123,"request":{"type":"info"|"action","payload":{ }}}
```
```json
{"channel":"post","data":{"id":123,"response":{"type":"info"|"action"|"error","payload":{ }}}}
```
`method` and `id` are mandatory; `explorer` requests are not supported over WS.

---

## 2. INFO endpoint (`POST /info`)

Docs: `.../api/info-endpoint.md`, `.../api/info-endpoint/perpetuals.md`

**Global rules.** Time-ranged responses return at most 500 elements or distinct blocks —
paginate using the last returned timestamp as the next `startTime`. **`user` must be the
master/sub-account address, never the agent wallet address** — the #1 cause of empty results.

Coin naming: perps use `meta.universe[].name` (`"BTC"`); HIP-3 perps use `"{dex}:{coin}"`;
spot uses `"PURR/USDC"` or `"@{index}"`.

### `meta` — perp universe

```json
{"type":"meta","dex":""}
```
```json
{
  "universe": [
    {"name":"BTC","szDecimals":5,"maxLeverage":50},
    {"name":"ETH","szDecimals":4,"maxLeverage":50},
    {"name":"HPOS","szDecimals":0,"maxLeverage":3,"onlyIsolated":true},
    {"name":"LOOM","szDecimals":1,"maxLeverage":3,"isDelisted":true,
     "marginMode":"strictIsolated","onlyIsolated":true}
  ],
  "marginTables": [
    [50, {"description":"","marginTiers":[{"lowerBound":"0.0","maxLeverage":50}]}],
    [51, {"description":"tiered 10x","marginTiers":[
        {"lowerBound":"0.0","maxLeverage":10},
        {"lowerBound":"3000000.0","maxLeverage":5}]}]
  ]
}
```
`dex` defaults to `""`. **The array index into `universe` is the asset ID.**
`marginMode: "strictIsolated"` = margin cannot be removed; `"noCross"` = isolated only.
`onlyIsolated` is deprecated (means either of the two).

### `metaAndAssetCtxs`

```json
{"type":"metaAndAssetCtxs","dex":""}
```
Returns a 2-tuple `[meta, ctxs]` where `ctxs[i]` corresponds to `universe[i]`:
```json
[
  {"universe":[],"marginTables":[],"collateralToken":0},
  [
    {"dayNtlVlm":"1169046.29406","funding":"0.0000125",
     "impactPxs":["14.3047","14.3444"],"markPx":"14.3161","midPx":"14.314",
     "openInterest":"688.11","oraclePx":"14.32","premium":"0.00031774",
     "prevDayPx":"15.322"}
  ]
]
```
All numerics are strings. `funding` is the **hourly** rate. `openInterest` is in base units.
`premium` may be `null`.

### `allMids`

```json
{"type":"allMids","dex":""}
```
```json
{"APE":"4.33245","ARB":"1.21695"}
```
Falls back to last trade price if the book is empty.

### `l2Book`

```json
{"type":"l2Book","coin":"BTC","nSigFigs":5,"mantissa":null}
```
`nSigFigs`: `2 | 3 | 4 | 5 | null` (null = full precision). `mantissa`: `1 | 2 | 5`, **only
allowed when `nSigFigs == 5`**. Max 20 levels per side.
```json
{
  "coin":"BTC","time":1754450974231,
  "levels":[
    [{"px":"113377.0","sz":"7.6699","n":17},{"px":"113376.0","sz":"4.13714","n":8}],
    [{"px":"113397.0","sz":"0.11543","n":3}]
  ]
}
```
`levels[0]` = bids (descending), `levels[1]` = asks (ascending). `n` is the count of resting
orders aggregated at that price.

### `candleSnapshot`

```json
{"type":"candleSnapshot","req":{"coin":"BTC","interval":"15m",
  "startTime":1681923600000,"endTime":1681924499999}}
```
Intervals (verbatim): `"1m","3m","5m","15m","30m","1h","2h","4h","8h","12h","1d","3d","1w","1M"`.
**Only the most recent 5000 candles are available.**
```json
[{"T":1681924499999,"c":"29258.0","h":"29309.0","i":"15m","l":"29250.0",
  "n":189,"o":"29295.0","s":"BTC","t":1681923600000,"v":"0.98639"}]
```
`t` = open ms, `T` = close ms, `s` = symbol, `i` = interval, `o/c/h/l` = OHLC strings,
`v` = base volume string, `n` = trade count (int).

### `clearinghouseState`

```json
{"type":"clearinghouseState","user":"0x...","dex":""}
```
```json
{
  "assetPositions":[
    {"position":{
       "coin":"ETH",
       "cumFunding":{"allTime":"514.085417","sinceChange":"0.0","sinceOpen":"0.0"},
       "entryPx":"2986.3",
       "leverage":{"rawUsd":"-95.059824","type":"isolated","value":20},
       "liquidationPx":"2866.26936529",
       "marginUsed":"4.967826",
       "maxLeverage":50,
       "positionValue":"100.02765",
       "returnOnEquity":"-0.0026789",
       "szi":"0.0335",
       "unrealizedPnl":"-0.0134"},
     "type":"oneWay"}
  ],
  "crossMaintenanceMarginUsed":"0.0",
  "crossMarginSummary":{"accountValue":"13104.514502","totalMarginUsed":"0.0",
                        "totalNtlPos":"0.0","totalRawUsd":"13104.514502"},
  "marginSummary":{"accountValue":"13109.482328","totalMarginUsed":"4.967826",
                   "totalNtlPos":"100.02765","totalRawUsd":"13009.454678"},
  "time":1708622398623,
  "withdrawable":"13104.514502"
}
```
`leverage.type` is `"cross"` or `"isolated"`; **`rawUsd` is present only for isolated**.
`liquidationPx` may be `null`. `szi` is signed (negative = short). `marginSummary` covers
cross + isolated; `crossMarginSummary` covers cross only.

### `openOrders` / `frontendOpenOrders`

```json
{"type":"openOrders","user":"0x...","dex":""}
```
```json
[{"coin":"BTC","limitPx":"29792.0","oid":91490942,"side":"A","sz":"0.0",
  "timestamp":1681247412573}]
```
```json
{"type":"frontendOpenOrders","user":"0x...","dex":""}
```
```json
[{"coin":"BTC","isPositionTpsl":false,"isTrigger":false,"limitPx":"29792.0",
  "oid":91490942,"orderType":"Limit","origSz":"5.0","reduceOnly":false,
  "side":"A","sz":"5.0","timestamp":1681247412573,
  "triggerCondition":"N/A","triggerPx":"0.0"}]
```
`side`: `"B"` = bid/buy, `"A"` = ask/sell.

### `userFills` / `userFillsByTime`

```json
{"type":"userFills","user":"0x...","aggregateByTime":false}
{"type":"userFillsByTime","user":"0x...","startTime":1681222254710,
 "endTime":1681322254710,"aggregateByTime":false}
```
`userFills` returns at most 2000 most recent. `userFillsByTime` returns at most 2000 per
response, only the 10000 most recent are available; times are inclusive ms.
```json
[{"closedPnl":"0.0","coin":"AVAX","crossed":false,"dir":"Open Long",
  "hash":"0xa166e3fa...","oid":90542681,"px":"18.435","side":"B",
  "startPosition":"26.86","sz":"93.53","time":1681222254710,
  "fee":"0.01","feeToken":"USDC","builderFee":"0.01","tid":118906512037719}]
```
`fee` is **inclusive** of `builderFee`. `crossed=true` means the fill was taker. `tid` is the
unique trade id.

### `userFunding`

```json
{"type":"userFunding","user":"0x...","startTime":1681222254710,"endTime":1681322254710}
```
```json
[{"delta":{"coin":"ETH","fundingRate":"0.0000417","szi":"49.1477","type":"funding",
           "usdc":"-3.625312","nSamples":null},
  "hash":"0xa166...","time":1681222254710}]
```

### `historicalOrders`

`{"type":"historicalOrders","user":"0x..."}` — at most 2000 most recent, elements shaped like
`orderStatus.order` below.

### `orderStatus`

```json
{"type":"orderStatus","user":"0x...","oid":91490942}
```
`oid` is a `uint64` **or** a 16-byte hex cloid string.
```json
{"status":"order",
 "order":{"order":{"coin":"ETH","side":"A","limitPx":"2412.7","sz":"0.0","oid":1,
                   "timestamp":1724361546645,"triggerCondition":"N/A","isTrigger":false,
                   "triggerPx":"0.0","children":[],"isPositionTpsl":false,
                   "reduceOnly":true,"orderType":"Market","origSz":"0.0076",
                   "tif":"FrontendMarket","cloid":null},
          "status":"filled","statusTimestamp":1724361546645}}
```
Missing order: `{"status":"unknownOid"}`

**Full order-status enum** (drive the C++ `OrderStatus` mapping off this list):
`open`, `filled`, `canceled`, `triggered`, `rejected`, `marginCanceled`,
`vaultWithdrawalCanceled`, `openInterestCapCanceled`, `selfTradeCanceled`,
`reduceOnlyCanceled`, `siblingFilledCanceled`, `delistedCanceled`, `liquidatedCanceled`,
`scheduledCancel`, `tickRejected`, `minTradeNtlRejected`, `perpMarginRejected`,
`reduceOnlyRejected`, `badAloPxRejected`, `iocCancelRejected`, `badTriggerPxRejected`,
`marketOrderNoLiquidityRejected`, `positionIncreaseAtOpenInterestCapRejected`,
`positionFlipAtOpenInterestCapRejected`, `tooAggressiveAtOpenInterestCapRejected`,
`openInterestIncreaseRejected`, `insufficientSpotBalanceRejected`, `oracleRejected`,
`perpMaxPositionRejected`.

### `userRateLimit`

```json
{"type":"userRateLimit","user":"0x..."}
```
```json
{"cumVlm":"2854574.593578","nRequestsUsed":2890,"nRequestsCap":2864574,"nRequestsSurplus":0}
```

### `activeAssetData` — per-coin leverage + max sizes

```json
{"type":"activeAssetData","user":"0x...","coin":"APT"}
```
```json
{"user":"0xb658...","coin":"APT","leverage":{"type":"cross","value":3},
 "maxTradeSzs":["24836370.44","24836370.44"],
 "availableToTrade":["37019438.03","37019438.03"],"markPx":"4.4716"}
```
Arrays are `[buy, sell]`. **This is the direct source for the ticket's "Available to Trade"
and max-size slider.**

### Asset ID encoding — critical

Doc: `.../api/asset-ids.md`

| Class | Asset ID |
|---|---|
| Canonical perp | index in `meta.universe` (BTC = 0 mainnet) |
| Builder perp (HIP-3) | `100000 + perp_dex_index * 10000 + index_in_meta` |
| Spot | `10000 + spotMeta.universe[i].index` |
| Outcome | `100000000 + (10 * outcome + side)`, side in {0,1} |

Spot ID is not the token ID, and **mainnet ids differ from testnet** (HYPE: mainnet token 150
/ spot 107; testnet token 1105 / spot 1035). Never hardcode an asset id — always resolve from
`meta` at startup.

---

## 3. EXCHANGE endpoint (`POST /exchange`)

Doc: `.../api/exchange-endpoint.md`

Envelope for every action:
```json
{"action":{},"nonce":1716531066415,"signature":{"r":"0x..","s":"0x..","v":27},
 "vaultAddress":null,"expiresAfter":null}
```
`vaultAddress` — 42-char lowercase hex when acting for a vault or sub-account.
`expiresAfter` — ms timestamp after which the action is rejected; **not supported on
user-signed actions**; actions canceled for staleness consume **5x** the address rate limit.

### `order`

```json
{
  "action": {
    "type": "order",
    "orders": [{
      "a": 0, "b": true, "p": "1900.5", "s": "0.1", "r": false,
      "t": {"limit": {"tif": "Gtc"}},
      "c": "0x1234567890abcdef1234567890abcdef"
    }],
    "grouping": "na",
    "builder": {"b": "0x...", "f": 10}
  },
  "nonce": 1716531066415,
  "signature": {"r":"0x..","s":"0x..","v":27}
}
```
`a`=asset, `b`=isBuy, `p`=limit price (string), `s`=size (string), `r`=reduceOnly,
`t`=order type, `c`=cloid (optional).

**Order type `t`** is a one-of:
```json
{"limit": {"tif": "Alo" | "Ioc" | "Gtc" | "FrontendMarket"}}
{"trigger": {"isMarket": true, "triggerPx": "2600", "tpsl": "tp" | "sl"}}
```
- `Alo` — post-only; canceled instead of immediately matching.
- `Ioc` — unfilled part canceled instead of resting.
- `Gtc` — no special behaviour.
- `FrontendMarket` — accepted input TIF, "similar to Ioc but add a note that this is a market
  order". Present in `nktkas` as `picklist(["Gtc","Ioc","Alo","FrontendMarket"])` and in
  `orderStatus.order.tif`, but **not** in the official exchange-endpoint TIF table.
- `LiquidationMarket` — **UNVERIFIED**; not in docs, Python SDK, or the TS picklist.

For a trigger order, `p` is the limit price used *after* trigger (ignored in effect when
`isMarket: true`, but still required and still tick-validated); `triggerPx` is the activation
price.

**`c` (cloid)** — 128-bit hex, exactly `0x` + 32 lowercase hex chars, `^0x[a-fA-F0-9]{32}$`.

**`builder`** — `b` = fee recipient (must be lowercased before signing), `f` = fee in **tenths
of a basis point** (`f=10` → 1 bp). Max 100 for perps.

**`grouping`**:
| Value | Meaning |
|---|---|
| `"na"` | independent orders (default) |
| `"normalTpsl"` | parent + child TP/SL, fixed size, does not track position changes |
| `"positionTpsl"` | TP/SL that adjusts proportionally with position size |
| `{"p": <uint <= 1e8>}` | priority rate `p/1e8`; only when all orders are IOC, or all are non-reduce-only ALO |

**TP/SL attached to a parent** — first element is the parent, the rest are children,
submitted atomically:
```json
{"action":{"type":"order","grouping":"normalTpsl","orders":[
  {"a":1,"b":true,"p":"3500","s":"0.02","r":false,"t":{"limit":{"tif":"Gtc"}}},
  {"a":1,"b":false,"p":"3500","s":"0.02","r":true,
   "t":{"trigger":{"isMarket":true,"triggerPx":"3500","tpsl":"tp"}}},
  {"a":1,"b":false,"p":"2500","s":"0.02","r":true,
   "t":{"trigger":{"isMarket":true,"triggerPx":"2600","tpsl":"sl"}}}
]},"nonce":1716531066415,"signature":{}}
```
Children must be `r: true`, opposite side of the parent, same size. Non-reduce-only TP/SL is
rejected in **pre-validation** (one error for the whole batch). When one child fills, the
sibling is canceled with status `siblingFilledCanceled`. Children appear in
`orderStatus.order.children`; `positionTpsl` children carry `isPositionTpsl: true`.

**Responses:**
```json
{"status":"ok","response":{"type":"order","data":{"statuses":[{"resting":{"oid":77738308}}]}}}
{"status":"ok","response":{"type":"order","data":{"statuses":[
  {"filled":{"totalSz":"0.02","avgPx":"1891.4","oid":77747314}}]}}}
{"status":"ok","response":{"type":"order","data":{"statuses":[
  {"error":"Order must have minimum value of $10."}]}}}
```
`resting` and `filled` also carry `cloid` when one was supplied. Two additional bare-string
statuses exist: `"waitingForFill"` and `"waitingForTrigger"`.

### `cancel`

```json
{"action":{"type":"cancel","cancels":[{"a":0,"o":77738308}],"f":true},
 "nonce":1716531066415,"signature":{}}
```
```json
{"status":"ok","response":{"type":"cancel","data":{"statuses":["success"]}}}
{"status":"ok","response":{"type":"cancel","data":{"statuses":[
  {"error":"Order was never placed, already canceled, or filled."}]}}}
```
**`f` (fast)** — rejects if it refers to trigger orders; a future upgrade prioritizes fast
cancels in the mempool. **`f` must be omitted entirely if false** — actions hashed with
`f: false` are rejected. The latency doc recommends always sending `f: true` for non-trigger
cancels.

### `cancelByCloid`

```json
{"action":{"type":"cancelByCloid","cancels":[
  {"asset":0,"cloid":"0x1234567890abcdef1234567890abcdef"}],"f":true},
 "nonce":1716531066415,"signature":{}}
```
Note the asymmetry: `cancel` uses `a`/`o`, `cancelByCloid` uses **`asset`/`cloid`** spelled out.

### `modify` / `batchModify`

```json
{"action":{"type":"modify","oid":77738308,
  "order":{"a":0,"b":true,"p":"1950.0","s":"0.15","r":false,
           "t":{"limit":{"tif":"Gtc"}},"c":"0x..."},
  "a":true},
 "nonce":1716531066415,"signature":{}}
```
```json
{"action":{"type":"batchModify","modifies":[
   {"oid":77738308,"order":{"a":0,"b":true,"p":"1950.0","s":"0.15","r":false,
                            "t":{"limit":{"tif":"Gtc"}}}}],
  "a":true},
 "nonce":1716531066415,"signature":{}}
```
`oid` is a number **or** a cloid string. Top-level **`a` = `always_place`**: when `true`,
place the new order regardless of whether the cancel succeeded. When `false`, the new order
must be non-trigger ALO, or a non-executable GTC (whose TIF is then overridden to ALO).
**`a` must be omitted if false.** The Python SDK's `modify_order()` always routes through
`batchModify` with a single element.

### `scheduleCancel` — dead man's switch

```json
{"action":{"type":"scheduleCancel","time":1716531126415},
 "nonce":1716531066415,"signature":{}}
```
Omit `time` to clear. `time` **must be at least 5 seconds** in the future. On trigger all open
orders are canceled with status `scheduledCancel`; **max 10 triggers per day**, reset 00:00 UTC.

### `updateLeverage`

```json
{"action":{"type":"updateLeverage","asset":0,"isCross":true,"leverage":5},
 "nonce":1716531066415,"signature":{}}
```
→ `{"status":"ok","response":{"type":"default"}}`

### `updateIsolatedMargin`

```json
{"action":{"type":"updateIsolatedMargin","asset":0,"isBuy":true,"ntli":1000000},
 "nonce":1716531066415,"signature":{}}
```
`ntli` is a **signed integer with 6 decimals** (1000000 = 1 USDC); negative removes margin.
`isBuy` has no effect until hedge mode ships (SDK hardcodes `True`). Alternative that targets
a leverage instead of a USD amount:
`{"type":"topUpIsolatedOnlyMargin","asset":<asset>,"leverage":"<float string>"}`.

### `approveAgent` — user-signed

```json
{"action":{"type":"approveAgent","hyperliquidChain":"Mainnet","signatureChainId":"0xa4b1",
  "agentAddress":"0x...","agentName":"parsec valid_until 1750000000000",
  "nonce":1716531066415},
 "nonce":1716531066415,"signature":{}}
```
Python SDK detail worth copying exactly: `agentName` is included in the **signed** payload as
`""` when no name is given, then **deleted from the action before POSTing**:
```python
action = {"type":"approveAgent","agentAddress":account.address,
          "agentName": name or "","nonce":timestamp}
signature = sign_agent(self.wallet, action, is_mainnet)
if name is None:
    del action["agentName"]
```

### `approveBuilderFee` / `usdClassTransfer` / `withdraw3` — user-signed

```json
{"action":{"type":"approveBuilderFee","hyperliquidChain":"Mainnet",
  "signatureChainId":"0xa4b1","maxFeeRate":"0.001%","builder":"0x...",
  "nonce":1716531066415},"nonce":1716531066415,"signature":{}}
```
```json
{"action":{"type":"usdClassTransfer","hyperliquidChain":"Mainnet",
  "signatureChainId":"0xa4b1","amount":"1","toPerp":true,"nonce":1716531066415},
 "nonce":1716531066415,"signature":{}}
```
```json
{"action":{"type":"withdraw3","hyperliquidChain":"Mainnet","signatureChainId":"0xa4b1",
  "amount":"50","time":1716531066415,"destination":"0x..."},
 "nonce":1716531066415,"signature":{}}
```
For these, the `nonce`/`time` **inside** the action must equal the outer `nonce`.
`maxFeeRate` is a **percent string**. `usdClassTransfer` to a sub-account appends to the
amount string: `"1 subaccount:0x..."`. Withdrawals cost ~$1 and take ~5 min.

### Other actions (from the doc's section headings)

`order`, `cancel`, `cancelByCloid`, `scheduleCancel`, `modify`, `batchModify`,
`updateLeverage`, `updateIsolatedMargin`, `topUpIsolatedOnlyMargin`, `sendAsset`,
`usdSend`, `spotSend`, `withdraw3`, `usdClassTransfer`, `cDeposit`/`cWithdraw`,
`tokenDelegate`, vault deposit/withdraw, `approveAgent`, `approveBuilderFee`, `twapOrder`,
`twapCancel`, `reserveRequestWeight`, `noop`, `userSetAbstraction`, outcome
split/merge/negate, validator votes, claim rewards. SDK-only: `setReferrer`,
`createSubAccount`, `convertToMultiSigUser`, `spotDeploy`.

TWAP (out of scope for v1, recorded for later):
```json
{"action":{"type":"twapOrder","twap":{"a":0,"b":true,"s":"1.0","r":false,"m":60,"t":true}}}
{"action":{"type":"twapCancel","a":0,"t":77738308}}
```
`m` = minutes, `t` = randomize. Constraints: 30 s min sub-order interval, <=3% slippage per
sub-order, 5 min–7 days duration, $100 minimum.

### Market orders — how they are actually done

**There is no `market` action.** A market order is an aggressively priced IOC limit order.
Python SDK `Exchange.market_open` / `market_close`:

```python
DEFAULT_SLIPPAGE = 0.05  # 5%

def _slippage_price(self, name, is_buy, slippage, px=None):
    coin = self.info.name_to_coin[name]
    if not px:
        px = float(self.info.all_mids(_get_dex(coin))[coin])   # mid price
    asset = self.info.coin_to_asset[coin]
    is_spot = asset >= 10_000
    px *= (1 + slippage) if is_buy else (1 - slippage)
    # round to 5 significant figures, then to (6|8) - szDecimals decimals
    return round(float(f"{px:.5g}"),
                 (6 if not is_spot else 8) - self.info.asset_to_sz_decimals[asset])

def market_open(self, name, is_buy, sz, px=None, slippage=DEFAULT_SLIPPAGE,
                cloid=None, builder=None):
    px = self._slippage_price(name, is_buy, slippage, px)
    return self.order(name, is_buy, sz, px, order_type={"limit": {"tif": "Ioc"}},
                      reduce_only=False, cloid=cloid, builder=builder)
```
Exact convention: reference = **`allMids[coin]`** (not BBO), default slippage **5%**,
`px * (1 +/- slippage)`, then the two-stage rounding, then `{"limit":{"tif":"Ioc"}}`.
`market_close` is the same but derives size/side from `clearinghouseState.assetPositions` and
sets `reduce_only=True`.

> The SDK's rounding runs through a Python float with banker's rounding. Reimplement in
> **decimal**, mirroring the same two stages, or your prices will differ at boundaries.

---

## 4. Signing

Doc: `.../api/signing.md` (troubleshooting only — the algorithm is not published). Ground
truth: `hyperliquid/utils/signing.py` and `nktkas/hyperliquid/docs/signing.md`; the two were
cross-verified and match exactly.

Two schemes exist. Confusing them is documented as the #1 signing bug.

### 4a. L1 actions — order, cancel, modify, updateLeverage, scheduleCancel, TWAP

```python
def action_hash(action, vault_address, nonce, expires_after):
    data = msgpack.packb(action)                    # 1. msgpack the action
    data += nonce.to_bytes(8, "big")                # 2. nonce, uint64 BE
    if vault_address is None:
        data += b"\x00"                             # 3. no vault
    else:
        data += b"\x01" + address_to_bytes(vault_address)   # 0x01 + 20 raw bytes
    if expires_after is not None:
        data += b"\x00"                             # 4. separator byte 0x00
        data += expires_after.to_bytes(8, "big")    #    then uint64 BE
    return keccak(data)                             # 5. keccak-256

def construct_phantom_agent(hash, is_mainnet):
    return {"source": "a" if is_mainnet else "b", "connectionId": hash}
```

EIP-712 payload:
```python
{
 "domain": {"chainId": 1337, "name": "Exchange",
            "verifyingContract": "0x0000000000000000000000000000000000000000",
            "version": "1"},
 "types": {"Agent": [{"name":"source","type":"string"},
                     {"name":"connectionId","type":"bytes32"}],
           "EIP712Domain": [{"name":"name","type":"string"},
                            {"name":"version","type":"string"},
                            {"name":"chainId","type":"uint256"},
                            {"name":"verifyingContract","type":"address"}]},
 "primaryType": "Agent",
 "message": {"source": "a"|"b", "connectionId": <32-byte hash>}
}
```
Sign with ECDSA → `{"r": hex, "s": hex, "v": 27|28}`.

Confirmed details:
- Domain `name` = `"Exchange"`, `version` = `"1"`, `chainId` = **1337, hardcoded**,
  independent of the wallet's network; `verifyingContract` = zero address.
- Primary type `Agent`, fields in order `source: string`, `connectionId: bytes32`.
- **`source = "a"` on mainnet, `"b"` on testnet.** The Python SDK derives this as
  `base_url == MAINNET_API_URL` — any non-mainnet base URL (including localhost) signs `"b"`.
- `v` is `27` or `28`; wallets returning raw `0`/`1` must be normalized.
- The expires-after byte is `0x00` as a **separator** (not a presence flag) followed by the
  uint64 BE timestamp; the whole 9-byte block is absent when `expiresAfter` is unset.

### Cross-implementation test fixture (verified)

Both the Python SDK and an independent Rust implementation produce identical output for this
input. Use it as the first regression test in `rust/tests/signing_vectors.rs`:

```
action = {"type":"order","orders":[{"a":1,"b":true,"p":"1800","s":"0.02","r":false,
                                    "t":{"limit":{"tif":"Gtc"}}}],"grouping":"na"}
nonce  = 1690393044548
vault  = None,  expiresAfter = None

action_hash = 0xb8ed9e5a4ea7b2aebdb09fd794221c2599487f6dc8d3bf8f38656ab295f36120

privkey = 0xe908f86dbb4d55ac876378565aafeabc187f6690f046459397b17d9b9a19688e
signer  = 0xcD49bbAc6E85fdEB167EB7cA41A945d2b8758F6F
signature (mainnet) = {"r":"0x380b70e9e76181609c7d9f2ece74cf7717a0254f9d751137ffaedff685e81cc7",
                       "s":"0x27c1e43069db58d728e996b91112aff57a24f3d9221635adc053f20aa4283ad8",
                       "v":27}
```

And the phantom-agent vectors, for `connectionId =
0xde6c4037798a4434ca03cd05f00e3b803126221375cd1e7eaaaf041768be06eb` with the same key:
```
mainnet (source="a") = 0xfa8a41f6…aec7841c
testnet (source="b") = 0x1713c0fc…a881431c
```
Runnable both ways in `prototypes/signing-vectors/`.

**Msgpack field ordering is load-bearing.** Maps preserve insertion order and the hash commits
to it. Serializing through anything that sorts keys — `serde_json::Value`, a `HashMap`, a
`dict` in a language without insertion order — produces a valid-looking signature that
recovers the wrong address. Measured: the fixture above hashes to `0x356ed851…` instead of
`0xb8ed9e5a…` when routed through `serde_json::Value`. Required orders:
- `order` → `type`, `orders`, `grouping`, `builder?`; each order → `a`, `b`, `p`, `s`, `r`,
  `t`, `c?`; trigger → `isMarket`, `triggerPx`, `tpsl`
- `cancel` → `type`, `cancels[{a, o}]`, `f?`
- `cancelByCloid` → `type`, `cancels[{asset, cloid}]`, `f?`
- `batchModify` → `type`, `modifies[{oid, order}]`, `a?`
- `updateLeverage` → `type`, `asset`, `isCross`, `leverage`
- `updateIsolatedMargin` → `type`, `asset`, `isBuy`, `ntli`

**Optional false flags must be omitted, not serialized as `false`** — `f: false` and
`a: false` produce a hash the server rejects.

**Number serialization** — prices/sizes are strings with trailing zeros stripped:
```python
def float_to_wire(x: float) -> str:
    rounded = f"{x:.8f}"
    if abs(float(rounded) - x) >= 1e-12:
        raise ValueError("float_to_wire causes rounding", x)
    if rounded == "-0": rounded = "0"
    return f"{Decimal(rounded).normalize():f}"
```
`1.0` → `"1"`, `1900.50` → `"1900.5"`. `float_to_usd_int(x) = round(x * 1e6)` for `ntli`.

**Addresses must be lowercase before signing** — uppercase hex is documented error #4.

### 4b. User-signed actions — EIP-712 typed data, no phantom agent

```python
def user_signed_payload(primary_type, payload_types, action):
    chain_id = int(action["signatureChainId"], 16)
    return {
      "domain": {"name": "HyperliquidSignTransaction", "version": "1",
                 "chainId": chain_id,
                 "verifyingContract": "0x0000000000000000000000000000000000000000"},
      "types": {primary_type: payload_types, "EIP712Domain": [...]},
      "primaryType": primary_type,
      "message": action}
```
- Domain `name` = **`"HyperliquidSignTransaction"`** (not `"Exchange"`), `chainId` =
  `int(action["signatureChainId"], 16)` — the actual chain the wallet signs on.
- `signatureChainId` is a **hex string field inside the action**. Python SDK hardcodes
  `"0x66eee"` (421614, Arbitrum Sepolia); the docs show `"0xa4b1"` (42161, Arbitrum One).
  Either works — it only sets the domain chainId. **`hyperliquidChain`
  (`"Mainnet"`/`"Testnet"`) is what prevents cross-environment replay.**
- The action fields are the EIP-712 message directly: no msgpack, no keccak, no
  `connectionId`.
- `expiresAfter` is **not supported** here.

Primary types and field order (verbatim from `signing.py`):

| primaryType | Fields, in order |
|---|---|
| `HyperliquidTransaction:UsdSend` | `hyperliquidChain:string`, `destination:string`, `amount:string`, `time:uint64` |
| `HyperliquidTransaction:SpotSend` | `hyperliquidChain`, `destination:string`, `token:string`, `amount:string`, `time:uint64` |
| `HyperliquidTransaction:Withdraw` | `hyperliquidChain`, `destination:string`, `amount:string`, `time:uint64` |
| `HyperliquidTransaction:UsdClassTransfer` | `hyperliquidChain`, `amount:string`, `toPerp:bool`, `nonce:uint64` |
| `HyperliquidTransaction:ApproveAgent` | `hyperliquidChain`, `agentAddress:address`, `agentName:string`, `nonce:uint64` |
| `HyperliquidTransaction:ApproveBuilderFee` | `hyperliquidChain`, `maxFeeRate:string`, `builder:address`, `nonce:uint64` |
| `HyperliquidTransaction:TokenDelegate` | `hyperliquidChain`, `validator:address`, `wei:uint64`, `isUndelegate:bool`, `nonce:uint64` |
| `HyperliquidTransaction:SendAsset` | `hyperliquidChain`, `destination`, `sourceDex`, `destinationDex`, `token`, `amount`, `fromSubAccount`, `nonce:uint64` |
| `HyperliquidTransaction:ConvertToMultiSigUser` | `hyperliquidChain`, `signers:string`, `nonce:uint64` |
| `HyperliquidTransaction:SendMultiSig` | `hyperliquidChain`, `multiSigActionHash:bytes32`, `nonce:uint64` |

### 4c. Nonce rules

Doc: `.../api/nonces-and-api-wallets.md`

- Nonce = unix **millisecond** timestamp.
- **The 100 highest nonces are stored per signer.** A new action must have a nonce larger than
  the smallest in that set, and never previously used.
- Nonces are tracked **per signer** — the user address when signing with the master key, the
  **agent address** when signing with an API wallet. One agent signing for user + vault +
  sub-account shares a single nonce set.
- Must fall within **`(T - 2 days, T + 1 day)`**, `T` = block timestamp in unix ms.
- Recommended architecture: one API wallet per trading process; an atomic counter per address
  producing unique nonces (fast-forwardable to current ms); batch orders/cancels every ~0.1 s;
  **batch ALO orders separately from IOC/GTC** (ALO-only batches are prioritized by
  validators).
- `noop` exists specifically to burn/invalidate a pending nonce.

---

## 5. Agent / API wallets

Docs: `.../api/nonces-and-api-wallets.md`, `.../api/exchange-endpoint.md#approve-an-api-wallet`

- A master account approves API wallets ("agent wallets") to sign on its behalf or for its
  sub-accounts.
- **API wallets are only used to sign.** Query account data with the *actual* master address —
  querying with the agent address returns empty results.
- **Limits:** 1 unnamed approved wallet **plus up to 3 named** per account; an additional 2
  named agents per sub-account.
- **Expiry:** default 180 days. Custom expiry via `agentName = "<name> valid_until <ms>"`, at
  most 180 days out.
- **Pruning:** an agent and its nonce state are pruned when it is deregistered (a new
  *unnamed* `approveAgent` deregisters the existing unnamed one; a matching-name
  `approveAgent` deregisters that named one), when it expires, or when the registering
  account runs out of funds.
- **Never reuse an agent address.** After deregistration its nonce state may be pruned, which
  makes previously signed actions replayable. Generate a fresh keypair every time.

**Capabilities.** Agents sign **L1 actions** — orders, cancels, modifies, leverage/margin
updates, scheduleCancel, TWAPs. They **cannot** move funds: withdrawals and transfers
(`withdraw3`, `usdSend`, `spotSend`, `usdClassTransfer`) and security actions
(`approveAgent`, `approveBuilderFee`, `convertToMultiSigUser`) are *user-signed* actions whose
EIP-712 message contains the action fields directly — the recovered signer **is** the acting
account, so an agent key would recover to its own fundless address. This separation is
structural and verifiable from `signing.py`, but the docs publish no explicit "agents cannot
withdraw" statement, so treat the exhaustive allow/deny list as **UNVERIFIED** and confirm
empirically on testnet (`08-testing.md` has a test for exactly this).

---

## 6. Price / size validation

Doc: `.../api/tick-and-lot-size.md`

**Price (`px`) — both constraints must hold simultaneously:**
1. At most **5 significant figures**, **and**
2. At most `MAX_DECIMALS - szDecimals` decimal places, `MAX_DECIMALS = 6` for perps (8 spot).
3. **Integer prices are always allowed regardless of significant figures** — `123456` is
   valid even though `12345.6` is not.

**Size (`sz`)** is rounded to the asset's `szDecimals`. With `szDecimals = 3`, `1.001` is
valid, `1.0001` is not.

Perp examples (verbatim from the docs):
- `1234.5` valid; `1234.56` invalid (too many significant figures)
- `0.001234` valid; `0.0012345` invalid (more than 6 decimals)
- with `szDecimals = 1`: `0.01234` valid; `0.012345` invalid (more than `6-1 = 5` decimals)

**Signing interaction:** trailing zeros must be stripped before signing, so `"1900.50"` must
go on the wire as `"1900.5"`.

**Minimum order value: $10 notional** for perps (`MinTradeNtl`). Whether reduce-only orders
are exempt is **UNVERIFIED**.

**There is no explicit per-asset tick size field.** The effective tick is derived from the
5-sig-fig + `(6 - szDecimals)` pair and therefore *varies with price magnitude*: at $113,377
the tick is 1.0 (integers always allowed), at $14.31 it is 0.001. Violations return
`Price must be divisible by tick size.` Lot size is simply `10^-szDecimals`.

**Open-order cap:** 1000 by default, +1 per 5M USDC of volume, capped at 5000. At >=1000 open
orders, new **reduce-only or trigger** orders are rejected.

---

## 7. Rate limits

Doc: `.../api/rate-limits-and-user-limits.md`

### Per IP
- REST shares an aggregated weight limit of **1200 per minute**.
- `exchange` weight: `1 + floor(batch_length / 40)`.
- `info` weight **2**: `l2Book`, `allMids`, `clearinghouseState`, `orderStatus`,
  `spotClearinghouseState`, `exchangeStatus`.
- `info` weight **60**: `userRole`.
- **All other `info` requests: weight 20** — including `meta`, `metaAndAssetCtxs`,
  `candleSnapshot`, `openOrders`, `userFills`, `userRateLimit`.
- Extra weight **per 20 items returned** for `recentTrades`, `historicalOrders`, `userFills`,
  `userFillsByTime`, `fundingHistory`, `userFunding`, twap history, delegator history.
- `candleSnapshot`: extra weight **per 60 items returned**.
- WebSocket: **10** connections, **30** new connections/min, **1000** subscriptions, **10**
  unique users across user-specific subscriptions, **2000** messages/min sent across all
  connections, **100** simultaneous inflight post messages.

### Per address (actions only, not info)
- **1 request per 1 USDC of cumulative traded volume since address inception.**
- **Initial buffer: 10000 requests.**
- When rate limited: **one request every 10 seconds**.
- Cancels get a higher cumulative limit, `min(limit + 100000, limit * 2)`, so a rate-limited
  address can still cancel.
- Under congestion an address is limited to 2x its previous-day maker share of block space.
- Actions killed by a stale `expiresAfter` consume **5x** the normal address limit.

### Batching
A batch of `n` orders/cancels = **1 request for IP limits**, **`n` requests for address
limits**.

### 429 behaviour
**UNVERIFIED** — the docs never mention HTTP 429 or `Retry-After`. What is verifiable from the
Python SDK: 4xx bodies are parsed as JSON with `code`/`msg`/optional `data`; unparseable
bodies raise with raw text; 5xx raises a server error. **Do not assume a machine-readable 429
body — back off on the status code and track weight client-side.**

---

## 8. Error responses

Doc: `.../api/error-responses.md`

Three distinct error shapes, **all arriving as HTTP 200**:

**1. Top-level error** — whole request failed (bad signature, unknown user, malformed):
```json
{"status": "err", "response": "L1 error: User or API Wallet 0x0123... does not exist."}
```
`response` is a bare **string**, not an object.

**2. Bulk error** — per-element statuses:
```json
{"status":"ok","response":{"type":"order","data":{"statuses":[
  {"resting":{"oid":77738308}},
  {"error":"Order must have minimum value of $10."}]}}}
```

**3. Single error** — `response.data.status` (singular) holding `{error: string}`.

> **`status: "ok"` does not mean success.** Always walk `response.data.statuses[]`.

### Per-element error strings

| Type | String |
|---|---|
| `Tick` | `Price must be divisible by tick size.` |
| `MinTradeNtl` | `Order must have minimum value of $10.` |
| `MinTradeSpotNtl` | `Order must have minimum value of 10 {quote_token}.` |
| `PerpMargin` | `Insufficient margin to place order.` |
| `ReduceOnly` | `Reduce only order would increase position.` |
| `BadAloPx` | `Post only order would have immediately matched, bbo was {bbo}.` |
| `IocCancel` | `Order could not immediately match against any resting orders.` |
| `BadTriggerPx` | `Invalid TP/SL price.` |
| `MarketOrderNoLiquidity` | `No liquidity available for market order.` |
| `PositionIncreaseAtOpenInterestCap` | `Order would increase open interest while open interest is capped` |
| `PositionFlipAtOpenInterestCap` | `Order would increase open interest while open interest is capped` |
| `TooAggressiveAtOpenInterestCap` | `Order rejected due to price more aggressive than oracle while at open interest cap` |
| `OpenInterestIncrease` | `Order would increase open interest too quickly` |
| `InsufficientSpotBalance` | `(Spot-only) Order has insufficient spot balance to trade` |
| `Oracle` | `Order price too far from oracle` |
| `PerpMaxPosition` | `Order would cause position to exceed margin tier limit at current leverage` |
| `MissingOrder` (cancel) | `Order was never placed, already canceled, or filled.` |

### Pre-validation — critical for batch handling

Errors that are a deterministic function of the payload come back **earlier, as a single error
for the entire payload**: empty order batch, non-reduce-only TP/SL, price too far from
reference, and some tick-size violations.

> "For API users that use batching, it's recommended to handle the case where a single error
> is returned for a batch of multiple orders. In this case, the response could be duplicated
> `n` times before being sent to the callback function, as the whole batch was rejected for
> this same reason."

### Signature failures

An incorrect signature never says why — it recovers a *different address* and surfaces as:
```
L1 error: User or API Wallet 0x0123... does not exist.
Must deposit before performing actions. User: 0x123...
```
The recovered address changes with every different input. Documented root causes: (1) wrong
signing scheme, (2) wrong msgpack field order, (3) trailing zeros on numbers, (4) uppercase
hex in address fields, (5) trusting a local `recover_signer` check — the local payload is
rebuilt from the action and does not necessarily match what the server builds.

---

## 9. Gotcha checklist — pin these on the wall

1. `status: "ok"` is not success. Three error shapes, all HTTP 200.
2. msgpack key order is part of the signature; `f: false` / `a: false` must be **omitted**.
3. `cancel` uses `{a, o}`; `cancelByCloid` uses `{asset, cloid}`.
4. `source = "a"|"b"` is derived from the base URL — a proxy silently flips you to testnet
   signing.
5. Nonces are per **signer**, not per account.
6. Query info with the **master** address; sign with the **agent** key.
7. Price validation is a conjunction of two rules plus an integer escape hatch — implement in
   decimal, never float.
8. Strip trailing zeros before signing: `"1900.50"` → `"1900.5"`.
9. `expiresAfter` prepends a `0x00` separator before its 8-byte BE timestamp, and is
   unsupported on user-signed actions.
10. Batching is 1 IP request but `n` address requests; the address limit is earned at 1
    request per USDC traded, off a 10000 buffer.
11. Never hardcode an asset id; resolve from `meta` at startup, per environment.
12. `scheduleCancel` needs `time` >= 5 s out and allows only 10 triggers per UTC day.

---

# Part II — WebSocket API

Verified against the official docs **and live probes against `wss://api.hyperliquid.xyz/ws`
on 2026-08-19** with a raw RFC-6455 client. Items tagged **[MEASURED]** come from those live
probes; **[DOC]** is doc text; **[UNVERIFIED]** could not be confirmed.

> **Read this first: `webData2` no longer exists.** It has been replaced by `webData3`, and
> the live server *rejects* a `webData2` subscribe with a parse error. The Python SDK is stale
> on this point. See §W2.3.

---

## W1. URLs, lifecycle, heartbeats

| Network | URL |
|---|---|
| Mainnet | `wss://api.hyperliquid.xyz/ws` |
| Testnet | `wss://api.hyperliquid-testnet.xyz/ws` |

**Idle timeout — note the direction.** [DOC] *"The server will close any connection if it
hasn't sent a message to it in the last 60 seconds."* The 60 s clock is on
**server → client** traffic, not client → server. A connection subscribed only to a quiet
feed dies even while you are sending. Send `ping` on a timer regardless of your own outbound
traffic — only the `pong` (or real data) resets the clock.

```json
→ { "method": "ping" }
← { "channel": "pong" }
```

**Ping interval: 50 s.** Not in the docs; from `hyperliquid/websocket_manager.py`:
```python
def send_ping(self):
    while not self.stop_event.wait(50):
        self.ws.send(json.dumps({"method": "ping"}))
```

**No greeting frame.** The Python SDK special-cases a literal text frame
`"Websocket connection established."`. The live server sent no such frame on either network
**[MEASURED]** — legacy, but keep the guard: it is non-JSON and would crash a naive parser.

**Reconnect.** [DOC] *"all automated users should handle disconnects from the server side and
gracefully reconnect. Disconnection from API servers may happen periodically and without
announcement. Missed data during the reconnect will be present in the snapshot ack on
reconnect."* There is no session resumption, no resume token, no replay cursor — re-send
every `subscribe` from scratch. For user feeds the snapshot ack backfills the gap; for
`l2Book`/`bbo` the next push *is* the recovery, since both are full snapshots.

**Subscription identity is normalized server-side** [MEASURED]:
```json
→ {"method":"subscribe","subscription":{"type":"l2Book","coin":"BTC"}}
← {"channel":"subscriptionResponse","data":{"method":"subscribe",
     "subscription":{"type":"l2Book","coin":"BTC","nSigFigs":null,"mantissa":null,"fast":false}}}
```
Duplicate subscribe → `{"channel":"error","data":"Already subscribed: {...}"}`. Unsubscribe
matches on the *normalized* form, so a loose `{"type":"l2Book","coin":"BTC"}` correctly
cancels a subscription created with explicit nulls.

---

## W2. Subscriptions

```json
{ "method": "subscribe",   "subscription": { } }
{ "method": "unsubscribe", "subscription": { } }
```
Data messages are `{"channel":"<type>","data":<payload>}`. The channel name equals the
subscription `type` **except**: `userEvents` → `"user"`; `activeAssetCtx` on a spot asset →
`"activeSpotAssetCtx"` **[MEASURED]**.

### W2.0 Snapshot vs delta — the table that drives client bootstrap

| Subscription | Initial snapshot? | Steady state | `isSnapshot` |
|---|---|---|---|
| `l2Book` | yes (every msg is one) | full snapshot, top-N | no |
| `bbo` | yes | full BBO on change | no |
| `trades` | **yes — 30 recent trades** [MEASURED] | incremental array | no |
| `candle` | yes (current bucket) | repeated updates of same/next bucket | no |
| `allMids` | yes | full map every push | no |
| `activeAssetCtx` / `activeSpotAssetCtx` | yes | full ctx | no |
| `activeAssetData` | yes | full | no |
| `fastAssetCtxs` | yes, then **delta** (changed coins only) | delta | no |
| `userFills` | yes | streaming fills | **yes** |
| `userFundings` | yes | on the hour | **yes** |
| `userNonFundingLedgerUpdates` | yes | streaming | **yes** |
| `userTwapSliceFills` / `userTwapHistory` | yes | streaming | **yes** |
| `orderUpdates` | **no** | pure delta | no |
| `userEvents` | **no** | pure delta | no |
| `notification` | no | delta | no |
| `webData3`, `clearinghouseState`, `openOrders`, `spotState`, `twapStates` | yes | full state re-push (~4–5 s) | no |

Two sharp edges:
- **`trades` backfills 30 trades on subscribe** — undocumented. Dedupe or you double-count on
  every reconnect.
- **`orderUpdates` sends no snapshot.** Subscribing does *not* give you your resting orders.
  Bootstrap from `openOrders`/`frontendOpenOrders` and reconcile.

### W2.1 `allMids`
```json
{"type":"allMids"}   |   {"type":"allMids","dex":"<dex>"}
→ {"channel":"allMids","data":{"mids":{"BTC":"69172.5","@107":"69.408","#11210":"0.999245"}}}
```
**15.8 KB per message, ~1 every 5 s** [MEASURED]. Keys mix perp names, spot indices `@N`,
prediction outcomes `#N`, and HIP-3 `dex:COIN`. Full map every time. **Wrong feed for a
latency-sensitive client** — use `bbo` per coin, or `fastAssetCtxs`.

### W2.2 `notification`
```json
{"type":"notification","user":"0x..."}
→ {"channel":"notification","data":{"notification":"Resting order filled: Sold 7262 MON at $0.023038"}}
```
Human-readable strings for UI toasts. Never drive state from it.

### W2.3 `webData3` — replaces `webData2`
```json
{"type":"webData3","user":"0x..."}
→ {"channel":"webData3","data":{
    "userState":{"agentAddress":null,"agentValidUntil":null,"cumLedger":"6815897.23",
                 "serverTime":1787195122463,"isVault":true,"user":"0x31ca..."},
    "perpDexStates":[{"totalVaultEquity":"0.0",
                      "perpsAtOpenInterestCap":["CANTO","FTM","JELLY"]}]}}
```
[DOC] warns: *"Additional undocumented fields in WebData3 will be removed on a future
update."*

**Migration note:** `webData3` is now *lighter* than old `webData2` — positions and orders
were split out into dedicated `clearinghouseState`, `openOrders`, `spotState`, `twapStates`
subscriptions. **Subscribe to those individually rather than taking the frontend blob.**
Sizes [MEASURED]: `webData3` 609 B, `clearinghouseState` 65 KB, `openOrders` 28 KB — the
latter two re-pushed in full every ~4 s.

The docs' own Example #3 still shows `{"type":"webData",...}` — stale on two counts. Python
SDK still defines `WebData2Subscription`; it will be rejected.

### W2.4 `candle`
```json
{"type":"candle","coin":"BTC","interval":"1m"}
→ {"channel":"candle","data":{"t":1787195040000,"T":1787195099999,"s":"BTC","i":"1m",
    "o":"69170.0","c":"69172.0","h":"69174.0","l":"69170.0","v":"0.80052","n":47}}
```
**Docs are wrong on types.** The TS block declares `o,c,h,l,v: number`; on the wire they are
**strings** [MEASURED], and the Rust SDK agrees (`src/ws/sub_structs.rs`: `pub close: String`).
Only `t`, `T`, `n` are numbers. Docs also say the format is `Candle[]`; the wire sends a bare
object.

### W2.5 `l2Book`
```json
{"type":"l2Book","coin":"BTC"}
{"type":"l2Book","coin":"BTC","nSigFigs":5,"mantissa":2,"fast":true}
```
[DOC] *"Optional parameters: `nSigFigs: int`, `mantissa: int`, `fast: boolean` (5 levels if
fast, 20 levels if slow)"*
```json
{"channel":"l2Book","data":{"coin":"BTC","time":1787195067883,"levels":[
  [{"px":"69172.0","sz":"1.21098","n":5},{"px":"69171.0","sz":"0.35315","n":2}],
  [{"px":"69173.0","sz":"8.45682","n":30},{"px":"69174.0","sz":"0.63175","n":5}]]}}
```
**Ordering confirmed [MEASURED]:** `levels[0]` = bids **descending**, `levels[1]` = asks
**ascending**; index 0 of each is the touch. `px`/`sz` are strings; `n` is a number. With
`fast:true` the data object gains `"fast":true`.

### W2.6 `trades`
```json
{"type":"trades","coin":"BTC"}
→ {"channel":"trades","data":[{"coin":"BTC","side":"A","px":"69170.0","sz":"0.00018",
    "time":1787195046105,"hash":"0x0000...0000","tid":456615105517601,
    "users":["0xf5d81a13...","0x4d20a11d..."]}]}
```
`users` is `[buyer, seller]`. `side` is the **aggressor** side (`"A"` = sell, `"B"` = buy).
[DOC] *"tid is 50-bit hash of (buyer_oid, seller_oid). For a globally unique trade id, use
(block_time, coin, tid)"* — **`tid` alone is not unique**; dedupe on the triple. `hash` is
sometimes all-zeros [MEASURED]; never key on it.

### W2.7 `orderUpdates`
```json
{"type":"orderUpdates","user":"0x..."}
→ {"channel":"orderUpdates","data":[
    {"order":{"coin":"ETH","side":"A","limitPx":"2412.7","sz":"0.0","oid":1,
              "timestamp":1724361546645,"origSz":"0.0076","cloid":null},
     "status":"filled","statusTimestamp":1724361546645}]}
```
`sz` = **remaining** size, `origSz` = original. The full 28-value `status` enum is in Part I
§`orderStatus`. **Treat it as open-ended**: the `*Rejected` / `*Canceled` suffix convention
holds today, but new values ship without notice — classify by suffix, never match
exhaustively.

### W2.8 `userEvents` — channel is `"user"`
```json
{"type":"userEvents","user":"0x..."}
```
Four mutually exclusive variants:
```typescript
type WsUserEvent = {"fills": WsFill[]} | {"funding": WsUserFunding}
                 | {"liquidation": WsLiquidation} | {"nonUserCancel": WsNonUserCancel[]};
```
```json
{"channel":"user","data":{"fills":[{"coin":"MINA","px":"0.042572","sz":"373.0","side":"B",
  "time":1787195122810,"startPosition":"-1069998.0","dir":"Close Short",
  "closedPnl":"-0.767634","hash":"0x81cb46c9...","oid":520430480361,"crossed":true,
  "fee":"0.0","tid":603829295392620,"feeToken":"USDC","twapId":null}]}}
```
```typescript
interface WsUserFunding { time:number; coin:string; usdc:string; szi:string; fundingRate:string; }
interface WsLiquidation { lid:number; liquidator:string; liquidated_user:string;
                          liquidated_ntl_pos:string; liquidated_account_value:string; }
interface WsNonUserCancel { coin:String; oid:number; }
```
`WsLiquidation` is the only **snake_case** struct in the whole API. `nonUserCancel` means the
exchange killed your order (liquidation, OI cap, delisting) — not you.

Python SDK limitation: `websocket_manager.py` raises `NotImplementedError` on a second
`userEvents`/`orderUpdates` subscription, because those messages carry no `user` field and
cannot be demultiplexed. One connection per account, or route it yourself.

### W2.9 `userFills`
```json
{"type":"userFills","user":"0x...","aggregateByTime":false}
→ {"channel":"userFills","data":{"isSnapshot":true,"user":"0x31ca...","fills":[...]}}
```
`isSnapshot` is **absent** (not `false`) on streaming messages — test it truthily, don't
require the key. Snapshot measured at 9.7 KB.
```typescript
interface WsFill {
  coin:string; px:string; sz:string; side:string; time:number;
  startPosition:string; dir:string;          // dir is display-only
  closedPnl:string; hash:string;             // L1 tx hash
  oid:number; crossed:boolean;               // crossed = was taker
  fee:string;                                // negative = rebate
  tid:number; liquidation?:FillLiquidation;
  feeToken:string; builderFee?:string;       // builderFee is ALREADY inside fee
}
```
Wire adds an undocumented `twapId` [MEASURED]. `crossed` is your maker/taker flag — the
cheapest fee attribution available. **Adding `builderFee` to `fee` double-counts.**

### W2.10–2.11 `userFundings`, `userNonFundingLedgerUpdates`
Array keys are undocumented: **`fundings`** (each entry with an extra `nSamples`) and
**`nonFundingLedgerUpdates`** [MEASURED]. The latter is full account history, unbounded —
27 KB for one probed account. Discriminate on `delta.type`: `deposit, withdraw,
internalTransfer, subAccountTransfer, liquidation, vaultCreate, vaultDeposit,
vaultDistribution, vaultWithdraw, vaultLeaderCommission, spotTransfer, accountClassTransfer,
spotGenesis, rewardsClaim`.

### W2.12 `activeAssetCtx` / `activeSpotAssetCtx`
```json
{"type":"activeAssetCtx","coin":"BTC"}
→ {"channel":"activeAssetCtx","data":{"coin":"BTC","ctx":{
    "funding":"0.0000125","openInterest":"34524.51236","prevDayPx":"64319.0",
    "dayNtlVlm":"5691036987.81","premium":"-0.0001329822","oraclePx":"69182.2",
    "markPx":"69172.0","midPx":"69172.5","impactPxs":["69172.0","69173.0"],
    "dayBaseVlm":"84496.56164"}}}
```
~1 msg/s [MEASURED]. **This is the feed behind the header strip** (mark, oracle, 24 h change,
volume, OI, funding). Docs are wrong twice: all ctx numerics are **strings**, and `premium`,
`impactPxs`, `dayBaseVlm`, inner `coin` are undocumented but always present.

### W2.13 `activeAssetData` (perps only)
```json
{"type":"activeAssetData","user":"0x...","coin":"BTC"}
→ {"channel":"activeAssetData","data":{"user":"0x31ca...","coin":"BTC",
    "leverage":{"type":"cross","value":20},
    "maxTradeSzs":["803.83003","803.95321"],
    "availableToTrade":["2779282.52","2779708.42"],"markPx":"69151.0"}}
```
Tuples are `[buy, sell]`; docs say numbers, wire sends **strings**, plus undocumented
`markPx`. **This is the correct pre-trade sizing feed** — it backs "Available to Trade" and
the size slider without a `clearinghouseState` round trip per order.

### W2.14 `bbo`
```json
{"type":"bbo","coin":"BTC"}
→ {"channel":"bbo","data":{"coin":"BTC","time":1787195068169,
    "bbo":[{"px":"69172.0","sz":"0.92185","n":4},{"px":"69173.0","sz":"9.07775","n":32}]}}
```
`bbo[0]` = best bid, `bbo[1]` = best ask; either may be `null` on an empty side.
[DOC] *"Bbo updates that are sent only if the bbo changes on a block."* **This is the feed you
want for execution** — see W3.

### W2.15 `userTwapSliceFills` / `userTwapHistory`
TWAP slice fills do **not** appear in `userFills` — separate stream. Out of scope for v1, but
if TWAPs are ever added, subscribing only to `userFills` silently loses those executions.

### W2.16 Subscriptions newer than the SDKs

All verified live [MEASURED]:

| Sub | Subscription message | Notes |
|---|---|---|
| `clearinghouseState` | `{"type":"clearinghouseState","user":"0x...","dex":""}` | 65 KB, ~4 s |
| `openOrders` | `{"type":"openOrders","user":"0x...","dex":""}` | 28 KB, ~4 s, full order objects incl. `triggerCondition`, `children`, `isPositionTpsl`, `tif` |
| `spotState` | `{"type":"spotState","user":"0x...","isPortfolioMargin":bool}` | ack normalizes the field to **`ignorePortfolioMargin`** |
| `twapStates` | `{"type":"twapStates","user":"0x...","dex":""}` | |
| `allDexsClearinghouseState` | `{"type":"allDexsClearinghouseState","user":"0x..."}` | |
| `allDexsAssetCtxs` | `{"type":"allDexsAssetCtxs"}` | **111 KB per message — avoid** |
| `fastAssetCtxs` | `{"type":"fastAssetCtxs"}` | see below |

**`fastAssetCtxs`** is the one to know for latency. `data` is a base64 string wrapping a
**raw DEFLATE (RFC 1951, no zlib/gzip wrapper)** payload:
```
base64-decode → inflate raw → UTF-8 → JSON
Python: zlib.decompress(b, wbits=-15)     Rust: flate2::read::DeflateDecoder
```
Doc's self-test vector:
```
payload: "q1ZyCnFWsqpWyk0syg6oULJSsjQ3NTDQM1Wq1VFyDfFAkTI2MzXQMwJLVVRWWfmFuTiiyBuamOoZKdXWAgA="
decoded: {"BTC":{"markPx":"97500.5"},"ETH":{"markPx":"3650.25"},"xyz:NVDA":{"markPx":"145.2"}}
```
Live [MEASURED]: 16 KB base64 → 57 KB JSON, ~1 msg/s. [DOC] *"The first message is a
snapshot, subsequent messages contain only coins that have updated."* — **the only
delta-encoded subscription in the API**, ~3.5x smaller on the wire than `allMids` while
covering every dex.

---

## W3. `l2Book` mechanics — and why `bbo` is the execution feed

**Full snapshot, never a diff.** Docs comment it inline:
`// Snapshot feed, pushed on each block that is at least 0.5 since last push`. Confirmed
empirically — there is no add/remove/change encoding anywhere in the message. **You never
apply a delta and therefore can never desync.** Your book is `data.levels`, replaced wholesale.

**Depth:** 20 levels/side default, **5 levels/side with `fast:true`**. Both confirmed
[MEASURED]. REST `l2Book` likewise caps at 20. **No parameter yields more than 20** on the
public API; deeper books require a local node (W7).

### Measured cadence — 40 s continuous capture, BTC, all feeds on one connection [MEASURED]

| Feed | msgs | rate | median Δt | min | max | bytes/msg |
|---|---|---|---|---|---|---|
| `bbo` | 296 | **7.40/s** | **0.103 s** | 0.000 | 0.716 | 145 |
| `l2Book fast:true` | 73 | 1.82/s | 0.548 s | 0.122 | 0.791 | 470 |
| `l2Book` default | 9 | **0.23/s** | **5.25 s** | 1.117 | 5.525 | 1600 |
| `allMids` | 8 | 0.20/s | 5.04 s | 4.836 | 5.269 | 15802 |

**`bbo` beats default `l2Book` by ~32x in rate and ~11x in bytes** — roughly 360x more
efficient per unit of top-of-book information. Distinct throttle tiers emerged: ~0.1 s
event-driven (`bbo`) · ~0.5 s (`l2Book fast`) · ~1 s (`activeAssetCtx`, `fastAssetCtxs`) ·
~4–5 s (`l2Book` default, `allMids`, user-state snapshots).

> **Caveat.** The docs state 0.5 s coalescing for `l2Book` generally; the default variant
> measured ~5 s from the probing sandbox, while `bbo` and `l2Book fast` on the *same socket*
> ran at documented speed — so this is not local network throttling, but an
> environment-specific difference cannot be ruled out. Treat the **ordering**
> (`bbo` >> `fast` >> `default`) as solid; re-measure the absolute default-`l2Book` number
> from your own region before designing around it.

**Coalescing is lossy by design.** Intermediate book states between pushes are never sent and
cannot be recovered from this API. Every book transition requires raw diffs from a node (W7).

### `nSigFigs` / `mantissa` — display aggregation only

[DOC] `nSigFigs`: 2, 3, 4, 5, or `null` (full precision). `mantissa`: 1, 2, or 5, **only when
`nSigFigs == 5`**. Verified [MEASURED]:
- `{"coin":"SOL","nSigFigs":3}` → `84.4, 84.3, 84.2, 84.1...` — bucketed to 0.1
- `{"coin":"DOGE","nSigFigs":5,"mantissa":5}` → `0.075195, 0.07519, 0.075185...` — steps of
  0.000005

These **merge** adjacent levels (summing `sz` and `n`). They shrink messages and make a
smoother display book — **but they destroy the true touch price.** Wire them to the book
panel's aggregation dropdown (the `0.00001 ▾` control in the reference UI) and **never** to
execution logic.

---

## W4. POST requests over WebSocket

```json
{ "method": "post", "id": <u64>, "request": { "type": "info" | "action", "payload": { } } }
→ { "channel": "post", "data": { "id": <u64>,
      "response": { "type": "info"|"action"|"error", "payload": { } } } }
```
`method` and `id` are mandatory. `explorer` requests are unsupported.

**Signing is identical to REST.** The `payload` is byte-for-byte the same object you would
POST to `/exchange` — same msgpack hash, same EIP-712 domain, same nonce, same `signature`.
`vaultAddress` and `expiresAfter` pass through unchanged. The WS wrapper is pure transport.

### Three distinct failure modes [MEASURED] — the most important part of this section

**1. Semantic failure → normal `post` envelope, `type` stays `"info"`/`"action"`:**
```json
← {"channel":"post","data":{"id":12,"response":{"type":"action",
     "payload":{"status":"err","response":"Unable to recover signer."}}}}
```
A rejected order comes back as `type:"action"` with `payload.status == "err"` — **not**
`type:"error"`. **Checking only `response.type === "error"` will silently treat every
rejected order as a success.**

**2. Malformed / unparseable request → bare `error` channel, `id` is LOST:**
```json
→ {"method":"post","id":2,"request":{"type":"info","payload":{"type":"bogusInfoType"}}}
← {"channel":"error","data":"Error parsing JSON into valid websocket request: {...}"}
```
The response has **no `id` field at all** — the only correlation handle is the echoed request
text inside the error string. Unknown info `type` values fail at the parse layer, not routing.

> **Design implication:** every inflight post needs a client-side timeout. A malformed request
> never produces a `post` envelope, so an `id`-keyed promise map leaks forever. Time out, and
> reconcile order state out-of-band rather than assuming an unanswered post did not execute.

**3. `type:"error"`** — [DOC] the transport tier: *"a `String` is returned mirroring the HTTP
status code and description"* (429, 5xx). Exact shape **[UNVERIFIED]**.

### Latency and rate-limit implications vs REST

- **Rate limits are shared, not separate.** WS posts count against the same per-IP 1200
  weight/min budget and the same address-based action limits. **Moving to WS buys no extra
  quota.**
- **What it does buy:** no TCP+TLS handshake per order, no HTTP headers, no pool contention.
  On a warm connection this removes a full RTT of setup from the cold path.
- **Inflight cap: 100 across *all* connections** — you cannot widen it by opening more
  sockets. Keep your own inflight counter with a ceiling below 100 and backpressure above it.
- **Responses arrive out of order.** Always correlate by `id`.

---

## W5. WebSocket rate limits (per IP)

| Limit | Value |
|---|---|
| Max WebSocket connections | **10** |
| Max new connections per minute | **30** |
| Max subscriptions | **1000** |
| Max unique users across user-specific subscriptions | **10** |
| Max messages sent per minute (all connections) | **2000** |
| Max simultaneous inflight post messages (all connections) | **100** |

Behaviour on violation is **[UNVERIFIED]** for WS specifically. Inferable: post violations
surface as `response.type == "error"` with the mirrored HTTP 429 string; connection- and
subscription-count violations most plausibly produce an `error`-channel string or a close.

---

## W6. Ordering, sequence numbers, staleness

**There are no sequence numbers. None. Anywhere.** Verified by exhaustive inspection of every
live message across all subscription types [MEASURED] — no `seq`, `u`, `U`, `pu`, or counter
field exists on any channel. This is a real architectural difference from Binance/Bybit/OKX,
whose resync algorithms are built on exactly such fields.

**No gap detection is possible, and none is needed for books.** `l2Book` and `bbo` are full
snapshots, so a dropped or coalesced message causes no corruption — the next message restores
state. The classic "buffer diffs, fetch snapshot, replay from `U`" dance does not apply. The
tradeoff: you cannot know how many intermediate states you missed.

**What you do get: `time`, a block timestamp in epoch ms.** Verified [MEASURED] across 296
`bbo` and 82 `l2Book` messages: monotonically non-decreasing, and **all subscriptions for a
given block share the same value** (BTC, ETH, and DOGE `l2Book` all reported
`time: 1787195122342` in one probe). So `time` is a **block clock** usable to align feeds
across coins — but not a counter, and it cannot reveal a gap.

### Staleness detection — the only available defense, layered

1. **Wall-clock skew on `data.time`.** Alarm when `now_ms - data.time` exceeds ~3x the
   expected cadence for that feed tier (W3). Catches a silently wedged connection — the
   dangerous failure, since TCP can stay open with no data flowing.
2. **Duplicate-content detection.** Measured `0/8` consecutive-identical slow `l2Book`
   payloads and `0/295` for `bbo` (ignoring `time`) — the server does not re-push unchanged
   books, so repeated identical payloads signal a stuck upstream.
3. **`exchangeStatus` as a heartbeat oracle.** Cheap (weight 2), returns L1 time:
   `{"type":"exchangeStatus"}` → `{"specialStatuses":null,"time":1787195273903}`. The node
   README recommends exactly this: compare L1 and local timestamps, ignore server information
   when the L1 timestamp is sufficiently stale.
4. **Cross-feed consistency.** Run `bbo` alongside `l2Book` for traded coins. `bbo` updates
   ~32x more often, so `l2Book.levels[0][0]` diverging from `bbo[0]` for longer than the
   coalescing window means the book feed is lagging. **Cheapest high-signal check available.**
5. **Periodic REST reconciliation** of `openOrders` / `clearinghouseState`, since
   `orderUpdates` has no snapshot and no gap detection.
6. **Missed-`pong` counter.** Two consecutive unanswered 50 s pings → tear down and reconnect
   rather than waiting for the 60 s server close.

**Ordering guarantees: none documented, at any level.** Within one subscription messages
arrive in block order (monotonic `time`), but nothing is published about ordering *between*
subscriptions on one connection, nor between `post` responses and subscription data. Do not
assume an `orderUpdates` message arrives after the `post` response that created the order.

---

## W7. Faster alternatives — explicitly OUT of scope for v1

**Decision D8: v1 talks only to the public Hyperliquid REST and WebSocket endpoints.** No
non-validating node, no `order_book_server`, no third-party gateway. This section is recorded
so the option is understood, not so it gets built. See `02-architecture.md` §6.1.1 for why
adopting any of it later is a config change rather than a rewrite.

### Non-validating node — `github.com/hyperliquid-dex/node`
Ports 4001/4002 must be publicly open for gossip. Relevant flags:

| Flag | Effect |
|---|---|
| `--write-trades` / `--write-fills` | trades / fills in API format |
| `--write-order-statuses` | every L1 order status |
| `--write-raw-book-diffs` | **every L1 order diff — the true tick-by-tick book** |
| `--batch-by-block` | one block per line with block metadata |
| `--stream-with-block-info` | write events as processed, not once per block |
| `--disable-output-file-buffering` | flush each line immediately — **the key latency flag** |
| `--serve-info` | local HTTP info server on `localhost:3001/info` |

`--write-raw-book-diffs` is what gets below the WS coalescing floor:
```json
{"user":"0x7684...","oid":35061046831,"coin":"CHILLGUY","side":"Bid","px":"1.36",
 "raw_book_diff":{"new":{"sz":"186910.0"}}}
{"user":"0x7684...","oid":35061055064,"coin":"BTC","side":"Bid","px":"115323.2",
 "raw_book_diff":{"update":{"origSz":"0.2086","newSz":"0.2076"}}}
{"user":"0xc64c...","oid":35061057543,"coin":"HYPE","side":"Ask","px":"115200.2",
 "raw_book_diff":"remove"}
```
It carries `user` and `oid` per level change — information the public WS never exposes at any
tier. Combined with L4 snapshots this reconstructs a full order-level book. **~100 GB of logs
per day** at default settings.

**`--serve-info` deliberately does not serve `l2Book`** — [DOC] *"they are only indexed by a
small number of assets and can be easily polled or subscribed to within the standard rate
limits."* So it solves rate limits and trust, **not** book latency.

### `github.com/hyperliquid-dex/order_book_server`
Turns node output back into a WS API. Carries an "as is, educational purposes" disclaimer.
Serves `l2book` (with **`n_levels` up to 100**, 5x the public ceiling) and `l4Book` (full
snapshot then order diffs by block). **Self-guarding pattern worth copying:** it exits if the
node stops producing events for 5 s, and periodically re-fetches book snapshots to compare
against internal state, exiting on divergence.

### Foundation non-validating node
Runs in **AWS `apne1-az1` (Tokyo)** — same region as the validator set, which is why
colocation discussion centers on Tokyo. It is a **peer to gossip from**, not a data API.
Eligibility: 10,000 HYPE staked, Tier 1+ maker rebate tier, 98% time-weighted uptime.
Gossip roots are queryable without any of this:
```bash
curl -X POST -H "Content-Type: application/json" \
  --data '{"type":"gossipRootIps"}' https://api.hyperliquid.xyz/info
```

**Order submission always goes to `api.hyperliquid.xyz` regardless of your market-data
source** — third-party providers shorten the read path, never the write path.

---

## W8. Docs-vs-wire discrepancy list — these will bite the deserializer

| # | Field | Docs say | Wire sends |
|---|---|---|---|
| 1 | `webData2` | — | **subscribe rejected**; use `webData3`. Python SDK still ships `WebData2Subscription` |
| 2 | `Candle.o/c/h/l/v` | `number` | **string** |
| 3 | `Candle` format | `Candle[]` | bare object |
| 4 | `PerpsAssetCtx` / `SpotAssetCtx` numerics | `number` | **string** |
| 5 | `activeAssetCtx.ctx` | documented fields | + `premium`, `impactPxs`, `dayBaseVlm`, inner `coin` |
| 6 | `activeAssetData.maxTradeSzs` / `availableToTrade` | `[number, number]` | `[string, string]`, + undocumented `markPx` |
| 7 | `WsFill` | documented fields | + undocumented `twapId` |
| 8 | `userFundings` array key | unnamed | **`fundings`**, each with extra `nSamples` |
| 9 | `userNonFundingLedgerUpdates` key | unnamed | **`nonFundingLedgerUpdates`**, numerics are strings |
| 10 | `spotState` param | `isPortfolioMargin` | ack normalizes to **`ignorePortfolioMargin`** |
| 11 | `WebData3.cumLedger` | `number` | **string** |
| 12 | `trades` on subscribe | not mentioned | **30-trade backfill** |
| 13 | Docs Example #3 | `{"type":"webData",...}` | no such type |
| 14 | `fastAssetCtxs` values | `number` | **string** |

> **General rule: assume every numeric-looking field is a decimal string.** Hyperliquid
> serializes prices, sizes, and USD amounts as strings throughout; the TypeScript in the docs
> is aspirational on this point. Parse into a decimal/fixed-point type, never a float.

---

## W9. What parsec actually subscribes to

Concrete consequence of everything above — the v1 subscription set per active instrument:

| Purpose | Subscription | Why this one |
|---|---|---|
| Execution price | `bbo` | 7.4/s, 145 B — 360x more efficient than `l2Book` per unit of touch info |
| Book display | `l2Book` (`fast:true` for the active coin) | 5 levels @ ~0.5 s for the traded coin; default 20-level for the depth panel |
| Book aggregation control | `l2Book` + `nSigFigs`/`mantissa` | display only, never execution |
| Tape | `trades` | dedupe on `(time, coin, tid)`; drop the 30-trade backfill on resubscribe |
| Chart | `candle` per selected interval | backfill via REST `candleSnapshot` first |
| Header strip | `activeAssetCtx` | mark, oracle, funding, OI, 24 h vol/change at ~1/s |
| Ticket sizing | `activeAssetData` | `availableToTrade`, `maxTradeSzs`, current leverage/mode |

Account-wide (subscribed once, not per instrument):

| Purpose | Subscription |
|---|---|
| Positions & margin | `clearinghouseState` |
| Resting orders | `openOrders` (bootstrap) + `orderUpdates` (delta; **no snapshot**) |
| Fills | `userFills` (has snapshot) — preferred over `userEvents` for fill accounting |
| Funding accrual | `userFundings` |
| Toasts | `notification` |

Deliberately **not** subscribed: `allMids` (15.8 KB/5 s for data `activeAssetCtx` already
gives per-coin), `allDexsAssetCtxs` (111 KB/msg), `webData3` (only vault/agent metadata now).
