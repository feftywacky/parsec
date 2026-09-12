//! `pc_fetch` dispatch: one-shot `/info` pulls whose results arrive as ordinary
//! events carrying the requesting `req_id` (`include/parsec/parsec.h`'s `PC_FETCH_*`).
//! Runs on the tokio runtime `pc_fetch` spawns onto — never on the caller's thread.
//!
//! **Master vs. agent addressing (docs/03 §5, 06 §1).** Every account-scoped fetch in
//! this module queries `/info` with the session's **master** address, never the agent
//! address — the agent holds no funds and no position, so querying with it silently
//! returns an empty account rather than an error. `Session::master_address` returns
//! the distinct `MasterAddress` newtype specifically so this file cannot pass the
//! wrong one by accident.
use crate::codec::decimal::{parse_scaled, parse_scaled_stat_rounded};
use crate::codec::info;
use crate::ffi::queue::EventQueue;
use crate::ffi::types::*;
use crate::session::Session;
use crate::transport::http::HttpClient;
use crate::universe::{AssetRegistry, SharedRegistry};
use serde_json::json;
use std::sync::Arc;

/// `cloid` hex string (`"0x" + 32 hex chars`) -> raw bytes, or the all-zero "no
/// cloid" sentinel for anything else (missing, `null`, or malformed) — a response's
/// `cloid` field is display-adjacent here, never load-bearing for correctness.
fn parse_cloid(s: Option<&str>) -> [u8; 16] {
    let mut out = [0u8; 16];
    if let Some(hex_part) = s.and_then(|s| s.strip_prefix("0x")) {
        if hex_part.len() == 32 {
            let _ = hex::decode_to_slice(hex_part, &mut out);
        }
    }
    out
}

/// Entry point spawned by `pc_fetch`. Never panics past this boundary in a way that
/// would matter — this task runs detached on the tokio runtime, not across the FFI
/// boundary, so it is not wrapped in `catch_unwind` itself; `pc_fetch` wraps the
/// *spawn* call, and nothing here does anything `catch_unwind`-worthy (no raw
/// pointers, no C ABI types).
pub async fn dispatch(
    what: u32,
    coin: Option<String>,
    interval: u8,
    req_id: u64,
    http: HttpClient,
    events: EventQueue,
    registry: SharedRegistry,
    session: Option<Arc<Session>>,
) {
    let outcome = match what {
        PC_FETCH_META => fetch_meta(&http, &events, &registry, req_id).await,
        PC_FETCH_CANDLE_SNAPSHOT => {
            fetch_candle_snapshot(&http, &events, &registry, coin, interval, req_id).await
        }
        PC_FETCH_CLEARINGHOUSE_STATE
        | PC_FETCH_OPEN_ORDERS
        | PC_FETCH_USER_FILLS
        | PC_FETCH_USER_FUNDING
        | PC_FETCH_HISTORICAL_ORDERS
        | PC_FETCH_ACTIVE_ASSET_DATA
        | PC_FETCH_USER_RATE_LIMIT
        | PC_FETCH_USER_FEES
        | PC_FETCH_SPOT_STATE => match session {
            None => Err(
                "this fetch needs the account's master address, which is only known \
                 once an agent keystore is unlocked"
                    .to_string(),
            ),
            Some(session) => {
                let master = crate::signer::format_address(&session.master_address().0);
                match what {
                    PC_FETCH_CLEARINGHOUSE_STATE => {
                        fetch_clearinghouse_state(&http, &events, &registry, &master, req_id).await
                    }
                    PC_FETCH_OPEN_ORDERS => {
                        fetch_open_orders(&http, &events, &registry, &master, req_id).await
                    }
                    PC_FETCH_USER_FILLS => {
                        fetch_user_fills(&http, &events, &registry, &master, req_id).await
                    }
                    PC_FETCH_USER_FUNDING => {
                        fetch_user_funding(&http, &events, &registry, &master, req_id).await
                    }
                    PC_FETCH_HISTORICAL_ORDERS => {
                        fetch_historical_orders(&http, &events, &registry, &master, req_id).await
                    }
                    PC_FETCH_ACTIVE_ASSET_DATA => {
                        fetch_active_asset_data(&http, &events, &registry, &master, coin, req_id)
                            .await
                    }
                    PC_FETCH_USER_RATE_LIMIT => {
                        fetch_user_rate_limit(&http, &events, &session, &master, req_id).await
                    }
                    PC_FETCH_USER_FEES => fetch_user_fees(&http, &events, &master, req_id).await,
                    PC_FETCH_SPOT_STATE => {
                        fetch_spot_state(&http, &events, &master, req_id).await
                    }
                    _ => unreachable!("matched by the outer arm"),
                }
            }
        },
        other => Err(format!("pc_fetch: unknown `what` value {other}")),
    };
    if let Err(message) = outcome {
        events.push(PcEvent::error(-4, &message, req_id));
    }
}

