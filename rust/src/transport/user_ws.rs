//! The **user** WebSocket (docs/03 §W9): a second connection, independent of
//! `transport::ws`'s market-data socket, carrying every account-scoped subscription —
//! `orderUpdates`, `userFills`, `userFundings`, `clearinghouseState`, `notification`.
//! Every event this module emits carries `socket = PC_SOCK_USER` on its `PC_EV_CONN`
//! events so the C++ status bar can track the two sockets independently.
//!
//! Reuses the same hardening `transport::ws` already proved out for the market
//! socket — jittered backoff, resubscribe-on-reconnect, missed-pong teardown — rather
//! than inventing a second copy of that logic with different bugs.
//!
//! **`orderUpdates` has no snapshot** (docs/03 §W2.0): subscribing does not hand back
//! the resting order book, only future deltas. So on every connect *and* reconnect,
//! after subscribing, this module additionally pulls `openOrders` and
//! `userFillsByTime(since=last_seen)` over REST and pushes them as ordinary
//! snapshot-flagged events — the same reconciliation docs/02 §8 describes. The C++
//! `OrderRouter`/`Reconciler` then treats those exactly like any other snapshot.
use crate::codec::{decimal::parse_scaled, info, ws_msg::string};
use crate::ffi::{
    fetch::{
        fetch_clearinghouse_state, fetch_open_orders, fetch_user_fills_since, fill_event,
        push_clearinghouse_state,
    },
    queue::EventQueue,
    types::*,
};
use crate::session::Session;
use crate::transport::backoff::Backoff;
use crate::transport::http::HttpClient;
use crate::universe::SharedRegistry;
use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use std::{
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc,
    },
    time::Duration,
};
use tokio_tungstenite::tungstenite::Message;

type Socket =
    tokio_tungstenite::WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>;

const SOCK_USER: u8 = PC_SOCK_USER;

/// Runs until the process exits or the owning engine is dropped (this task is
/// detached, same lifecycle as `transport::ws::run`). Started once, right after a
/// keystore finishes unlocking — there is nothing to subscribe to before the master
/// address is known.
pub async fn run(
    events: EventQueue,
    http: HttpClient,
    registry: SharedRegistry,
    session: Arc<Session>,
    mainnet: bool,
) {
    let endpoint = if mainnet {
        "wss://api.hyperliquid.xyz/ws"
    } else {
        "wss://api.hyperliquid-testnet.xyz/ws"
    };
    let master = crate::signer::format_address(&session.master_address().0);
    // Persisted across reconnects (not just across messages within one connection):
    // this is exactly what makes the post-reconnect `userFillsByTime(since=...)` pull
    // backfill only what was actually missed.
    let last_seen_fill_ms = Arc::new(AtomicU64::new(crate::ffi::actions::now_ms()));

    let mut backoff = Backoff::default();
    let mut reconnects: u32 = 0;
    loop {
        match tokio_tungstenite::connect_async(endpoint).await {
            Ok((mut socket, _)) => {
                reconnects = 0;
                backoff.reset();
                emit_conn(&events, CONN_CONNECTED, reconnects);
                for payload in subscriptions(&master) {
                    let request = json!({"method": "subscribe", "subscription": payload});
                    let _ = socket.send(Message::Text(request.to_string())).await;
                }
                // `orderUpdates` sends no snapshot — backfill via REST every time this
                // loop (re)connects, per this module's doc comment.
                reconcile(&http, &events, &registry, &master, &last_seen_fill_ms).await;

                run_connection(
                    &mut socket,
                    &events,
                    &registry,
                    &last_seen_fill_ms,
                    &mut backoff,
                    reconnects,
                )
                .await;
            }
            Err(error) => events.push(PcEvent::error(
                -3,
                &format!("user websocket connect failed: {error}"),
                0,
            )),
        }
        emit_conn(&events, CONN_DISCONNECTED, reconnects);
        reconnects = reconnects.saturating_add(1);
        emit_conn(&events, CONN_RECONNECTING, reconnects);
        tokio::time::sleep(Duration::from_millis(backoff.next_ms())).await;
    }
}

fn subscriptions(master: &str) -> [Value; 5] {
    [
        json!({"type": "orderUpdates", "user": master}),
        json!({"type": "userFills", "user": master, "aggregateByTime": false}),
        json!({"type": "userFundings", "user": master}),
        json!({"type": "clearinghouseState", "user": master, "dex": ""}),
        json!({"type": "notification", "user": master}),
    ]
}

