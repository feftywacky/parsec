#ifndef PARSEC_H
#define PARSEC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PC_ABI_VERSION 3u
#define PC_COIN_LEN 16
#define PC_ERR_LEN 192
#define PC_MAX_LEVELS 24
#define PC_ASSET_NONE 0xFFFFFFFFu

typedef struct pc_engine pc_engine;
typedef uint64_t pc_req_id;
typedef int64_t pc_px;
typedef int64_t pc_qty;
typedef int64_t pc_usd;

typedef struct {
    uint32_t abi_version;
    bool mainnet;
    char keystore_path[512];
    char passphrase[256];
    uint32_t event_queue_capacity;
    uint32_t io_worker_threads;
} pc_config;

enum {
    PC_STREAM_BBO = 1u << 0, PC_STREAM_L2 = 1u << 1,
    PC_STREAM_L2_FAST = 1u << 2, PC_STREAM_TRADES = 1u << 3,
    PC_STREAM_ASSET_CTX = 1u << 4, PC_STREAM_ASSET_DATA = 1u << 5,
    PC_STREAM_CANDLE = 1u << 6,
};
enum {
    PC_EV_L2_BOOK = 1, PC_EV_BBO, PC_EV_TRADE, PC_EV_CANDLE, PC_EV_ASSET_CTX,
    PC_EV_ASSET_DATA, PC_EV_ORDER_UPDATE, PC_EV_FILL, PC_EV_POSITION, PC_EV_ACCOUNT,
    PC_EV_ORDER_ACK, PC_EV_CONN, PC_EV_RATE, PC_EV_ERROR, PC_EV_FUNDING, PC_EV_FEE_RATES,
    PC_EV_SPOT_BALANCE,
};
enum {
    PC_F_SNAPSHOT = 1u << 0, PC_F_SNAPSHOT_BEGIN = 1u << 1,
    PC_F_SNAPSHOT_END = 1u << 2, PC_F_STALE = 1u << 3, PC_F_CLOSED = 1u << 4,
    /* PC_EV_L2_BOOK only: this snapshot came from the `fast:true` l2Book subscription (5
       levels, ~530ms cadence) rather than the default one (20 levels, ~5.4s). Both arrive on
       the venue's single `l2Book` channel with nothing in the payload to tell them apart, so
       the Rust edge classifies by depth -- see rust/src/transport/ws.rs. */
    PC_F_L2_FAST = 1u << 5,
    /* PC_EV_ORDER_UPDATE only: this event describes an order's PAST, not its present. Both
       `frontendOpenOrders` (what is resting right now) and `historicalOrders` (everything
       that ever happened) arrive as PC_EV_ORDER_UPDATE snapshot batches, and they answer
       different questions -- without this bit a consumer cannot tell an order that is still
       resting from one that resolved days ago, and a history backfill silently repopulates
       the open-orders list. Set on every `historicalOrders` event and on nothing else. */
    PC_F_HISTORICAL = 1u << 6,
};

/* Candle interval ids. Passed as pc_subscribe's `interval` argument alongside
   PC_STREAM_CANDLE, and echoed back in pc_candle::interval. */
enum {
    PC_IV_1S = 0, PC_IV_5S, PC_IV_10S, PC_IV_30S,
    PC_IV_1M, PC_IV_5M, PC_IV_15M, PC_IV_30M, PC_IV_1H, PC_IV_4H, PC_IV_1D,
    PC_IV_COUNT,
};
/* The venue has no sub-minute candles -- `candleSnapshot`/`candle` reject "1s".."30s"
   outright -- so PC_IV_1S..PC_IV_30S are never subscribed or backfilled. They are folded
   locally from the `trades` stream instead (md::CandleSeries::fold_trade), which means they
   carry no history: they begin at the first print after the subscription opens. Everything
   from PC_IV_1M up is the venue's own candle feed. */
#define PC_IV_FIRST_VENUE PC_IV_1M

/* pc_order_update::status. Mirrors the venue's orderUpdates status strings
   (03 SS W2.7); anything parsec does not model arrives as PC_ORD_UNKNOWN. */
enum {
    PC_ORD_UNKNOWN = 0, PC_ORD_OPEN, PC_ORD_FILLED, PC_ORD_CANCELED,
    PC_ORD_TRIGGERED, PC_ORD_REJECTED, PC_ORD_MARGIN_CANCELED,
};

