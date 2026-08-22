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
    fetch::{fetch_open_orders, fetch_user_fills_since, fill_event, push_clearinghouse_state},
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
) {
    let mut ping = tokio::time::interval(Duration::from_secs(50));
    let mut idle = tokio::time::interval(Duration::from_secs(5));
    let mut last_message = tokio::time::Instant::now();
    let mut pings_outstanding: u32 = 0;
    loop {
        tokio::select! {
            _ = ping.tick() => {
                if pings_outstanding >= 2 {
                    return;
                }
                if socket.send(Message::Ping(Vec::new())).await.is_err() {
                    return;
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
                rtt_us: 0, // the user socket carries no latency probe; only the market one does
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
            if let Ok(state) = serde_json::from_value::<info::ClearinghouseState>(data.clone()) {
                push_clearinghouse_state(events, registry, 0, &state);
            }
        }
        "notification" => {
            if let Some(text) = string(data, "notification") {
                events.push(PcEvent::error(0, text, 0));
            }
        }
        _ => {}
    }
}