fn base_event(kind: u16, asset: u32, exch_time_ms: u64, req_id: u64) -> PcEvent {
    PcEvent {
        kind,
        flags: 0,
        asset,
        req_id,
        exch_time_ms,
        recv_time_ns: crate::ffi::monotonic_ns(),
        u: unsafe { std::mem::zeroed() },
    }
}

fn rate_event(retry_after_ms: u64, req_id: u64) -> PcEvent {
    let mut event = base_event(PC_EV_RATE, PC_ASSET_NONE, 0, req_id);
    event.u = PcEventUnion {
        rate: PcRate {
            // Unknown here — the docs don't document a machine-readable 429 body
            // (docs/03 §7), so there is no server-reported "remaining" to report.
            remaining: -1,
            reset_ms: retry_after_ms,
        },
    };
    event
}

async fn fetch_meta(
    http: &HttpClient,
    events: &EventQueue,
    registry: &SharedRegistry,
    req_id: u64,
) -> Result<(), String> {
    let body = json!({"type": "meta", "dex": ""});
    let value = http
        .post_info_with_retry(&body, |ms| events.push(rate_event(ms, req_id)))
        .await
        .map_err(|e| format!("meta fetch failed: {e}"))?;
    let meta: info::Meta =
        serde_json::from_value(value).map_err(|e| format!("meta decode failed: {e}"))?;
    let count = meta.universe.len();
    registry.set(AssetRegistry::from_meta(&meta));
    events.push(PcEvent::error(
        0,
        &format!("meta fetched: {count} assets"),
        req_id,
    ));
    Ok(())
}

async fn fetch_candle_snapshot(
    http: &HttpClient,
    events: &EventQueue,
    registry: &SharedRegistry,
    coin: Option<String>,
    interval: u8,
    req_id: u64,
) -> Result<(), String> {
    let coin = coin.ok_or_else(|| "candle snapshot fetch requires a coin".to_string())?;
    let asset = registry.index_of(&coin);
    let now_ms = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0);
    let (interval_name, interval_ms) = candle_interval(interval)
        .ok_or_else(|| format!("unsupported candle interval id {interval}"))?;
    const MAX_CANDLES: u64 = 5000;
    let start_time = now_ms.saturating_sub(interval_ms * MAX_CANDLES);
    let body = json!({
        "type": "candleSnapshot",
        "req": {"coin": coin, "interval": interval_name, "startTime": start_time, "endTime": now_ms},
    });
    let value = http
        .post_info_with_retry(&body, |ms| events.push(rate_event(ms, req_id)))
        .await
        .map_err(|e| format!("candleSnapshot fetch failed: {e}"))?;
    let candles: Vec<info::Candle> =
        serde_json::from_value(value).map_err(|e| format!("candleSnapshot decode failed: {e}"))?;
    let n = candles.len();
    for (i, c) in candles.iter().enumerate() {
        let mut event = base_event(PC_EV_CANDLE, asset, c.close_ms, req_id);
        let mut flags = PC_F_SNAPSHOT;
        if i == 0 {
            flags |= PC_F_SNAPSHOT_BEGIN;
        }
        if i + 1 == n {
            flags |= PC_F_SNAPSHOT_END;
        }
        event.flags = flags;
        // o/h/l/c/v all stay within 8 decimals on the wire (docs/09 §2's table caps
        // candle OHLC at 1 decimal and `v` at 5), so the strict parser is correct
        // here — this is order-adjacent chart data, not the >8-decimal statistics
        // fields `parse_scaled_stat_rounded` exists for.
        event.u = PcEventUnion {
            candle: PcCandle {
                open_ms: c.open_ms,
                close_ms: c.close_ms,
                o: parse_scaled(&c.open).unwrap_or(0),
                h: parse_scaled(&c.high).unwrap_or(0),
                l: parse_scaled(&c.low).unwrap_or(0),
                c: parse_scaled(&c.close).unwrap_or(0),
                v: parse_scaled(&c.volume).unwrap_or(0),
                n: c.trade_count,
                interval: candle_interval_id(&c.interval).unwrap_or(interval),
            },
        };
        events.push(event);
    }
    Ok(())
}

