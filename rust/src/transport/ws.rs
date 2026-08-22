use crate::transport::backoff::Backoff;
use crate::{
    codec::{
        decimal::{parse_scaled, parse_scaled_stat_rounded},
        ws_msg::{integer, string},
    },
    ffi::{queue::EventQueue, types::*},
    universe::SharedRegistry,
};
use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use std::{collections::HashMap, time::Duration};
use tokio::sync::mpsc;
use tokio_tungstenite::tungstenite::Message;

type Socket =
    tokio_tungstenite::WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>;

/// Mirrors PC_STREAM_* in include/parsec/parsec.h.
const PC_STREAM_BBO: u32 = 1 << 0;
const PC_STREAM_L2: u32 = 1 << 1;
const PC_STREAM_L2_FAST: u32 = 1 << 2;
const PC_STREAM_TRADES: u32 = 1 << 3;
const PC_STREAM_ASSET_CTX: u32 = 1 << 4;
const PC_STREAM_CANDLE: u32 = 1 << 6;

/// Unanswered pings before the socket is considered dead. At the 5s ping cadence this is a
/// ~30s window, in the same ballpark as the two-missed-pong rule it replaces had at 50s, and
/// the 60s idle-message backstop below still covers a socket that goes quiet another way.
const PING_TEARDOWN_COUNT: u32 = 6;

/// The venue replays the last 30 trades on every `trades` subscribe (03 §W2.0/§W8 #12), so a
/// reconnect would otherwise duplicate the whole tail of the tape.
const TRADE_DEDUPE_WINDOW: usize = 256;

#[derive(Clone)]
pub enum Command {
    Subscribe {
        id: u64,
        coin: String,
        mask: u32,
        interval: u8,
        book: BookAggregation,
    },
    Unsubscribe {
        id: u64,
        coin: String,
        mask: u32,
        interval: u8,
        book: BookAggregation,
    },
}

/// Price-granularity parameters for the `l2Book` subscriptions. The venue aggregates the book
/// server-side and still returns its full 20 levels at the coarser step, which is the only way
/// to fill a ladder past the ~$19 span 5-significant-figure levels cover on BTC. `None` means
/// the wire field is sent as `null` (the venue's native granularity).
///
/// These must be carried on Unsubscribe as well as Subscribe: the venue matches an
/// unsubscribe against the exact subscription payload, so unsubscribing a `nSigFigs: 4` book
/// with `nSigFigs: null` silently leaves the old subscription running and the ladder then
/// receives two different granularities on the same channel.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug, Default)]
pub struct BookAggregation {
    pub n_sig_figs: Option<u8>,
    pub mantissa: Option<u8>,
}
impl Command {
    pub fn with_id(mut self, id: u64) -> Self {
        match &mut self {
            Self::Subscribe { id: slot, .. } | Self::Unsubscribe { id: slot, .. } => *slot = id,
        };
        self
    }
}
/// Bounded most-recent-tid set for the 30-trade backfill dedupe. A ring plus a hash set keeps
/// membership O(1) while bounding memory -- the tape only ever needs to recognise the recent
/// past, so the oldest tid is evicted once the window is full.
#[derive(Default)]
struct TradeDedupe {
    seen: std::collections::HashSet<u64>,
    order: std::collections::VecDeque<u64>,
}

impl TradeDedupe {
    /// Returns true if this tid is new (and records it), false if it is a replay.
    fn insert(&mut self, tid: u64) -> bool {
        if tid == 0 {
            return true; // no tid to dedupe on; never suppress
        }
        if !self.seen.insert(tid) {
            return false;
        }
        self.order.push_back(tid);
        if self.order.len() > TRADE_DEDUPE_WINDOW {
            if let Some(old) = self.order.pop_front() {
                self.seen.remove(&old);
            }
        }
        true
    }
}