/* pc_order_ack::status. PC_ACK_ERR means the venue accepted the request but
   rejected this order: `err` carries the per-element string (03 SS 8). */
enum {
    PC_ACK_RESTING = 0, PC_ACK_FILLED, PC_ACK_ERR, PC_ACK_WAITING_FOR_FILL,
    PC_ACK_WAITING_FOR_TRIGGER, PC_ACK_SUCCESS, PC_ACK_TIMEOUT,
};

/* pc_conn::socket and pc_conn::state. */
enum { PC_SOCK_MARKET = 0, PC_SOCK_USER };
enum { PC_CONN_DISCONNECTED = 0, PC_CONN_CONNECTED, PC_CONN_RECONNECTING };

/* pc_fetch's `what` argument: a one-shot REST pull whose result arrives as
   ordinary events carrying the same req_id. */
enum {
    PC_FETCH_META = 1, PC_FETCH_CLEARINGHOUSE_STATE, PC_FETCH_OPEN_ORDERS,
    PC_FETCH_USER_FILLS, PC_FETCH_USER_FUNDING, PC_FETCH_HISTORICAL_ORDERS,
    PC_FETCH_CANDLE_SNAPSHOT, PC_FETCH_ACTIVE_ASSET_DATA, PC_FETCH_USER_RATE_LIMIT,
    PC_FETCH_USER_FEES, PC_FETCH_SPOT_STATE,
};

typedef struct { pc_px px; pc_qty sz; uint32_t n; uint32_t _pad; } pc_level;
typedef struct {
    uint8_t n_bid, n_ask; uint16_t _pad0; uint32_t _pad1;
    pc_level bids[PC_MAX_LEVELS]; pc_level asks[PC_MAX_LEVELS];
} pc_l2;
typedef struct { pc_level bid, ask; bool has_bid, has_ask; } pc_bbo;
/* `time_ms` is the venue block clock for this print. It is part of the trade, not just
   of the event that carried it: TradeTape stores pc_trade values, so a tape entry
   without it cannot render a time column. */
typedef struct { pc_px px; pc_qty sz; uint64_t tid; uint64_t time_ms; uint8_t is_buy; } pc_trade;
typedef struct { uint64_t open_ms, close_ms; pc_px o,h,l,c; pc_qty v; uint32_t n; uint8_t interval; } pc_candle;
typedef struct { pc_px mark,oracle,mid,prev_day; pc_usd day_ntl_vlm,open_interest; int64_t funding_1e8; } pc_asset_ctx;
/* `max_trade_*` are base-asset quantities; `avail_*` mirror `availableToTrade` and are
   side-specific USDC notional budgets. */
typedef struct { pc_qty max_trade_buy,max_trade_sell; pc_usd avail_buy,avail_sell; pc_px mark; uint32_t leverage; uint8_t is_cross; } pc_asset_data;
/* Effective account fee rates from `userFees`. Both values are fractions on the shared
   1e8 grid: 0.00045 (0.045%) is 45000. `maker_rate` may be negative when the account
   receives a maker rebate. */
typedef struct { int64_t maker_rate, taker_rate; } pc_fee_rates;
/* `trigger_px` and `tpsl` (PC_TPSL_*) are populated only for trigger orders, i.e. a resting
   take-profit or stop-loss. For those, `px` is the LIMIT price the order becomes once it
   fires -- for a market trigger that is a slippage-padded price far from where the order
   actually rests, so `trigger_px` is the only price that means "where this order sits".
   The `orderUpdates` WS payload carries none of these fields (03 SS W2.7); they arrive with
   the `frontendOpenOrders` snapshot the reconciler re-pulls. */
typedef struct { uint64_t oid; uint8_t cloid[16]; uint16_t status; pc_px px; pc_qty sz,orig_sz; pc_px trigger_px; uint8_t is_buy,reduce_only,is_trigger,tpsl,is_market_trigger; } pc_order_update;
/* pc_fill::dir. The venue's own `dir` string on `userFills` (03 SS W2.9), which says what the
   fill DID to the position -- opened it, closed it, or flipped it -- and is not derivable from
   the side alone: a sell is an opening short or a closing long depending on what was held. */