/// The `openOrders` + `userFillsByTime(since=last_seen)` backfill described in this
/// module's doc comment. Best-effort: a failure here just means the next ~4 s
/// `clearinghouseState`/`openOrders` re-push (or the next reconnect) tries again —
/// it must never block the socket from otherwise coming up.
async fn reconcile(
    http: &HttpClient,
    events: &EventQueue,
    registry: &SharedRegistry,
    master: &str,
    last_seen_fill_ms: &AtomicU64,
) {
    // The `clearinghouseState` subscription is the steady-state source of positions and
    // margin, but its first push can be several seconds out -- and until one lands the C++
    // engine has no account snapshot, which is what gates its own ~4 s REST reconciler. Pull
    // it once here so an authenticated session has buying power immediately on connect
    // instead of waiting on (or deadlocking against) that first subscription push.
    if let Err(message) = fetch_clearinghouse_state(http, events, registry, master, 0).await {
        events.push(PcEvent::error(
            -11,
            &format!("clearinghouseState reconcile: {message}"),
            0,
        ));
    }
    if let Err(message) = fetch_open_orders(http, events, registry, master, 0).await {
        events.push(PcEvent::error(
            -11,
            &format!("orderUpdates reconcile: {message}"),
            0,
        ));
    }
    let since = last_seen_fill_ms.load(Ordering::Relaxed);
    if let Err(message) = fetch_user_fills_since(http, events, registry, master, since).await {
        events.push(PcEvent::error(
            -11,
            &format!("fills reconcile: {message}"),
            0,
        ));
    }
}

async fn run_connection(
    socket: &mut Socket,
    events: &EventQueue,
    registry: &SharedRegistry,
    last_seen_fill_ms: &Arc<AtomicU64>,
    _backoff: &mut Backoff,
    reconnects: u32,
) {
    let mut ping = tokio::time::interval(Duration::from_secs(50));
    let mut idle = tokio::time::interval(Duration::from_secs(5));
    let mut last_message = tokio::time::Instant::now();
    let mut pings_outstanding: u32 = 0;
    // Only the oldest unanswered ping is timed; pongs are not correlated to a specific ping,
    // so timing the newest would under-report RTT whenever one goes missing (same rule as
    // transport/ws.rs).
    let mut ping_sent_at: Option<tokio::time::Instant> = None;
    loop {
        tokio::select! {
            _ = ping.tick() => {
                if pings_outstanding >= 2 {
                    return;
                }
                if socket.send(Message::Ping(Vec::new())).await.is_err() {
                    return;
                }
                if ping_sent_at.is_none() {
                    ping_sent_at = Some(tokio::time::Instant::now());
                }
                pings_outstanding += 1;
            }
            _ = idle.tick() => {
                if last_message.elapsed() > Duration::from_secs(60) {
                    return;
                }
            }
            frame = socket.next() => match frame {
                Some(Ok(Message::Text(text))) => {
                    last_message = tokio::time::Instant::now();
                    match serde_json::from_str::<Value>(&text) {
                        Ok(value) => parse_event(events, registry, value, last_seen_fill_ms),
                        Err(_) if text.starts_with("Websocket connection") => {}
                        Err(_) => events.push(PcEvent::error(-4, "non-JSON frame from user socket", 0)),
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
                        emit_conn_rtt(events, CONN_CONNECTED, reconnects, rtt as u32);
                    }
                }
                Some(Ok(Message::Close(_))) | Some(Err(_)) | None => return,
                _ => {}
            }
        }
    }
}

const CONN_DISCONNECTED: u8 = PC_CONN_DISCONNECTED;
const CONN_CONNECTED: u8 = PC_CONN_CONNECTED;
const CONN_RECONNECTING: u8 = PC_CONN_RECONNECTING;

fn emit_conn(events: &EventQueue, state: u8, reconnects: u32) {
    emit_conn_rtt(events, state, reconnects, 0)
}