#[derive(Clone)]
struct Subscription {
    coin: String,
    mask: u32,
    interval: u8,
    book: BookAggregation,
}
pub async fn run(
    mut commands: mpsc::UnboundedReceiver<Command>,
    events: EventQueue,
    mainnet: bool,
    registry: SharedRegistry,
) {
    let endpoint = if mainnet {
        "wss://api.hyperliquid.xyz/ws"
    } else {
        "wss://api.hyperliquid-testnet.xyz/ws"
    };
    let mut subscriptions: HashMap<(String, u32, u8), Subscription> = HashMap::new();
    // Deliberately outside the connect loop: the backfill dedupe only earns its keep across a
    // reconnect, which is precisely when the venue replays the last 30 trades.
    let mut seen_trades = TradeDedupe::default();
    let mut backoff = Backoff::default();
    let mut reconnects: u32 = 0;
    loop {
        match tokio_tungstenite::connect_async(endpoint).await {
            Ok((mut socket, _)) => {
                reconnects = 0;
                backoff.reset();
                emit_conn(&events, SOCK_MARKET, CONN_CONNECTED, reconnects);
                for sub in subscriptions.values() {
                    subscribe(&mut socket, sub).await;
                }
                // Pings double as the latency probe, so they run on a 5s cadence rather than
                // the old 50s: a ping frame is a few bytes, and a round-trip figure that
                // refreshes once a minute is not a readout a trader can act on. The
                // dead-connection rule is expressed in wall time rather than ping count so
                // shortening the interval does not make teardown hair-trigger.
                let mut ping = tokio::time::interval(Duration::from_secs(5));
                let mut ping_sent_at: Option<tokio::time::Instant> = None;
                let mut idle = tokio::time::interval(Duration::from_secs(5));
                let mut last_message = tokio::time::Instant::now();
                let mut pings_outstanding: u32 = 0;
                loop {
                    tokio::select! {
                        command = commands.recv() => match command {
                            Some(Command::Subscribe { id, coin, mask, interval, book }) => {
                                let sub = Subscription { coin, mask, interval, book };
                                subscriptions.insert((sub.coin.clone(), mask, interval), sub.clone());
                                subscribe(&mut socket, &sub).await;
                                events.push(PcEvent::error(0, "subscription accepted", id));
                            }
                            Some(Command::Unsubscribe { id, coin, mask, interval, book }) => {
                                let sub = Subscription { coin: coin.clone(), mask, interval, book };
                                subscriptions.remove(&(coin, mask, interval));
                                unsubscribe(&mut socket, &sub).await;
                                events.push(PcEvent::error(0, "unsubscription accepted", id));
                            }
                            None => return,
                        },
                        _ = ping.tick() => {
                            // 04 §4: tear down after two unanswered pongs rather than waiting
                            // for the server to close. The 60 s idle clock below is the
                            // server->client backstop; this catches a half-open socket where
                            // our writes still succeed locally.
                            if pings_outstanding >= PING_TEARDOWN_COUNT {
                                break;
                            }
                            if socket.send(Message::Ping(Vec::new())).await.is_err() {
                                break;
                            }
                            // Only the oldest unanswered ping is timed: pongs are not
                            // correlated to a specific ping here, so timing the newest would
                            // under-report RTT whenever one goes missing.
                            if ping_sent_at.is_none() {
                                ping_sent_at = Some(tokio::time::Instant::now());
                            }
                            pings_outstanding += 1;
                        }
                        _ = idle.tick() => {
                            if last_message.elapsed() > Duration::from_secs(60) {
                                break;
                            }
                        }
                        frame = socket.next() => match frame {
                            Some(Ok(Message::Text(text))) => {
                                last_message = tokio::time::Instant::now();
                                match serde_json::from_str::<Value>(&text) {
                                    Ok(value) => {
                                        parse_event(&events, value, &registry, &mut seen_trades)
                                    }
                                    // The venue still emits a bare, non-JSON greeting frame
                                    // ("Websocket connection established.") on some paths
                                    // (04 §4). It is not an error and must not be surfaced as
                                    // one; anything else non-JSON is worth reporting once.
                                    Err(_) if text.starts_with("Websocket connection") => {}
                                    Err(_) => events.push(PcEvent::error(
                                        -4,
                                        "non-JSON frame from venue",
                                        0,
                                    )),
                                }
                            }
                            Some(Ok(Message::Ping(payload))) => {
                                let _ = socket.send(Message::Pong(payload)).await;
                            }
                            Some(Ok(Message::Pong(_))) => {
                                last_message = tokio::time::Instant::now();
                                pings_outstanding = 0;
                                if let Some(sent) = ping_sent_at.take() {
                                    let rtt = sent.elapsed().as_micros().min(u32::MAX as u128);
                                    emit_conn_rtt(
                                        &events,
                                        SOCK_MARKET,
                                        CONN_CONNECTED,
                                        reconnects,
                                        rtt as u32,
                                    );
                                }
                            }
                            Some(Ok(Message::Close(_))) | Some(Err(_)) | None => break,
                            _ => {}
                        }
                    }
                }
            }
            Err(error) => events.push(PcEvent::error(
                -3,
                &format!("websocket connect failed: {error}"),
                0,
            )),
        }
        // Two distinct states, deliberately: the UI greys the affected panel the instant the
        // socket drops, then shows a reconnecting badge for the backoff window (02 §8).
        emit_conn(&events, SOCK_MARKET, CONN_DISCONNECTED, reconnects);
        reconnects = reconnects.saturating_add(1);
        emit_conn(&events, SOCK_MARKET, CONN_RECONNECTING, reconnects);
        // Jittered exponential backoff, 0.5 s -> 30 s (02 §8). The jitter keeps many clients
        // from retrying a recovering endpoint in lockstep.
        tokio::time::sleep(Duration::from_millis(backoff.next_ms())).await;
    }
}
/// Mirrors PC_SOCK_* / PC_CONN_* in include/parsec/parsec.h.
const SOCK_MARKET: u8 = 0;
const CONN_DISCONNECTED: u8 = 0;
const CONN_CONNECTED: u8 = 1;
const CONN_RECONNECTING: u8 = 2;