enum {
    PC_DIR_UNKNOWN = 0, PC_DIR_OPEN_LONG, PC_DIR_CLOSE_LONG, PC_DIR_OPEN_SHORT,
    PC_DIR_CLOSE_SHORT, PC_DIR_LONG_TO_SHORT, PC_DIR_SHORT_TO_LONG, PC_DIR_LIQUIDATION,
    PC_DIR_BUY, PC_DIR_SELL
};
typedef struct { uint64_t oid,tid; uint8_t cloid[16]; pc_px px; pc_qty qty; pc_usd fee,closed_pnl; uint8_t is_buy,is_taker,dir; } pc_fill;
typedef struct { pc_qty szi; pc_px entry_px,liq_px; pc_usd position_value,unrealized_pnl,margin_used,cum_funding; int32_t roe_bps; uint32_t leverage; uint8_t is_cross; } pc_position;
/* `account_value`, `total_margin_used` and `total_ntl_pos` are `marginSummary` -- cross AND
   isolated. `cross_account_value` is `crossMarginSummary.accountValue`, i.e. the same equity
   with every isolated position's margin and PnL removed; it is the only one of the two that
   belongs in a cross-margin liquidation calculation, since isolated equity cannot be pulled in
   to defend a cross position. `cross_maintenance_margin` is likewise cross-only, so it pairs
   with `cross_account_value` and NOT with `account_value`. */
typedef struct { pc_usd account_value,cross_account_value,total_margin_used,total_ntl_pos,withdrawable,cross_maintenance_margin; } pc_account;
/* The account's USDC spot row (`spotClearinghouseState`). Hyperliquid runs a single USDC
   collateral pool: `total` is every USDC the account holds anywhere, and the part currently
   deployed as perp equity is counted inside it, so `total` is NOT separate money from
   `pc_account::account_value` -- it CONTAINS it. `hold` is what the venue reports as locked;
   the venue's own Balances tab shows `total` and `total - hold`.
   USDC only: it is the sole perp collateral, and the panels that consume this need a
   collateral figure rather than a spot portfolio. */
typedef struct { pc_usd total, hold; } pc_spot;
typedef struct { uint16_t status; uint64_t oid; pc_qty filled_sz; pc_px avg_px; char err[PC_ERR_LEN]; } pc_order_ack;
/* `rtt_us` is the last measured WebSocket ping/pong round trip in microseconds, 0 when not
   yet measured. It is the only latency figure that is actually a round trip -- the per-feed
   cadence numbers elsewhere describe how often the venue pushes, not how long a packet takes. */
typedef struct { uint8_t socket,state; uint32_t reconnects, rtt_us; } pc_conn;
typedef struct { int64_t remaining; uint64_t reset_ms; } pc_rate;
typedef struct { int32_t code; char msg[PC_ERR_LEN]; } pc_error;
/* A funding payment. Deliberately its own struct rather than a pc_fill with remapped
   fields: overloading `fee` to mean "funding paid" and `px` to mean "rate" is exactly the
   kind of reinterpretation that reads correctly and computes wrongly. `usdc` is signed --
   negative is paid, positive is received. */
typedef struct {
    pc_usd usdc; pc_qty szi; int64_t rate_1e8; uint32_t n_samples;
} pc_funding;

typedef struct {
    uint16_t kind, flags; uint32_t asset; pc_req_id req_id;
    uint64_t exch_time_ms, recv_time_ns;
    union {
        pc_l2 l2; pc_bbo bbo; pc_trade trade; pc_candle candle; pc_asset_ctx asset_ctx;
        pc_asset_data asset_data; pc_fee_rates fee_rates; pc_order_update order_update; pc_fill fill;
        pc_position position; pc_account account; pc_spot spot; pc_order_ack ack; pc_conn conn;
        pc_rate rate; pc_error error; pc_funding funding;
    } u;
} pc_event;

enum { PC_TIF_GTC = 0, PC_TIF_IOC, PC_TIF_ALO, PC_TIF_FRONTEND_MARKET };
enum { PC_TPSL_NONE = 0, PC_TPSL_TP, PC_TPSL_SL };
enum { PC_GROUP_NA = 0, PC_GROUP_NORMAL_TPSL, PC_GROUP_POSITION_TPSL };
typedef struct {
    uint32_t asset; uint8_t is_buy, reduce_only, tif, tpsl;
    pc_px limit_px; pc_qty sz; pc_px trigger_px; uint8_t is_market_trigger;
    uint8_t cloid[16];
} pc_order;
typedef struct { uint8_t grouping, n_orders; pc_order orders[4]; } pc_order_req;