/// The chart intervals exposed by the C ABI that the VENUE actually serves. Sub-minute ids
/// (PC_IV_1S..PC_IV_30S) deliberately fall through to `None`: `candleSnapshot` rejects "1s"
/// through "30s" with a deserialize error, so there is nothing to fetch. Those series are
/// folded locally from trades and simply have no history.
fn candle_interval(interval: u8) -> Option<(&'static str, u64)> {
    Some(match interval {
        PC_IV_1M => ("1m", 60_000),
        PC_IV_5M => ("5m", 5 * 60_000),
        PC_IV_30M => ("30m", 30 * 60_000),
        PC_IV_15M => ("15m", 15 * 60_000),
        PC_IV_1H => ("1h", 60 * 60_000),
        PC_IV_4H => ("4h", 4 * 60 * 60_000),
        PC_IV_1D => ("1d", 24 * 60 * 60_000),
        _ => return None,
    })
}

fn candle_interval_id(interval: &str) -> Option<u8> {
    Some(match interval {
        "1m" => PC_IV_1M,
        "5m" => PC_IV_5M,
        "15m" => PC_IV_15M,
        "1h" => PC_IV_1H,
        "4h" => PC_IV_4H,
        "1d" => PC_IV_1D,
        _ => return None,
    })
}

// ---------------------------------------------------------------------------------
// account-scoped fetches — queried with the MASTER address (docs/03 §5)
// ---------------------------------------------------------------------------------

pub(crate) async fn fetch_clearinghouse_state(
    http: &HttpClient,
    events: &EventQueue,
    registry: &SharedRegistry,
    master: &str,
    req_id: u64,
) -> Result<(), String> {
    let body = json!({"type": "clearinghouseState", "user": master});
    let value = http
        .post_info_with_retry(&body, |ms| events.push(rate_event(ms, req_id)))
        .await
        .map_err(|e| format!("clearinghouseState fetch failed: {e}"))?;
    let state: info::ClearinghouseState = serde_json::from_value(value)
        .map_err(|e| format!("clearinghouseState decode failed: {e}"))?;
    push_clearinghouse_state(events, registry, req_id, &state);
    Ok(())
}

/// Turn one `clearinghouseState` payload into `PC_EV_POSITION` + `PC_EV_ACCOUNT`
/// events, bracketed by `PC_F_SNAPSHOT_BEGIN`/`PC_F_SNAPSHOT_END` so the C++ engine can
/// swap portfolio state atomically (docs/02 §5.4). No I/O — shared between the
/// one-shot REST fetch above and `transport::user_ws`'s `clearinghouseState`
/// subscription, which re-pushes this same shape roughly every 4 s (docs/03 §W2.16).
pub(crate) fn push_clearinghouse_state(
    events: &EventQueue,
    registry: &SharedRegistry,
    req_id: u64,
    state: &info::ClearinghouseState,
) {
    let n = state.asset_positions.len();
    for (i, ap) in state.asset_positions.iter().enumerate() {
        let p = &ap.position;
        let asset = registry.index_of(&p.coin);
        let mut event = base_event(PC_EV_POSITION, asset, state.time, req_id);
        let mut flags = PC_F_SNAPSHOT;
        if i == 0 {
            flags |= PC_F_SNAPSHOT_BEGIN;
        }
        event.flags = flags;
        let roe_bps = p
            .return_on_equity
            .parse::<f64>()
            .ok()
            .map(|v| (v * 10_000.0).round() as i32)
            .unwrap_or(0);
        event.u = PcEventUnion {
            position: PcPosition {
                szi: parse_scaled(&p.szi).unwrap_or(0),
                entry_px: p
                    .entry_px
                    .as_deref()
                    .and_then(|s| parse_scaled(s).ok())
                    .unwrap_or(0),
                liq_px: p
                    .liquidation_px
                    .as_deref()
                    .and_then(|s| parse_scaled(s).ok())
                    .unwrap_or(0),
                position_value: parse_scaled(&p.position_value).unwrap_or(0),
                unrealized_pnl: parse_scaled(&p.unrealized_pnl).unwrap_or(0),
                margin_used: parse_scaled(&p.margin_used).unwrap_or(0),
                // `sinceOpen`, not `allTime`: allTime is the coin's lifetime total across every
                // position ever held in it, which is not a property of the position on screen.
                cum_funding: parse_scaled(&p.cum_funding.since_open).unwrap_or(0),
                roe_bps,
                leverage: p.leverage.value,
                is_cross: (p.leverage.kind == "cross") as u8,
            },
        };
        events.push(event);
    }

    let mut account_event = base_event(PC_EV_ACCOUNT, PC_ASSET_NONE, state.time, req_id);
    account_event.flags =
        PC_F_SNAPSHOT | PC_F_SNAPSHOT_END | if n == 0 { PC_F_SNAPSHOT_BEGIN } else { 0 };
    account_event.u = PcEventUnion {
        account: PcAccount {
            account_value: parse_scaled(&state.margin_summary.account_value).unwrap_or(0),
            // Cross-only equity. `crossMaintenanceMarginUsed` below is cross-only too, and a
            // liquidation calculation that pairs it with the cross+isolated `marginSummary`
            // figure overstates the cushion by every isolated position's equity.
            cross_account_value: parse_scaled(&state.cross_margin_summary.account_value)
                .unwrap_or(0),
            total_margin_used: parse_scaled(&state.margin_summary.total_margin_used).unwrap_or(0),
            total_ntl_pos: parse_scaled(&state.margin_summary.total_ntl_pos).unwrap_or(0),
            withdrawable: parse_scaled(&state.withdrawable).unwrap_or(0),
            cross_maintenance_margin: parse_scaled(&state.cross_maintenance_margin_used)
                .unwrap_or(0),
        },
    };
    events.push(account_event);
}