fn emit_conn(events: &EventQueue, socket: u8, state: u8, reconnects: u32) {
    emit_conn_rtt(events, socket, state, reconnects, 0)
}

fn emit_conn_rtt(events: &EventQueue, socket: u8, state: u8, reconnects: u32, rtt_us: u32) {
    events.push(PcEvent {
        kind: 12,
        flags: 0,
        asset: PC_ASSET_NONE,
        req_id: 0,
        exch_time_ms: 0,
        recv_time_ns: crate::ffi::monotonic_ns(),
        u: PcEventUnion {
            conn: PcConn {
                socket,
                state,
                reconnects,
                rtt_us,
            },
        },
    });
}
/// The venue's interval strings, indexed by PC_IV_* (include/parsec/parsec.h). The sub-minute
/// ids map to "1m": the venue has no seconds candles, and those series are folded from the
/// trades stream on the C++ side, so a `candle` subscription is never issued for them (the
/// engine does not call subscribe with a sub-minute id at all -- this is a floor, not a path).
const INTERVALS: [&str; PC_IV_COUNT as usize] = [
    "1m", "1m", "1m", "1m", "1m", "5m", "15m", "30m", "1h", "4h", "1d",
];

fn interval_str(interval: u8) -> &'static str {
    INTERVALS.get(interval as usize).copied().unwrap_or("1m")
}

/// Every subscription payload implied by a mask, in one place so subscribe and unsubscribe
/// can never drift apart. Mask bits mirror PC_STREAM_* in include/parsec/parsec.h.
fn subscription_payloads(sub: &Subscription) -> Vec<Value> {
    let mut out = Vec::new();
    if sub.mask & PC_STREAM_BBO != 0 {
        out.push(json!({"type": "bbo", "coin": sub.coin}));
    }
    if sub.mask & PC_STREAM_L2 != 0 {
        out.push(json!({"type": "l2Book", "coin": sub.coin,
                        "nSigFigs": sub.book.n_sig_figs, "mantissa": sub.book.mantissa}));
    }
    if sub.mask & PC_STREAM_L2_FAST != 0 {
        // The two l2Book subscriptions run at the SAME price granularity on purpose: the fast
        // one is 5 levels at ~536ms and the default 20 at ~5.4s, so the fast feed is what keeps
        // the top of the ladder live while the default feed fills the tail. Measured on
        // mainnet BTC, `fast:true` composes with `nSigFigs` -- aggregating server-side does not
        // cost the fast cadence.
        out.push(json!({"type": "l2Book", "coin": sub.coin, "fast": true,
                        "nSigFigs": sub.book.n_sig_figs, "mantissa": sub.book.mantissa}));
    }
    if sub.mask & PC_STREAM_TRADES != 0 {
        out.push(json!({"type": "trades", "coin": sub.coin}));
    }
    if sub.mask & PC_STREAM_ASSET_CTX != 0 {
        out.push(json!({"type": "activeAssetCtx", "coin": sub.coin}));
    }
    if sub.mask & PC_STREAM_CANDLE != 0 {
        out.push(
            json!({"type": "candle", "coin": sub.coin, "interval": interval_str(sub.interval)}),
        );
    }
    out
}

