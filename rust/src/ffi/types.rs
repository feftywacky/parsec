use std::os::raw::c_char;
pub const PC_MAX_LEVELS: usize = 24;
pub const PC_COIN_LEN: usize = 16;
pub const PC_ERR_LEN: usize = 192;
pub const PC_ASSET_NONE: u32 = u32::MAX;

// ---- pc_event::kind (include/parsec/parsec.h's PC_EV_* enum) ----
// Named here so call sites (this crate's `ffi` and `codec::info`) never spell out the
// magic numbers directly, matching the header's own comment on the freshly-added
// enums. `transport/ws.rs` (owned by another agent) predates this and still uses
// literals for the kinds it already emits; not touched here.
pub const PC_EV_L2_BOOK: u16 = 1;
pub const PC_EV_BBO: u16 = 2;
pub const PC_EV_TRADE: u16 = 3;
pub const PC_EV_CANDLE: u16 = 4;
pub const PC_EV_ASSET_CTX: u16 = 5;
pub const PC_EV_ASSET_DATA: u16 = 6;
pub const PC_EV_ORDER_UPDATE: u16 = 7;
pub const PC_EV_FILL: u16 = 8;
pub const PC_EV_POSITION: u16 = 9;
pub const PC_EV_ACCOUNT: u16 = 10;
pub const PC_EV_ORDER_ACK: u16 = 11;
pub const PC_EV_CONN: u16 = 12;
pub const PC_EV_RATE: u16 = 13;
pub const PC_EV_ERROR: u16 = 14;
pub const PC_EV_FUNDING: u16 = 15;
pub const PC_EV_FEE_RATES: u16 = 16;
pub const PC_EV_SPOT_BALANCE: u16 = 17;

/// The highest `PC_EV_*` value that counts as market data for `EventQueue`'s
/// drop-oldest-on-overflow policy (docs/02 §4.1) — kinds 1..=5. Everything above this
/// is a user/account event (or connection/rate/error signal) and is never dropped.
pub const PC_EV_MARKET_DATA_MAX: u16 = PC_EV_ASSET_CTX;

// ---- pc_event::flags (PC_F_*) ----
pub const PC_F_SNAPSHOT: u16 = 1 << 0;
pub const PC_F_SNAPSHOT_BEGIN: u16 = 1 << 1;
pub const PC_F_SNAPSHOT_END: u16 = 1 << 2;
/// PC_EV_L2_BOOK only: this snapshot came from the `fast:true` l2Book subscription rather than
/// the default one. Mirrors PC_F_L2_FAST in include/parsec/parsec.h.
pub const PC_F_L2_FAST: u16 = 1 << 5;
/// `PC_EV_ORDER_UPDATE` only: this event describes an order's past, not its present. Set on
/// every `historicalOrders` event so a consumer can tell it from a `frontendOpenOrders` one.
pub const PC_F_HISTORICAL: u16 = 1 << 6;

// ---- pc_order_update::status (PC_ORD_*) ----
pub const PC_ORD_UNKNOWN: u16 = 0;
pub const PC_ORD_OPEN: u16 = 1;
pub const PC_ORD_FILLED: u16 = 2;
pub const PC_ORD_CANCELED: u16 = 3;
pub const PC_ORD_TRIGGERED: u16 = 4;
pub const PC_ORD_REJECTED: u16 = 5;
pub const PC_ORD_MARGIN_CANCELED: u16 = 6;

// ---- pc_fetch's `what` argument (PC_FETCH_*) ----
pub const PC_FETCH_META: u32 = 1;
pub const PC_FETCH_CLEARINGHOUSE_STATE: u32 = 2;
pub const PC_FETCH_OPEN_ORDERS: u32 = 3;
pub const PC_FETCH_USER_FILLS: u32 = 4;
pub const PC_FETCH_USER_FUNDING: u32 = 5;
pub const PC_FETCH_HISTORICAL_ORDERS: u32 = 6;
pub const PC_FETCH_CANDLE_SNAPSHOT: u32 = 7;
pub const PC_FETCH_ACTIVE_ASSET_DATA: u32 = 8;
pub const PC_FETCH_USER_RATE_LIMIT: u32 = 9;
pub const PC_FETCH_USER_FEES: u32 = 10;
pub const PC_FETCH_SPOT_STATE: u32 = 11;