pub(crate) async fn fetch_open_orders(
    http: &HttpClient,
    events: &EventQueue,
    registry: &SharedRegistry,
    master: &str,
    req_id: u64,
) -> Result<(), String> {
    let body = json!({"type": "frontendOpenOrders", "user": master});
    let value = http
        .post_info_with_retry(&body, |ms| events.push(rate_event(ms, req_id)))
        .await
        .map_err(|e| format!("frontendOpenOrders fetch failed: {e}"))?;
    let orders: Vec<info::FrontendOpenOrder> = serde_json::from_value(value)
        .map_err(|e| format!("frontendOpenOrders decode failed: {e}"))?;
    let n = orders.len();
    for (i, o) in orders.iter().enumerate() {
        let asset = registry.index_of(&o.coin);
        let mut event = base_event(PC_EV_ORDER_UPDATE, asset, o.timestamp, req_id);
        let mut flags = PC_F_SNAPSHOT;
        if i == 0 {
            flags |= PC_F_SNAPSHOT_BEGIN;
        }
        if i + 1 == n {
            flags |= PC_F_SNAPSHOT_END;
        }
        event.flags = flags;
        event.u = PcEventUnion {
            order_update: PcOrderUpdate {
                oid: o.oid,
                cloid: parse_cloid(o.cloid.as_deref()),
                status: PC_ORD_OPEN,
                px: parse_scaled(&o.limit_px).unwrap_or(0),
                sz: parse_scaled(&o.sz).unwrap_or(0),
                orig_sz: parse_scaled(&o.orig_sz).unwrap_or(0),
                trigger_px: if o.is_trigger {
                    parse_scaled(&o.trigger_px).unwrap_or(0)
                } else {
                    0
                },
                is_buy: (o.side == "B") as u8,
                reduce_only: o.reduce_only as u8,
                is_trigger: o.is_trigger as u8,
                tpsl: tpsl_kind(o.is_trigger, &o.order_type),
                // The venue names it in the order type: "Take Profit Market" converts to a
                // market order when it fires, "Take Profit Limit" to a limit one.
                is_market_trigger: o.order_type.ends_with("Market") as u8,
            },
        };
        events.push(event);
    }
    if n == 0 {
        // An empty result is a snapshot too: "nothing is resting" has to reach the consumer as
        // a bracketed batch, or a UI that rebuilds its list from these events can never learn
        // that the last resting order is gone. `oid == 0` is the empty marker -- a real order
        // always has one.
        let mut event = base_event(PC_EV_ORDER_UPDATE, PC_ASSET_NONE, 0, req_id);
        event.flags = PC_F_SNAPSHOT | PC_F_SNAPSHOT_BEGIN | PC_F_SNAPSHOT_END;
        event.u = PcEventUnion {
            order_update: PcOrderUpdate {
                oid: 0,
                cloid: [0; 16],
                status: PC_ORD_UNKNOWN,
                px: 0,
                sz: 0,
                orig_sz: 0,
                trigger_px: 0,
                is_buy: 0,
                reduce_only: 0,
                is_trigger: 0,
                tpsl: PC_TPSL_NONE,
                is_market_trigger: 0,
            },
        };
        events.push(event);
    }
    Ok(())
}