pc_engine* pc_engine_create(const pc_config* cfg);
void pc_engine_destroy(pc_engine*);
uint32_t pc_abi_version(void);
int32_t pc_last_error(pc_engine*, char* out, uint32_t cap);
int32_t pc_poll(pc_engine*, pc_event* out, uint32_t max, uint64_t timeout_ns);
pc_req_id pc_subscribe(pc_engine*, const char* coin, uint32_t mask, uint8_t interval);
pc_req_id pc_unsubscribe(pc_engine*, const char* coin, uint32_t mask, uint8_t interval);
/* Book-granularity-aware variants for the l2Book streams (PC_STREAM_L2 / PC_STREAM_L2_FAST).
   `n_sig_figs` < 0 and `mantissa` == 0 mean the venue's native granularity; otherwise the
   venue aggregates the book server-side and still returns its full depth at the coarser price
   step, which is the only way to fill a deep ladder (5-significant-figure levels span only
   ~$19 on BTC). An unsubscribe must pass the SAME granularity the subscribe used -- the venue
   matches on the exact payload. */
pc_req_id pc_subscribe_book(pc_engine*, const char* coin, uint32_t mask, int8_t n_sig_figs,
                            uint8_t mantissa);
pc_req_id pc_unsubscribe_book(pc_engine*, const char* coin, uint32_t mask, int8_t n_sig_figs,
                              uint8_t mantissa);
pc_req_id pc_place_order(pc_engine*, const pc_order_req*);
pc_req_id pc_cancel_order(pc_engine*, uint32_t asset, uint64_t oid);
pc_req_id pc_cancel_by_cloid(pc_engine*, uint32_t asset, const uint8_t cloid[16]);
pc_req_id pc_modify_order(pc_engine*, uint64_t oid, const pc_order*);
pc_req_id pc_cancel_all(pc_engine*);
pc_req_id pc_set_leverage(pc_engine*, uint32_t asset, bool is_cross, uint32_t leverage);
pc_req_id pc_set_iso_margin(pc_engine*, uint32_t asset, pc_usd usd_delta);
pc_req_id pc_schedule_cancel(pc_engine*, uint64_t deadline_ms);
pc_req_id pc_fetch(pc_engine*, uint32_t what, const char* coin);
/* Candle snapshots use the same one-shot fetch path but carry the requested interval
   explicitly. `pc_fetch` remains available for the other fetch kinds and defaults candle
   snapshots to 1m for ABI compatibility. */
pc_req_id pc_fetch_candle_snapshot(pc_engine*, const char* coin, uint8_t interval);
/* ---------------------------------------------------------------------------------
   Account connection: agent-wallet onboarding and interactive unlock (docs/06 §2).

   parsec never stores your MetaMask master key. It stores an *agent wallet* key: a
   separate keypair your master key approves once, which can trade the account but
   cannot withdraw from it (docs/06 §1). So "connecting an account" is two things —
   a one-time `parsec setup` that approves an agent and writes an encrypted keystore,
   and a per-session unlock of that keystore with a passphrase.
   --------------------------------------------------------------------------------- */

/* --- one-time onboarding, `parsec setup`. Engine-free: no pc_engine exists yet. --- */

typedef struct pc_setup pc_setup;

#define PC_ADDR_STR_CAP 43  /* "0x" + 40 hex + NUL */

/* Generate a fresh agent keypair and fix this approval's parameters. The private key
   lives inside the returned handle and never crosses this boundary — there is no call
   to retrieve it, by design (docs/06 §5 rule 1).

   `agent_name` may be NULL for the default `parsec-<host>-<id>`; the ` valid_until <ms>`
   suffix the venue enforces is appended automatically. `lifetime_ms` may be 0 for the
   180-day default, and is clamped to that maximum. Always a NEW keypair: an agent
   address is never reused, because the venue may prune a deregistered agent's nonce
   state and make old signed actions replayable (docs/06 §1). */
pc_setup* pc_setup_begin(bool mainnet, const char* agent_name, uint64_t lifetime_ms);
void pc_setup_free(pc_setup*);
int32_t pc_setup_last_error(pc_setup*, char* out, uint32_t cap);