// ---- pc_order_ack::status (PC_ACK_*) ----
pub const PC_ACK_RESTING: u16 = 0;
pub const PC_ACK_FILLED: u16 = 1;
pub const PC_ACK_ERR: u16 = 2;
pub const PC_ACK_WAITING_FOR_FILL: u16 = 3;
pub const PC_ACK_WAITING_FOR_TRIGGER: u16 = 4;
pub const PC_ACK_SUCCESS: u16 = 5;
pub const PC_ACK_TIMEOUT: u16 = 6;

// ---- pc_conn::socket (PC_SOCK_*) ----
pub const PC_SOCK_MARKET: u8 = 0;
pub const PC_SOCK_USER: u8 = 1;

// ---- pc_conn::state (PC_CONN_*) ----
pub const PC_CONN_DISCONNECTED: u8 = 0;
pub const PC_CONN_CONNECTED: u8 = 1;
pub const PC_CONN_RECONNECTING: u8 = 2;

// ---- pc_order::tif (PC_TIF_*) — numerically identical to codec::exchange::TIF_* ----
pub const PC_TIF_GTC: u8 = 0;
pub const PC_TIF_IOC: u8 = 1;
pub const PC_TIF_ALO: u8 = 2;
pub const PC_TIF_FRONTEND_MARKET: u8 = 3;

// ---- pc_fill::dir (PC_DIR_*) ----
pub const PC_DIR_UNKNOWN: u8 = 0;
pub const PC_DIR_OPEN_LONG: u8 = 1;
pub const PC_DIR_CLOSE_LONG: u8 = 2;
pub const PC_DIR_OPEN_SHORT: u8 = 3;
pub const PC_DIR_CLOSE_SHORT: u8 = 4;
pub const PC_DIR_LONG_TO_SHORT: u8 = 5;
pub const PC_DIR_SHORT_TO_LONG: u8 = 6;
pub const PC_DIR_LIQUIDATION: u8 = 7;
pub const PC_DIR_BUY: u8 = 8;
pub const PC_DIR_SELL: u8 = 9;

// ---- pc_order::tpsl (PC_TPSL_*) ----
pub const PC_TPSL_NONE: u8 = 0;
pub const PC_TPSL_TP: u8 = 1;
pub const PC_TPSL_SL: u8 = 2;

// ---- pc_order_req::grouping (PC_GROUP_*) ----
pub const PC_GROUP_NA: u8 = 0;
pub const PC_GROUP_NORMAL_TPSL: u8 = 1;
pub const PC_GROUP_POSITION_TPSL: u8 = 2;