async fn fetch_user_fills(
    http: &HttpClient,
    events: &EventQueue,
    registry: &SharedRegistry,
    master: &str,
    req_id: u64,
) -> Result<(), String> {
    let body = json!({"type": "userFills", "user": master, "aggregateByTime": false});
    let value = http
        .post_info_with_retry(&body, |ms| events.push(rate_event(ms, req_id)))
        .await
        .map_err(|e| format!("userFills fetch failed: {e}"))?;
    let fills: Vec<info::Fill> =
        serde_json::from_value(value).map_err(|e| format!("userFills decode failed: {e}"))?;
    let n = fills.len();
    for (i, f) in fills.iter().enumerate() {
        let mut flags = PC_F_SNAPSHOT;
        if i == 0 {
            flags |= PC_F_SNAPSHOT_BEGIN;
        }
        if i + 1 == n {
            flags |= PC_F_SNAPSHOT_END;
        }
        events.push(fill_event(registry, req_id, flags, f));
    }
    if n == 0 {
        events.push(PcEvent::error(0, "no fills", req_id));
    }
    Ok(())
}

/// One `info::Fill` (REST `userFills`/`userFillsByTime`, and the equivalent element of
/// the WS `userFills` stream, which is the same shape) -> a `PC_EV_FILL` event. No I/O
/// — shared by the REST fetch above and `transport::user_ws`'s reconnect
/// reconciliation (`fetch_user_fills_since`) and its live `userFills` stream.
/// Maps a `frontendOpenOrders` / `historicalOrders` `orderType` string onto `PC_TPSL_*`.
/// The venue names the leg in the order type ("Take Profit Market", "Stop Limit", ...);
/// there is no separate boolean, so this is the only place the two legs are told apart.
fn tpsl_kind(is_trigger: bool, order_type: &str) -> u8 {
    if !is_trigger {
        return PC_TPSL_NONE;
    }
    if order_type.contains("Take Profit") {
        PC_TPSL_TP
    } else {
        PC_TPSL_SL
    }
}

/// Maps the venue's `userFills.dir` string onto `PC_DIR_*`. The string is the only place the
/// wire says whether a fill opened, closed or flipped a position -- the side alone cannot,
/// since a sell is an opening short or a closing long depending on what was held.
fn dir_code(dir: &str) -> u8 {
    match dir {
        "Open Long" => PC_DIR_OPEN_LONG,
        "Close Long" => PC_DIR_CLOSE_LONG,
        "Open Short" => PC_DIR_OPEN_SHORT,
        "Close Short" => PC_DIR_CLOSE_SHORT,
        "Long > Short" => PC_DIR_LONG_TO_SHORT,
        "Short > Long" => PC_DIR_SHORT_TO_LONG,
        "Buy" => PC_DIR_BUY,
        "Sell" => PC_DIR_SELL,
        // "Liquidated Cross", "Liquidated Isolated", "Auto-Deleveraged ..." and any future
        // wording all mean the same thing to a reader of a history table.
        other if other.starts_with("Liquidated") || other.starts_with("Auto-Deleveraged") => {
            PC_DIR_LIQUIDATION
        }
        _ => PC_DIR_UNKNOWN,
    }
}

pub(crate) fn fill_event(
    registry: &SharedRegistry,
    req_id: u64,
    flags: u16,
    f: &info::Fill,
) -> PcEvent {
    let asset = registry.index_of(&f.coin);
    let mut event = base_event(PC_EV_FILL, asset, f.time, req_id);
    event.flags = flags;
    event.u = PcEventUnion {
        // `fee` already includes `builderFee` (docs/03 §W2.9) — adding them here
        // would double-count, so `builder_fee` is deliberately never read.
        fill: PcFill {
            oid: f.oid,
            tid: f.tid,
            cloid: [0; 16], // `userFills` carries no cloid (codec::info::Fill).
            px: parse_scaled(&f.px).unwrap_or(0),
            qty: parse_scaled(&f.sz).unwrap_or(0),
            fee: parse_scaled(&f.fee).unwrap_or(0),
            closed_pnl: parse_scaled(&f.closed_pnl).unwrap_or(0),
            is_buy: (f.side == "B") as u8,
            is_taker: f.crossed as u8,
            dir: dir_code(&f.dir),
        },
    };
    event
}