int32_t pc_setup_agent_address(pc_setup*, char* out, uint32_t cap);
int32_t pc_setup_agent_name(pc_setup*, char* out, uint32_t cap);
/* Known only after one of the two approval paths below: it is *recovered from the
   signature*, never supplied by the caller. Returns -1 before then. */
int32_t pc_setup_master_address(pc_setup*, char* out, uint32_t cap);
uint64_t pc_setup_valid_until_ms(pc_setup*);

/* Approve the agent: the master key exists here for the duration of one signature (docs/06
   §2 steps 2-5). `master_sk_hex` is a WRITABLE buffer holding the key as hex; it is zeroed
   before this function returns on every path, including every error path. The master address
   is then recovered from the signature, never derived from the key. */
int32_t pc_setup_sign_with_master(pc_setup*, char* master_sk_hex);

/* POST the approval to /exchange. BLOCKS. Returns 0 only on {"status":"ok"}. */
int32_t pc_setup_submit(pc_setup*);

/* Seal the agent key and write ~/.parsec/keystore-<network>.json, mode 0600 (parent
   dir 0700). Refuses unless pc_setup_submit succeeded, and refuses to overwrite an
   existing keystore. BLOCKS for ~3.5s in Argon2id — never call it on a UI thread. */
int32_t pc_setup_write_keystore(pc_setup*, const char* passphrase, const char* path);

/* --- per-session unlock --- */

/* pc_auth_status values. PC_AUTH_NO_KEYSTORE means no keystore is configured at all —
   surface that as "connect an account", not as a passphrase prompt. PC_AUTH_EXPIRED means
   one exists but its agent approval has lapsed; no passphrase can open it, so surface a
   re-approval, not a prompt. Both are reported from the keystore's cleartext header,
   before anything is typed. */
enum {
    PC_AUTH_NO_KEYSTORE = 0, PC_AUTH_LOCKED, PC_AUTH_UNLOCKING,
    PC_AUTH_UNLOCKED, PC_AUTH_FAILED, PC_AUTH_EXPIRED,
};
int32_t pc_auth_status(pc_engine*);

/* Start an unlock attempt. Returns immediately — Argon2id runs on a blocking task, so
   poll pc_auth_status for the outcome (the reason for a failure also arrives as a
   PC_EV_ERROR event). `passphrase` is a WRITABLE buffer and is zeroed before return. */
int32_t pc_unlock(pc_engine*, char* passphrase);

/* Point the engine at a keystore created after startup (the in-app connect flow). Refused
   once unlocked. Returns 0 on success. */
int32_t pc_set_keystore_path(pc_engine*, const char* path);

/* Delete every local keystore (both networks) and return the engine to its fresh-install
   state, so the UI shows "connect an account" next. Returns the number of files removed,
   or -1 if refused (already unlocked/unlocking) or a delete failed — see pc_last_error.
   LOCAL ONLY: the agent approval still stands at the venue until it lapses; revoking it
   needs the master key parsec never stores. The deleted key was fundless regardless. */
int32_t pc_reset_accounts(pc_engine*);

/* The unlocked account's addresses and agent expiry. -1 until unlocked. The two
   addresses are not interchangeable: `master` holds the funds and is what every /info
   query uses, `agent` is the fundless key that signs (docs/06 §1). */
int32_t pc_auth_addresses(pc_engine*, char* master_out, uint32_t master_cap,
                          char* agent_out, uint32_t agent_cap, uint64_t* valid_until_ms);

/* Read a keystore's cleartext header without a passphrase, so an unlock prompt can name
   the account and warn about a near-expiry agent before anything is typed. Every field
   here is already cleartext in the file and AAD-bound, so it cannot be altered without
   making the keystore undecryptable. */
int32_t pc_keystore_peek(const char* path, char* master_out, uint32_t master_cap,
                         char* agent_out, uint32_t agent_cap, uint64_t* valid_until_ms,
                         bool* is_mainnet);

int32_t pc_asset_count(pc_engine*);
int32_t pc_asset_info(pc_engine*, uint32_t asset, char* name_out, uint32_t cap,
                      uint8_t* sz_decimals, uint32_t* max_leverage, uint8_t* only_isolated);

#ifdef __cplusplus
}
#endif
#endif