async fn subscribe(socket: &mut Socket, sub: &Subscription) {
    for payload in subscription_payloads(sub) {
        let request = json!({"method": "subscribe", "subscription": payload});
        let _ = socket.send(Message::Text(request.to_string())).await;
    }
}

/// Unsubscribes exactly the streams the mask names. This previously sent a single hardcoded
/// `bbo` unsubscribe regardless of the mask, so every other stream stayed live on the socket
/// and kept billing against the connection's subscription cap.
async fn unsubscribe(socket: &mut Socket, sub: &Subscription) {
    for payload in subscription_payloads(sub) {
        let request = json!({"method": "unsubscribe", "subscription": payload});
        let _ = socket.send(Message::Text(request.to_string())).await;
    }
}

/// Dense asset index for a coin, resolved from the `meta` universe (03 "Asset ID encoding
/// -- critical"). This used to be derived from the *sorted position of the coin among the
/// current subscriptions*, which silently reassigned indices whenever a subscription was
/// added or dropped, misattributing every subsequent event. The registry is the only correct
/// source; before `meta` lands it returns ASSET_NONE, which every consumer already handles.
fn asset_for(coin: &str, registry: &SharedRegistry) -> u32 {
    if coin.is_empty() {
        return PC_ASSET_NONE;
    }
    registry.index_of(coin)
}

fn base(kind: u16, asset: u32, time: u64) -> PcEvent {
    PcEvent {
        kind,
        flags: 0,
        asset,
        req_id: 0,
        exch_time_ms: time,
        recv_time_ns: crate::ffi::monotonic_ns(),
        u: unsafe { std::mem::zeroed() },
    }
}
/// Strict parse: rejects >8 decimals rather than truncating. Use for anything that can reach
/// an order (`px`, `sz`) -- all <=6 decimals on the wire.
fn number(v: &Value, key: &str) -> i64 {
    string(v, key)
        .and_then(|s| parse_scaled(s).ok())
        .unwrap_or(0)
}