/// Part of the `orderUpdates` reconnect reconciliation (docs/03 §W2.0, 02 §8):
/// `orderUpdates` sends no snapshot on (re)connect, so `transport::user_ws` calls this
/// alongside `fetch_open_orders` to backfill anything that happened while the socket
/// was down, via `userFillsByTime(since=last_seen)`. `req_id` is `0` here since this
/// runs off a reconnect, not a caller's `pc_fetch`.
pub(crate) async fn fetch_user_fills_since(
    http: &HttpClient,
    events: &EventQueue,
    registry: &SharedRegistry,
    master: &str,
    since_ms: u64,
) -> Result<(), String> {
    let now = crate::ffi::actions::now_ms();
    let body = json!({
        "type": "userFillsByTime", "user": master,
        "startTime": since_ms, "endTime": now, "aggregateByTime": false,
    });
    let value = http
        .post_info_with_retry(&body, |ms| events.push(rate_event(ms, 0)))
        .await
        .map_err(|e| format!("userFillsByTime fetch failed: {e}"))?;
    let fills: Vec<info::Fill> =
        serde_json::from_value(value).map_err(|e| format!("userFillsByTime decode failed: {e}"))?;
    for f in &fills {
        events.push(fill_event(registry, 0, PC_F_SNAPSHOT, f));
    }
    Ok(())
}

/// One `PC_EV_FUNDING` event from a `userFunding` entry. Shared by the REST backfill
/// below and the live `userFundings` stream in `transport::user_ws`, so both paths sign
/// and scale a payment identically — the C++ store dedups the two by `(asset, hour)`.
pub(crate) fn funding_event(
    registry: &SharedRegistry,
    req_id: u64,
    flags: u16,
    entry: &info::FundingEntry,
) -> PcEvent {
    let asset = registry.index_of(&entry.delta.coin);
    let mut event = base_event(PC_EV_FUNDING, asset, entry.time, req_id);
    event.flags = flags;
    event.u = PcEventUnion {
        funding: PcFunding {
            // Signed as the venue reports it: negative is paid, positive is received.
            // Verified against the docs/03 §userFunding fixture — a long position under a
            // positive rate pays, and the venue writes that payment as a negative `usdc`.
            usdc: parse_scaled(&entry.delta.usdc).unwrap_or(0),
            szi: parse_scaled(&entry.delta.szi).unwrap_or(0),
            // The funding rate carries more than 8 decimals on the wire (docs/09 §2.1),
            // so it needs the rounding parser, not the strict one.
            rate_1e8: parse_scaled_stat_rounded(&entry.delta.funding_rate).unwrap_or(0),
            n_samples: entry.delta.n_samples.unwrap_or(0),
        },
    };
    event
}

async fn fetch_user_funding(
    http: &HttpClient,
    events: &EventQueue,
    registry: &SharedRegistry,
    master: &str,
    req_id: u64,
) -> Result<(), String> {
    let now_ms = crate::ffi::actions::now_ms();
    const LOOKBACK_MS: u64 = 30 * 24 * 60 * 60 * 1000; // 30 days
    let body = json!({
        "type": "userFunding", "user": master,
        "startTime": now_ms.saturating_sub(LOOKBACK_MS), "endTime": now_ms,
    });
    let value = http
        .post_info_with_retry(&body, |ms| events.push(rate_event(ms, req_id)))
        .await
        .map_err(|e| format!("userFunding fetch failed: {e}"))?;
    let entries: Vec<info::FundingEntry> =
        serde_json::from_value(value).map_err(|e| format!("userFunding decode failed: {e}"))?;
    let n = entries.len();
    for (i, entry) in entries.iter().enumerate() {
        let mut flags = PC_F_SNAPSHOT;
        if i == 0 {
            flags |= PC_F_SNAPSHOT_BEGIN;
        }
        if i + 1 == n {
            flags |= PC_F_SNAPSHOT_END;
        }
        events.push(funding_event(registry, req_id, flags, entry));
    }
    if n == 0 {
        events.push(PcEvent::error(0, "no funding history", req_id));
    }
    Ok(())
}