/// The user socket carries fills and order acks, so its round trip -- not the market socket's
/// -- is what an order actually pays on the way back. Measured from the same ping/pong probe
/// transport/ws.rs uses, just on a 50 s interval rather than 5 s.
fn emit_conn_rtt(events: &EventQueue, state: u8, reconnects: u32, rtt_us: u32) {
    events.push(PcEvent {
        kind: PC_EV_CONN,
        flags: 0,
        asset: PC_ASSET_NONE,
        req_id: 0,
        exch_time_ms: 0,
        recv_time_ns: crate::ffi::monotonic_ns(),
        u: PcEventUnion {
            conn: PcConn {
                socket: SOCK_USER,
                state,
                reconnects,
                rtt_us,
            },
        },
    });
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

fn clearinghouse_state_payload(data: &Value) -> &Value {
    data.get("clearinghouseState").unwrap_or(data)
}

fn parse_event(
    events: &EventQueue,
    registry: &SharedRegistry,
    value: Value,
    last_seen_fill_ms: &AtomicU64,
) {
    let channel = string(&value, "channel").unwrap_or("");
    let data = value.get("data").unwrap_or(&Value::Null);
    match channel {
        "orderUpdates" => {
            let Some(updates) = data.as_array() else {
                return;
            };
            for update in updates {
                let Some(order) = update.get("order") else {
                    continue;
                };
                let coin = string(order, "coin").unwrap_or("");
                let asset = registry.index_of(coin);
                let timestamp = order.get("timestamp").and_then(Value::as_u64).unwrap_or(0);
                let mut event = base(PC_EV_ORDER_UPDATE, asset, timestamp);
                let status = string(update, "status").unwrap_or("");
                let side = string(order, "side").unwrap_or("");
                let sz = order
                    .get("sz")
                    .and_then(Value::as_str)
                    .and_then(|s| parse_scaled(s).ok())
                    .unwrap_or(0);
                let orig_sz = order
                    .get("origSz")
                    .and_then(Value::as_str)
                    .and_then(|s| parse_scaled(s).ok())
                    .unwrap_or(0);
                let px = order
                    .get("limitPx")
                    .and_then(Value::as_str)
                    .and_then(|s| parse_scaled(s).ok())
                    .unwrap_or(0);
                let oid = order.get("oid").and_then(Value::as_u64).unwrap_or(0);
                event.u = PcEventUnion {
                    order_update: PcOrderUpdate {
                        oid,
                        cloid: [0; 16], // WS `orderUpdates.order.cloid` isn't surfaced onward yet.
                        status: info::order_status_code(status),
                        px,
                        sz,
                        orig_sz,
                        is_buy: (side == "B") as u8,
                        reduce_only: 0, // not present on this payload (docs/03 §W2.7).
                    },
                };
                events.push(event);
            }
        }
        "userFills" => {
            let Some(fills) = data.get("fills").and_then(Value::as_array) else {
                return;
            };
            let is_snapshot = data
                .get("isSnapshot")
                .and_then(Value::as_bool)
                .unwrap_or(false);
            let flags = if is_snapshot { PC_F_SNAPSHOT } else { 0 };
            for raw in fills {
                let Ok(f) = serde_json::from_value::<info::Fill>(raw.clone()) else {
                    continue;
                };
                last_seen_fill_ms.fetch_max(f.time, Ordering::Relaxed);
                events.push(fill_event(registry, 0, flags, &f));
            }
        }
        "userFundings" => {
            // No dedicated event kind exists for funding payments — see
            // `ffi::fetch::fetch_user_funding`'s doc comment for the same tradeoff on
            // the REST side. Streaming fundings are dropped rather than force-fit,
            // since (unlike the REST path) there is no `pc_fetch` caller waiting on a
            // result here to disappoint; `PC_FETCH_USER_FUNDING` remains the
            // documented way to get funding history into the event stream.
        }
        "clearinghouseState" => {
            // Surface a decode failure rather than dropping it: this payload is the only
            // source of account value / buying power, and silently skipping it looks
            // identical to "not signed in" everywhere downstream.
            // The user websocket currently wraps the state as
            // {"user": ..., "dex": "", "clearinghouseState": {...}}, while the REST
            // `clearinghouseState` endpoint returns the inner object directly. Accept both
            // forms because they describe the same account snapshot.
            let state_value = clearinghouse_state_payload(data);
            match serde_json::from_value::<info::ClearinghouseState>(state_value.clone()) {
                Ok(state) => push_clearinghouse_state(events, registry, 0, &state),
                Err(error) => events.push(PcEvent::error(
                    -12,
                    &format!("clearinghouseState decode failed: {error}"),
                    0,
                )),
            }
        }
        // A rejected subscription (bad address, unknown sub type) comes back on this
        // channel; swallowing it left the account panes blank with no explanation.
        "error" => {
            let text = data.as_str().unwrap_or("unknown user-socket error");
            events.push(PcEvent::error(-13, text, 0));
        }
        "notification" => {
            if let Some(text) = string(data, "notification") {
                events.push(PcEvent::error(0, text, 0));
            }
        }
        _ => {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ffi::types::PC_EV_ACCOUNT;

    #[test]
    fn clearinghouse_state_websocket_envelope_is_unwrapped() {
        let message = json!({
            "channel": "clearinghouseState",
            "data": {
                "user": "0x0000000000000000000000000000000000000001",
                "dex": "",
                "clearinghouseState": {
                    "assetPositions": [],
                    "crossMaintenanceMarginUsed": "0.0",
                    "crossMarginSummary": {
                        "accountValue": "100.0",
                        "totalMarginUsed": "0.0",
                        "totalNtlPos": "0.0",
                        "totalRawUsd": "100.0"
                    },
                    "marginSummary": {
                        "accountValue": "100.0",
                        "totalMarginUsed": "0.0",
                        "totalNtlPos": "0.0",
                        "totalRawUsd": "100.0"
                    },
                    "time": 1,
                    "withdrawable": "100.0"
                }
            }
        });
        let events = EventQueue::new(64);
        let registry = SharedRegistry::empty();
        let last_seen_fill_ms = AtomicU64::new(0);

        parse_event(&events, &registry, message, &last_seen_fill_ms);

        assert_eq!(events.pop().unwrap().kind, PC_EV_ACCOUNT);
        assert!(events.pop().is_none());
    }
}