/// Rounding parse for DISPLAY-ONLY statistics. `funding`, `openInterest`, `dayNtlVlm`,
/// `premium` and `dayBaseVlm` arrive with 10 decimal places (measured -- docs/09 §2), so the
/// strict parser rejects them and they silently became 0, blanking the instrument strip.
/// Never use this for a price or size.
fn stat_number(v: &Value, key: &str) -> i64 {
    string(v, key)
        .and_then(|s| parse_scaled_stat_rounded(s).ok())
        .unwrap_or(0)
}
fn parse_event(
    events: &EventQueue,
    value: Value,
    registry: &SharedRegistry,
    seen_trades: &mut TradeDedupe,
) {
    let channel = string(&value, "channel").unwrap_or("");
    let data = value.get("data").unwrap_or(&Value::Null);
    // NOTE: `trades` is the one channel whose `data` is an ARRAY, so this top-level lookup
    // finds nothing there and its arm below resolves the coin per element instead. Every other
    // channel carries the coin on the data object.
    let coin = string(data, "coin")
        .or_else(|| string(data, "name"))
        .or_else(|| string(data, "s"))
        .unwrap_or("");
    let asset = asset_for(coin, registry);
    let time = integer(data, "time").unwrap_or(0);
    match channel {
        "bbo" => {
            let mut event = base(2, asset, time);
            let mut bbo = PcBbo {
                bid: PcLevel::default(),
                ask: PcLevel::default(),
                has_bid: false,
                has_ask: false,
            };
            if let Some(level) = data.get("bbo").and_then(|x| x.get(0)) {
                bbo.bid = PcLevel {
                    px: number(level, "px"),
                    sz: number(level, "sz"),
                    n: 0,
                    _pad: 0,
                };
                bbo.has_bid = true;
            }
            if let Some(level) = data.get("bbo").and_then(|x| x.get(1)) {
                bbo.ask = PcLevel {
                    px: number(level, "px"),
                    sz: number(level, "sz"),
                    n: 0,
                    _pad: 0,
                };
                bbo.has_ask = true;
            }
            event.u = PcEventUnion { bbo };
            events.push(event);
        }
        "l2Book" => {
            let mut event = base(1, asset, time);
            let mut l2 = PcL2 {
                n_bid: 0,
                n_ask: 0,
                _pad0: 0,
                _pad1: 0,
                bids: [PcLevel::default(); 24],
                asks: [PcLevel::default(); 24],
            };
            if let Some(levels) = data.get("levels").and_then(Value::as_array) {
                for (i, level) in levels
                    .first()
                    .and_then(Value::as_array)
                    .into_iter()
                    .flatten()
                    .take(24)
                    .enumerate()
                {
                    l2.bids[i] = PcLevel {
                        px: number(level, "px"),
                        sz: number(level, "sz"),
                        n: integer(level, "n").unwrap_or(0) as u32,
                        _pad: 0,
                    };
                    l2.n_bid += 1;
                }
                for (i, level) in levels
                    .get(1)
                    .and_then(Value::as_array)
                    .into_iter()
                    .flatten()
                    .take(24)
                    .enumerate()
                {
                    l2.asks[i] = PcLevel {
                        px: number(level, "px"),
                        sz: number(level, "sz"),
                        n: integer(level, "n").unwrap_or(0) as u32,
                        _pad: 0,
                    };
                    l2.n_ask += 1;
                }
            }
            // The venue delivers both l2Book subscriptions -- default and `fast:true` -- on
            // this one channel, and the payload carries nothing to distinguish them. Depth is
            // the only in-band discriminator: `fast:true` is capped at FAST_L2_MAX_LEVELS per
            // side while the default book returns up to 20. A genuinely thin instrument whose
            // default book also fits under the cap gets classified as fast, which is benign --
            // at that depth the two subscriptions are carrying the same content anyway, and it
            // self-corrects the moment the book deepens.
            const FAST_L2_MAX_LEVELS: u8 = 5;
            if l2.n_bid <= FAST_L2_MAX_LEVELS && l2.n_ask <= FAST_L2_MAX_LEVELS {
                event.flags |= PC_F_L2_FAST;
            }
            event.u = PcEventUnion { l2 };
            events.push(event);
        }
        "trades" => {
            if let Some(trades) = data.as_array() {
                for trade in trades {
                    let tid = integer(trade, "tid").unwrap_or(0);
                    // Every `trades` subscribe replays the last 30 prints. Without this, a
                    // reconnect re-emits them and the tape shows each trade twice
                    // (03 §W2.0, §W8 #12).
                    if !seen_trades.insert(tid) {
                        continue;
                    }
                    let trade_ms = integer(trade, "time").unwrap_or(time);
                    // The coin is on each trade, not on the enclosing `data` array -- resolving
                    // it from the array (as the shared `asset` above does) yields
                    // PC_ASSET_NONE, which md::MarketStore::apply then drops on its bounds
                    // check, silently emptying the tape and every trade-derived series.
                    let trade_asset = asset_for(string(trade, "coin").unwrap_or(coin), registry);
                    let mut event = base(3, trade_asset, trade_ms);
                    event.u = PcEventUnion {
                        trade: PcTrade {
                            px: number(trade, "px"),
                            sz: number(trade, "sz"),
                            tid,
                            time_ms: trade_ms,
                            is_buy: matches!(string(trade, "side"), Some("B")) as u8,
                        },
                    };
                    events.push(event);
                }
            }
        }
        "activeAssetCtx" => {
            // The payload is {"coin": "BTC", "ctx": {...}} -- every numeric lives one level
            // down under `ctx` (03 §W2.12). Reading them off `data` directly parsed every
            // field as 0, which blanked the entire instrument strip, not just the
            // 10-decimal statistics fields.
            let ctx = data.get("ctx").unwrap_or(&Value::Null);
            let mut event = base(5, asset, time);
            event.u = PcEventUnion {
                asset_ctx: PcAssetCtx {
                    mark: number(ctx, "markPx"),
                    oracle: number(ctx, "oraclePx"),
                    mid: number(ctx, "midPx"),
                    prev_day: number(ctx, "prevDayPx"),
                    day_ntl_vlm: stat_number(ctx, "dayNtlVlm"),
                    open_interest: stat_number(ctx, "openInterest"),
                    funding_1e8: stat_number(ctx, "funding"),
                },
            };
            events.push(event);
        }
        "candle" => {
            let open_ms = integer(data, "t").unwrap_or(0);
            let close_ms = integer(data, "T").unwrap_or(open_ms);
            let candle_time = if close_ms != 0 { close_ms } else { time };
            let interval = string(data, "i")
                .and_then(candle_interval_id)
                .unwrap_or(PC_IV_1M);
            let mut event = base(4, asset, candle_time);
            event.u = PcEventUnion {
                candle: PcCandle {
                    open_ms,
                    close_ms,
                    o: number(data, "o"),
                    h: number(data, "h"),
                    l: number(data, "l"),
                    c: number(data, "c"),
                    v: number(data, "v"),
                    n: integer(data, "n").unwrap_or(0) as u32,
                    interval,
                },
            };
            events.push(event);
        }
        _ => {}
    }
}