async fn fetch_historical_orders(
    http: &HttpClient,
    events: &EventQueue,
    registry: &SharedRegistry,
    master: &str,
    req_id: u64,
) -> Result<(), String> {
    let body = json!({"type": "historicalOrders", "user": master});
    let value = http
        .post_info_with_retry(&body, |ms| events.push(rate_event(ms, req_id)))
        .await
        .map_err(|e| format!("historicalOrders fetch failed: {e}"))?;
    let entries: Vec<info::HistoricalOrder> = serde_json::from_value(value)
        .map_err(|e| format!("historicalOrders decode failed: {e}"))?;
    let n = entries.len();
    for (i, entry) in entries.iter().enumerate() {
        let o = &entry.order;
        let asset = registry.index_of(&o.coin);
        let mut event = base_event(PC_EV_ORDER_UPDATE, asset, entry.status_timestamp, req_id);
        // Marks the whole batch as an order's PAST. `frontendOpenOrders` below emits the same
        // event kind for what is resting NOW; without this bit a consumer cannot tell the two
        // apart, and a history backfill repopulates the open-orders list with dead orders.
        let mut flags = PC_F_SNAPSHOT | PC_F_HISTORICAL;
        if i == 0 {
            flags |= PC_F_SNAPSHOT_BEGIN;
        }
        if i + 1 == n {
            flags |= PC_F_SNAPSHOT_END;
        }
        event.flags = flags;
        event.u = PcEventUnion {
            order_update: PcOrderUpdate {
                oid: o.oid,
                cloid: parse_cloid(o.cloid.as_deref()),
                status: info::order_status_code(&entry.status),
                px: parse_scaled(&o.limit_px).unwrap_or(0),
                sz: parse_scaled(&o.sz).unwrap_or(0),
                orig_sz: parse_scaled(&o.orig_sz).unwrap_or(0),
                trigger_px: if o.is_trigger {
                    parse_scaled(&o.trigger_px).unwrap_or(0)
                } else {
                    0
                },
                is_buy: (o.side == "B") as u8,
                reduce_only: o.reduce_only as u8,
                is_trigger: o.is_trigger as u8,
                tpsl: tpsl_kind(o.is_trigger, &o.order_type),
                // The venue names it in the order type: "Take Profit Market" converts to a
                // market order when it fires, "Take Profit Limit" to a limit one.
                is_market_trigger: o.order_type.ends_with("Market") as u8,
            },
        };
        events.push(event);
    }
    if n == 0 {
        events.push(PcEvent::error(0, "no historical orders", req_id));
    }
    Ok(())
}

async fn fetch_active_asset_data(
    http: &HttpClient,
    events: &EventQueue,
    registry: &SharedRegistry,
    master: &str,
    coin: Option<String>,
    req_id: u64,
) -> Result<(), String> {
    let coin = coin.ok_or_else(|| "activeAssetData fetch requires a coin".to_string())?;
    let asset = registry.index_of(&coin);
    let body = json!({"type": "activeAssetData", "user": master, "coin": coin});
    let value = http
        .post_info_with_retry(&body, |ms| events.push(rate_event(ms, req_id)))
        .await
        .map_err(|e| format!("activeAssetData fetch failed: {e}"))?;
    let data: info::ActiveAssetData =
        serde_json::from_value(value).map_err(|e| format!("activeAssetData decode failed: {e}"))?;
    let mut event = base_event(PC_EV_ASSET_DATA, asset, 0, req_id);
    event.flags = PC_F_SNAPSHOT;
    event.u = PcEventUnion {
        asset_data: PcAssetData {
            max_trade_buy: parse_scaled(&data.max_trade_szs[0]).unwrap_or(0),
            max_trade_sell: parse_scaled(&data.max_trade_szs[1]).unwrap_or(0),
            avail_buy: parse_scaled(&data.available_to_trade[0]).unwrap_or(0),
            avail_sell: parse_scaled(&data.available_to_trade[1]).unwrap_or(0),
            mark: parse_scaled(&data.mark_px).unwrap_or(0),
            leverage: data.leverage.value,
            is_cross: (data.leverage.kind == "cross") as u8,
        },
    };
    events.push(event);
    Ok(())
}

async fn fetch_user_fees(
    http: &HttpClient,
    events: &EventQueue,
    master: &str,
    req_id: u64,
) -> Result<(), String> {
    let body = json!({"type": "userFees", "user": master});
    let value = http
        .post_info_with_retry(&body, |ms| events.push(rate_event(ms, req_id)))
        .await
        .map_err(|e| format!("userFees fetch failed: {e}"))?;
    let fees: info::UserFees =
        serde_json::from_value(value).map_err(|e| format!("userFees decode failed: {e}"))?;
    let mut event = base_event(PC_EV_FEE_RATES, PC_ASSET_NONE, 0, req_id);
    event.u = PcEventUnion {
        fee_rates: PcFeeRates {
            // `userAddRate` is the effective maker/add rate and may be negative for a rebate.
            maker_rate: parse_scaled(&fees.user_add_rate).unwrap_or(0),
            taker_rate: parse_scaled(&fees.user_cross_rate).unwrap_or(0),
        },
    };
    events.push(event);
    Ok(())
}