// ---- pc_candle::interval / pc_subscribe's interval argument (PC_IV_*) ----
// PC_IV_1S..PC_IV_30S have no venue equivalent -- `candleSnapshot` and the `candle`
// subscription both reject "1s".."30s" -- so nothing in this crate ever maps them to a wire
// string. They exist in the id space only because the C++ side folds them locally from the
// trades stream; see include/parsec/parsec.h.
pub const PC_IV_1S: u8 = 0;
pub const PC_IV_5S: u8 = 1;
pub const PC_IV_10S: u8 = 2;
pub const PC_IV_30S: u8 = 3;
pub const PC_IV_1M: u8 = 4;
pub const PC_IV_5M: u8 = 5;
pub const PC_IV_15M: u8 = 6;
pub const PC_IV_30M: u8 = 7;
pub const PC_IV_1H: u8 = 8;
pub const PC_IV_4H: u8 = 9;
pub const PC_IV_1D: u8 = 10;
pub const PC_IV_COUNT: u8 = 11;
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PcConfig {
    pub abi_version: u32,
    pub mainnet: bool,
    pub keystore_path: [c_char; 512],
    pub passphrase: [c_char; 256],
    pub event_queue_capacity: u32,
    pub io_worker_threads: u32,
}
#[repr(C)]
#[derive(Copy, Clone, Default)]
pub struct PcLevel {
    pub px: i64,
    pub sz: i64,
    pub n: u32,
    pub _pad: u32,
}
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PcL2 {
    pub n_bid: u8,
    pub n_ask: u8,
    pub _pad0: u16,
    pub _pad1: u32,
    pub bids: [PcLevel; PC_MAX_LEVELS],
    pub asks: [PcLevel; PC_MAX_LEVELS],
}
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PcBbo {
    pub bid: PcLevel,
    pub ask: PcLevel,
    pub has_bid: bool,
    pub has_ask: bool,
}
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PcTrade {
    pub px: i64,
    pub sz: i64,
    pub tid: u64,
    /// Venue block clock for this print; see the note in include/parsec/parsec.h.
    pub time_ms: u64,
    pub is_buy: u8,
}
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PcCandle {
    pub open_ms: u64,
    pub close_ms: u64,
    pub o: i64,
    pub h: i64,
    pub l: i64,
    pub c: i64,
    pub v: i64,
    pub n: u32,
    pub interval: u8,
}
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PcAssetCtx {
    pub mark: i64,
    pub oracle: i64,
    pub mid: i64,
    pub prev_day: i64,
    pub day_ntl_vlm: i64,
    pub open_interest: i64,
    pub funding_1e8: i64,
}
#[repr(C)]
#[derive(Copy, Clone)]
/// Mirrors `pc_asset_data`: max trade values are base quantities; available values are USDC
/// notionals from the venue's `availableToTrade` response.
pub struct PcAssetData {
    pub max_trade_buy: i64,
    pub max_trade_sell: i64,
    pub avail_buy: i64,
    pub avail_sell: i64,
    pub mark: i64,
    pub leverage: u32,
    pub is_cross: u8,
}
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PcFeeRates {
    /// Effective maker/add rate, scaled by 1e8; negative means a rebate.
    pub maker_rate: i64,
    /// Effective taker/cross rate, scaled by 1e8.
    pub taker_rate: i64,
}
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PcOrderUpdate {
    pub oid: u64,
    pub cloid: [u8; 16],
    pub status: u16,
    pub px: i64,
    pub sz: i64,
    pub orig_sz: i64,
    /// Where a trigger order actually rests. `px` is the limit it converts to once it
    /// fires, which for a market trigger is deliberately far from the market.
    pub trigger_px: i64,
    pub is_buy: u8,
    pub reduce_only: u8,
    pub is_trigger: u8,
    /// `PC_TPSL_*`: which leg a trigger order is. `PC_TPSL_NONE` for a plain order.
    pub tpsl: u8,
    /// Whether a trigger order converts to a market order (rather than a limit) when it
    /// fires. Together with `tpsl` this is what names the order type in a history table.
    pub is_market_trigger: u8,
}
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PcFill {
    pub oid: u64,
    pub tid: u64,
    pub cloid: [u8; 16],
    pub px: i64,
    pub qty: i64,
    pub fee: i64,
    pub closed_pnl: i64,
    pub is_buy: u8,
    pub is_taker: u8,
    /// `PC_DIR_*`: what the fill did to the position, from the venue's `dir` string.
    pub dir: u8,
}
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PcPosition {
    pub szi: i64,
    pub entry_px: i64,
    pub liq_px: i64,
    pub position_value: i64,
    pub unrealized_pnl: i64,
    pub margin_used: i64,
    pub cum_funding: i64,
    pub roe_bps: i32,
    pub leverage: u32,
    pub is_cross: u8,
}
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PcAccount {
    /// `marginSummary.accountValue` — cross **and** isolated.
    pub account_value: i64,
    /// `crossMarginSummary.accountValue` — the equity that actually backs cross positions.
    pub cross_account_value: i64,
    pub total_margin_used: i64,
    pub total_ntl_pos: i64,
    pub withdrawable: i64,
    pub cross_maintenance_margin: i64,
}
/// The USDC row of `spotClearinghouseState`. See `pc_spot` in include/parsec/parsec.h:
/// `total` already contains the equity deployed in perps, it is not money beside it.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PcSpot {
    pub total: i64,
    pub hold: i64,
}
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PcOrderAck {
    pub status: u16,
    pub oid: u64,
    pub filled_sz: i64,
    pub avg_px: i64,
    pub err: [c_char; PC_ERR_LEN],
}
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PcConn {
    pub socket: u8,
    pub state: u8,
    pub reconnects: u32,
    /// Last measured ping/pong round trip in microseconds; 0 when not yet measured.
    pub rtt_us: u32,
}
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PcRate {
    pub remaining: i64,
    pub reset_ms: u64,
}
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PcError {
    pub code: i32,
    pub msg: [c_char; PC_ERR_LEN],
}
/// Funding payment. Mirrors `pc_funding`; see the note in include/parsec/parsec.h for why
/// this is not a `PcFill` with reinterpreted fields.
#[repr(C)]
#[derive(Copy, Clone, Default)]
pub struct PcFunding {
    /// Signed: negative is paid, positive is received.
    pub usdc: i64,
    pub szi: i64,
    pub rate_1e8: i64,
    pub n_samples: u32,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub union PcEventUnion {
    pub l2: PcL2,
    pub bbo: PcBbo,
    pub trade: PcTrade,
    pub candle: PcCandle,
    pub asset_ctx: PcAssetCtx,
    pub asset_data: PcAssetData,
    pub fee_rates: PcFeeRates,
    pub order_update: PcOrderUpdate,
    pub fill: PcFill,
    pub position: PcPosition,
    pub account: PcAccount,
    pub spot: PcSpot,
    pub ack: PcOrderAck,
    pub conn: PcConn,
    pub rate: PcRate,
    pub error: PcError,
    pub funding: PcFunding,
}
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PcEvent {
    pub kind: u16,
    pub flags: u16,
    pub asset: u32,
    pub req_id: u64,
    pub exch_time_ms: u64,
    pub recv_time_ns: u64,
    pub u: PcEventUnion,
}
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PcOrder {
    pub asset: u32,
    pub is_buy: u8,
    pub reduce_only: u8,
    pub tif: u8,
    pub tpsl: u8,
    pub limit_px: i64,
    pub sz: i64,
    pub trigger_px: i64,
    pub is_market_trigger: u8,
    pub cloid: [u8; 16],
}
#[repr(C)]
#[derive(Copy, Clone)]
pub struct PcOrderReq {
    pub grouping: u8,
    pub n_orders: u8,
    pub orders: [PcOrder; 4],
}
impl PcEvent {
    pub fn error(code: i32, message: &str, request: u64) -> Self {
        let mut msg = [0; PC_ERR_LEN];
        for (dst, src) in msg.iter_mut().zip(message.as_bytes()) {
            *dst = *src as c_char
        }
        Self {
            kind: PC_EV_ERROR,
            flags: 0,
            asset: PC_ASSET_NONE,
            req_id: request,
            exch_time_ms: 0,
            recv_time_ns: crate::ffi::monotonic_ns(),
            u: PcEventUnion {
                error: PcError { code, msg },
            },
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::{align_of, offset_of, size_of};
    #[test]
    fn abi_layout() {
        assert_eq!(size_of::<PcLevel>(), 24);
        assert_eq!(size_of::<PcFeeRates>(), 16);
        assert_eq!(size_of::<PcAccount>(), 48);
        assert_eq!(size_of::<PcSpot>(), 16);
        assert_eq!(size_of::<PcEvent>(), 1192);
        assert_eq!(offset_of!(PcEvent, u), 32);
        assert_eq!(align_of::<PcEvent>(), 8);
        assert_eq!(size_of::<PcConfig>(), 784);
    }
}