fn candle_interval_id(interval: &str) -> Option<u8> {
    Some(match interval {
        "1m" => PC_IV_1M,
        "5m" => PC_IV_5M,
        "15m" => PC_IV_15M,
        "30m" => PC_IV_30M,
        "1h" => PC_IV_1H,
        "4h" => PC_IV_4H,
        "1d" => PC_IV_1D,
        _ => return None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::codec::info::{Meta, UniverseEntry};
    use serde_json::json;

    fn registry_with(names: &[&str]) -> SharedRegistry {
        let meta = Meta {
            collateral_token: Default::default(),
            margin_tables: Default::default(),
            universe: names
                .iter()
                .map(|name| UniverseEntry {
                    name: (*name).to_string(),
                    sz_decimals: 5,
                    max_leverage: 40,
                    margin_table_id: None,
                    only_isolated: None,
                    is_delisted: None,
                    margin_mode: None,
                })
                .collect(),
        };
        SharedRegistry::from_meta(&meta)
    }

    // Regression guard for a bug that silently emptied the trade tape and every series derived
    // from it (the sub-minute candles): `trades` is the only channel whose `data` is an array,
    // so the coin is on each element rather than on the enclosing object. Resolving it from the
    // array yields PC_ASSET_NONE, and md::MarketStore::apply drops those on its bounds check --
    // no error anywhere, just a permanently empty tape.
    #[test]
    fn trades_resolve_their_asset_from_each_trade_not_the_array() {
        let registry = registry_with(&["BTC", "ETH"]);
        let events = EventQueue::new(64);
        let mut seen = TradeDedupe::default();

        parse_event(
            &events,
            json!({"channel": "trades", "data": [
                {"coin": "BTC", "side": "B", "px": "77405.0", "sz": "0.17068",
                 "time": 1787375626227u64, "tid": 397768595768333u64},
                {"coin": "ETH", "side": "A", "px": "2517.7", "sz": "1.5",
                 "time": 1787375626228u64, "tid": 397768595768334u64}]}),
            &registry,
            &mut seen,
        );

        let first = events.pop().expect("BTC trade event");
        let second = events.pop().expect("ETH trade event");
        assert_eq!(first.asset, registry.index_of("BTC"));
        assert_eq!(second.asset, registry.index_of("ETH"));
        assert_ne!(first.asset, crate::universe::ASSET_NONE);
        assert_ne!(second.asset, first.asset);
    }
}