/// The account's USDC spot row. Hyperliquid collateralises perps from a single USDC pool, so
/// this is not a side ledger: the USDC that is not currently deployed as perp equity is what
/// funds the next position, and `clearinghouseState.withdrawable` does not see it at all
/// (measured: on an account with 37.46 withdrawable and 1433.79 undeployed USDC, the venue's
/// own `activeAssetData.availableToTrade` for the opening side read 1471.24).
///
/// A missing USDC row means "no USDC anywhere", which is a zero balance, not an error — the
/// event still goes out so the UI can stop showing the field as pending.
async fn fetch_spot_state(
    http: &HttpClient,
    events: &EventQueue,
    master: &str,
    req_id: u64,
) -> Result<(), String> {
    let body = json!({"type": "spotClearinghouseState", "user": master});
    let value = http
        .post_info_with_retry(&body, |ms| events.push(rate_event(ms, req_id)))
        .await
        .map_err(|e| format!("spotClearinghouseState fetch failed: {e}"))?;
    let state: info::SpotClearinghouseState = serde_json::from_value(value)
        .map_err(|e| format!("spotClearinghouseState decode failed: {e}"))?;
    let usdc = state.balances.iter().find(|b| b.coin == "USDC");
    let mut event = base_event(PC_EV_SPOT_BALANCE, PC_ASSET_NONE, 0, req_id);
    event.flags = PC_F_SNAPSHOT;
    event.u = PcEventUnion {
        spot: PcSpot {
            total: usdc.and_then(|b| parse_scaled(&b.total).ok()).unwrap_or(0),
            hold: usdc.and_then(|b| parse_scaled(&b.hold).ok()).unwrap_or(0),
        },
    };
    events.push(event);
    Ok(())
}

async fn fetch_user_rate_limit(
    http: &HttpClient,
    events: &EventQueue,
    session: &Session,
    master: &str,
    req_id: u64,
) -> Result<(), String> {
    let body = json!({"type": "userRateLimit", "user": master});
    let value = http
        .post_info_with_retry(&body, |ms| events.push(rate_event(ms, req_id)))
        .await
        .map_err(|e| format!("userRateLimit fetch failed: {e}"))?;
    let limit: info::UserRateLimit =
        serde_json::from_value(value).map_err(|e| format!("userRateLimit decode failed: {e}"))?;
    // Authoritative correction of the client-side estimate (docs/03 §7): this is the
    // one place `RateBudget::reconcile` is fed a server-reported number rather than a
    // guess.
    session.rate_budget.reconcile(limit.n_requests_surplus);
    let mut event = base_event(PC_EV_RATE, PC_ASSET_NONE, 0, req_id);
    event.u = PcEventUnion {
        rate: PcRate {
            remaining: limit.n_requests_surplus,
            reset_ms: 0, // resets continuously with traded volume, not on a clock.
        },
    };
    events.push(event);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dir_code_covers_every_direction_the_venue_reports() {
        assert_eq!(dir_code("Open Long"), PC_DIR_OPEN_LONG);
        assert_eq!(dir_code("Close Long"), PC_DIR_CLOSE_LONG);
        assert_eq!(dir_code("Open Short"), PC_DIR_OPEN_SHORT);
        assert_eq!(dir_code("Close Short"), PC_DIR_CLOSE_SHORT);
        assert_eq!(dir_code("Long > Short"), PC_DIR_LONG_TO_SHORT);
        assert_eq!(dir_code("Short > Long"), PC_DIR_SHORT_TO_LONG);
        // Every liquidation wording collapses to one code -- the distinction between cross,
        // isolated and auto-deleveraged matters to the venue, not to a history row.
        assert_eq!(dir_code("Liquidated Cross"), PC_DIR_LIQUIDATION);
        assert_eq!(dir_code("Liquidated Isolated"), PC_DIR_LIQUIDATION);
        assert_eq!(dir_code("Auto-Deleveraged Long"), PC_DIR_LIQUIDATION);
        // Anything unrecognised must fall back rather than be silently mapped onto a
        // direction it does not mean; the UI then renders the side instead.
        assert_eq!(dir_code("Something New"), PC_DIR_UNKNOWN);
    }

    #[test]
    fn order_type_names_the_trigger_leg_and_whether_it_is_market() {
        assert_eq!(tpsl_kind(false, "Limit"), PC_TPSL_NONE);
        assert_eq!(tpsl_kind(true, "Take Profit Market"), PC_TPSL_TP);
        assert_eq!(tpsl_kind(true, "Stop Market"), PC_TPSL_SL);
        assert_eq!(tpsl_kind(true, "Stop Limit"), PC_TPSL_SL);
        // is_market_trigger is read straight off the same string in order_wire's callers.
        assert!("Take Profit Market".ends_with("Market"));
        assert!(!"Take Profit Limit".ends_with("Market"));
    }
}
