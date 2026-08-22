//! Authenticated `/exchange` actions: builds the ordered wire struct from
//! `codec::exchange`, signs it through `Session::sign_action`, POSTs it, and maps the
//! response onto `PC_EV_ORDER_ACK` / informational `PC_EV_ERROR` events.
//!
//! **The response trap (docs/03 §8).** `status: "ok"` at the top level is never
//! success by itself for `order`/`cancel`/`cancelByCloid`; the real outcome is one of:
//! 1. A **top-level error** — `{"status":"err","response":"<string>"}` — the whole
//!    request failed before any per-order validation ran (bad signature, unknown
//!    user, ...).
//! 2. A **bulk result** — `{"status":"ok","response":{"data":{"statuses":[...]}}}` —
//!    one element per submitted order/cancel, each independently `resting`, `filled`,
//!    `error`, or (cancel only) the bare string `"success"`.
//! 3. A **single error for the whole batch** — `{"status":"ok",
//!    "response":{"data":{"status":{"error":"..."}}}}` — pre-validation rejected the
//!    entire payload (e.g. non-reduce-only TP/SL, an empty batch). The docs' own
//!    guidance is to treat this as if the same error had been returned once per
//!    submitted element, so `map_batch_outcome` replicates it across every slot.
//!
//! Every ack pushed from here carries the `req_id` the triggering `pc_*` call
//! returned — the C++ engine correlates purely on that field (`pc_order_ack` has no
//! cloid), so getting it right is load-bearing.
use crate::codec::decimal::parse_scaled;
use crate::codec::exchange::{
    self, grouping, trigger_order, Cancel, CancelAction, CancelByCloidAction, CancelByCloidEntry,
    LeverageAction, ModifyAction, OidOrCloid, OrderAction, OrderWire, ScheduleCancelAction,
    UpdateIsolatedMarginAction, TPSL_SL, TPSL_TP,
};
use crate::ffi::queue::EventQueue;
use crate::ffi::types::*;
use crate::session::{Session, SessionError};
use crate::transport::http::{HttpClient, HttpError};
use serde::Serialize;
use serde_json::Value;
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};

pub(crate) fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

fn scaled(v: Option<&Value>) -> i64 {
    v.and_then(Value::as_str)
        .and_then(|s| parse_scaled(s).ok())
        .unwrap_or(0)
}

fn u64_field(v: Option<&Value>) -> u64 {
    v.and_then(Value::as_u64).unwrap_or(0)
}

/// One parsed element of `response.data.statuses[]`, or the batch-wide outcome from a
/// pre-validation rejection.
struct AckOutcome {
    status: u16,
    oid: u64,
    filled_sz: i64,
    avg_px: i64,
    err: String,
}

impl AckOutcome {
    fn error(message: impl Into<String>) -> Self {
        Self {
            status: PC_ACK_ERR,
            oid: 0,
            filled_sz: 0,
            avg_px: 0,
            err: message.into(),
        }
    }

    fn success() -> Self {
        Self {
            status: PC_ACK_SUCCESS,
            oid: 0,
            filled_sz: 0,
            avg_px: 0,
            err: String::new(),
        }
    }
}

/// Parse one element of an `order` action's `statuses[]` array (docs/03 §3's response
/// examples): `{"resting":{"oid":..}}`, `{"filled":{...}}`, `{"error":"..."}`, or the
/// bare strings `"waitingForFill"`/`"waitingForTrigger"`.
fn parse_order_status(elem: &Value) -> AckOutcome {
    if let Some(s) = elem.as_str() {
        return match s {
            "waitingForFill" => AckOutcome {
                status: PC_ACK_WAITING_FOR_FILL,
                oid: 0,
                filled_sz: 0,
                avg_px: 0,
                err: String::new(),
            },
            "waitingForTrigger" => AckOutcome {
                status: PC_ACK_WAITING_FOR_TRIGGER,
                oid: 0,
                filled_sz: 0,
                avg_px: 0,
                err: String::new(),
            },
            other => AckOutcome::error(format!("unrecognized order status: {other}")),
        };
    }
    if let Some(resting) = elem.get("resting") {
        return AckOutcome {
            status: PC_ACK_RESTING,
            oid: u64_field(resting.get("oid")),
            filled_sz: 0,
            avg_px: 0,
            err: String::new(),
        };
    }
    if let Some(filled) = elem.get("filled") {
        return AckOutcome {
            status: PC_ACK_FILLED,
            oid: u64_field(filled.get("oid")),
            filled_sz: scaled(filled.get("totalSz")),
            avg_px: scaled(filled.get("avgPx")),
            err: String::new(),
        };
    }
    if let Some(err) = elem.get("error").and_then(Value::as_str) {
        return AckOutcome::error(err.to_string());
    }
    AckOutcome::error("unrecognized order status shape")
}

/// Parse one element of a `cancel`/`cancelByCloid` action's `statuses[]` array: the
/// bare string `"success"` or `{"error":"..."}`.
fn parse_cancel_status(elem: &Value) -> AckOutcome {
    if elem.as_str() == Some("success") {
        return AckOutcome::success();
    }
    if let Some(err) = elem.get("error").and_then(Value::as_str) {
        return AckOutcome::error(err.to_string());
    }
    AckOutcome::error("unrecognized cancel status shape")
}

/// Walk one `/exchange` response and produce exactly `expected` outcomes, handling
/// all three shapes from docs/03 §8 (see this module's doc comment). `parse_elem`
/// decodes one element of a per-order/per-cancel `statuses[]` array.
fn map_batch_outcome(
    response: &Value,
    expected: usize,
    parse_elem: impl Fn(&Value) -> AckOutcome,
) -> Vec<AckOutcome> {
    let expected = expected.max(1);
    // Shape 1: top-level failure — `response` itself is a bare string.
    if response.get("status").and_then(Value::as_str) == Some("err") {
        let message = response
            .get("response")
            .and_then(Value::as_str)
            .unwrap_or("request rejected")
            .to_string();
        return (0..expected).map(|_| AckOutcome::error(&message)).collect();
    }
    let data = response.pointer("/response/data");
    // Shape 2: one element per submitted order/cancel.
    if let Some(statuses) = data
        .and_then(|d| d.get("statuses"))
        .and_then(Value::as_array)
    {
        return statuses.iter().map(parse_elem).collect();
    }
    // Shape 3: pre-validation rejected the whole batch as a single error — the docs
    // recommend treating this as if it applied to every element (docs/03 §8's
    // "Pre-validation" note).
    if let Some(error) = data
        .and_then(|d| d.get("status"))
        .and_then(|s| s.get("error"))
        .and_then(Value::as_str)
    {
        return (0..expected).map(|_| AckOutcome::error(error)).collect();
    }
    // `{"status":"ok","response":{"type":"default"}}` — success with no per-element
    // detail (updateLeverage, updateIsolatedMargin, scheduleCancel). Callers that
    // expect per-order acks never hit this arm because `statuses` is always present
    // for `order`/`cancel`/`cancelByCloid`.
    (0..expected).map(|_| AckOutcome::success()).collect()
}

fn ack_event(req_id: u64, asset: u32, outcome: AckOutcome) -> PcEvent {
    let mut err = [0 as std::os::raw::c_char; PC_ERR_LEN];
    for (dst, src) in err.iter_mut().zip(outcome.err.as_bytes()) {
        *dst = *src as std::os::raw::c_char;
    }
    PcEvent {
        kind: PC_EV_ORDER_ACK,
        flags: 0,
        asset,
        req_id,
        exch_time_ms: 0,
        recv_time_ns: crate::ffi::monotonic_ns(),
        u: PcEventUnion {
            ack: PcOrderAck {
                status: outcome.status,
                oid: outcome.oid,
                filled_sz: outcome.filled_sz,
                avg_px: outcome.avg_px,
                err,
            },
        },
    }
}

fn info_event(req_id: u64, code: i32, message: &str) -> PcEvent {
    PcEvent::error(code, message, req_id)
}

/// Sign `action` and POST it to `/exchange`, honouring the client-side rate budget
/// pre-flight gate (03 §7) and the documented 429 backoff. Returns the decoded JSON
/// response body, or a human-readable error with no signature/account material in it.
async fn sign_and_send<A: Serialize>(
    session: &Session,
    http: &HttpClient,
    events: &EventQueue,
    req_id: u64,
    action: A,
    address_weight: i64,
) -> Result<Value, String> {
    let now = now_ms();
    if !session.rate_budget.can_send(now) {
        return Err(
            "address rate budget exhausted or in post-429 cooldown; try again shortly".to_string(),
        );
    }
    let body = session
        .sign_action(action, now, None, None)
        .map_err(|e| match e {
            SessionError::NetworkMismatch => "signer network does not match transport".into(),
            SessionError::NonceOutOfWindow { nonce, t_ms } => {
                format!("nonce {nonce} fell outside the accepted window around {t_ms}")
            }
        })?;
    let json = serde_json::to_value(&body).map_err(|e| format!("encode failed: {e}"))?;
    // Address-limit accounting is spent optimistically, before the response is known
    // (03 §7's batching rule: n orders/cancels cost n address-limit units even though
    // this is one HTTP request) — an eventual 429 already means the estimate was
    // stale, so `reconcile` (via userRateLimit) is the authoritative correction, not
    // this pre-flight guess.
    session.rate_budget.consume(address_weight);
    http.post_exchange_with_retry(&json, |retry_after_ms| {
        events.push(PcEvent {
            kind: PC_EV_RATE,
            flags: 0,
            asset: PC_ASSET_NONE,
            req_id,
            exch_time_ms: 0,
            recv_time_ns: crate::ffi::monotonic_ns(),
            u: PcEventUnion {
                rate: PcRate {
                    remaining: session.rate_budget.remaining_estimate(),
                    reset_ms: retry_after_ms,
                },
            },
        });
        if retry_after_ms > 0 {
            session.rate_budget.on_rate_limited(now_ms());
        }
    })
    .await
    .map_err(describe_http_error)
}

fn describe_http_error(e: HttpError) -> String {
    match e {
        HttpError::RateLimited { .. } => "rate limited; exhausted local retry budget".to_string(),
        HttpError::Server(code) => format!("venue returned server error {code}"),
        HttpError::Client(code) => format!("venue returned client error {code}"),
        HttpError::Decode => "malformed response body".to_string(),
        HttpError::Request(msg) => format!("request failed: {msg}"),
    }
}

// ---------------------------------------------------------------------------------
// pc_order -> OrderWire
// ---------------------------------------------------------------------------------

fn order_wire(o: &PcOrder) -> OrderWire {
    if o.tpsl == PC_TPSL_NONE {
        exchange::limit_order(
            o.asset,
            o.is_buy != 0,
            o.limit_px,
            o.sz,
            o.reduce_only != 0,
            o.tif,
            o.cloid,
        )
    } else {
        let tpsl = if o.tpsl == PC_TPSL_TP {
            TPSL_TP
        } else {
            TPSL_SL
        };
        trigger_order(
            o.asset,
            o.is_buy != 0,
            o.limit_px,
            o.trigger_px,
            o.sz,
            o.reduce_only != 0,
            o.is_market_trigger != 0,
            tpsl,
            o.cloid,
        )
    }
}

fn grouping_str(g: u8) -> &'static str {
    match g {
        PC_GROUP_NORMAL_TPSL => grouping::NORMAL_TPSL,
        PC_GROUP_POSITION_TPSL => grouping::POSITION_TPSL,
        _ => grouping::NA,
    }
}

// ---------------------------------------------------------------------------------
// pc_place_order
// ---------------------------------------------------------------------------------

pub async fn place_order(
    session: Arc<Session>,
    http: HttpClient,
    events: EventQueue,
    req_id: u64,
    req: PcOrderReq,
) {
    let n = (req.n_orders as usize).min(req.orders.len());
    if n == 0 {
        events.push(info_event(req_id, -5, "order batch is empty"));
        return;
    }
    let orders: Vec<PcOrder> = req.orders[..n].to_vec();
    let wire: Vec<OrderWire> = orders.iter().map(order_wire).collect();
    let assets: Vec<u32> = orders.iter().map(|o| o.asset).collect();
    let action = OrderAction {
        typ: "order",
        orders: wire,
        grouping: grouping_str(req.grouping),
        builder: None,
    };
    match sign_and_send(&session, &http, &events, req_id, action, n as i64).await {
        Ok(response) => {
            for (outcome, asset) in map_batch_outcome(&response, n, parse_order_status)
                .into_iter()
                .zip(assets)
            {
                events.push(ack_event(req_id, asset, outcome));
            }
        }
        Err(message) => events.push(info_event(req_id, -6, &message)),
    }
}

// ---------------------------------------------------------------------------------
// pc_cancel_order / pc_cancel_by_cloid
// ---------------------------------------------------------------------------------

pub async fn cancel_order(
    session: Arc<Session>,
    http: HttpClient,
    events: EventQueue,
    req_id: u64,
    asset: u32,
    oid: u64,
) {
    let action = CancelAction {
        typ: "cancel",
        cancels: vec![Cancel { a: asset, o: oid }],
        // `f` omitted (false): a cancel_order caller doesn't tell us whether this is a
        // trigger order, and `f: true` rejects those outright (03 §"cancel").
        f: false,
    };
    match sign_and_send(&session, &http, &events, req_id, action, 1).await {
        Ok(response) => {
            for outcome in map_batch_outcome(&response, 1, parse_cancel_status) {
                let mut outcome = outcome;
                if outcome.status == PC_ACK_SUCCESS {
                    outcome.oid = oid;
                }
                events.push(ack_event(req_id, asset, outcome));
            }
        }
        Err(message) => events.push(info_event(req_id, -6, &message)),
    }
}

pub async fn cancel_by_cloid(
    session: Arc<Session>,
    http: HttpClient,
    events: EventQueue,
    req_id: u64,
    asset: u32,
    cloid: [u8; 16],
) {
    let action = CancelByCloidAction {
        typ: "cancelByCloid",
        cancels: vec![CancelByCloidEntry {
            asset,
            cloid: format!("0x{}", hex::encode(cloid)),
        }],
        f: false,
    };
    match sign_and_send(&session, &http, &events, req_id, action, 1).await {
        Ok(response) => {
            for outcome in map_batch_outcome(&response, 1, parse_cancel_status) {
                events.push(ack_event(req_id, asset, outcome));
            }
        }
        Err(message) => events.push(info_event(req_id, -6, &message)),
    }
}

// ---------------------------------------------------------------------------------
// pc_modify_order
// ---------------------------------------------------------------------------------

pub async fn modify_order(
    session: Arc<Session>,
    http: HttpClient,
    events: EventQueue,
    req_id: u64,
    oid: u64,
    order: PcOrder,
) {
    let asset = order.asset;
    let action = ModifyAction {
        typ: "modify",
        oid: OidOrCloid::Oid(oid),
        order: order_wire(&order),
        // `always_place` stays false/omitted: a modify that can't cancel the resting
        // order should not silently place a brand-new one (03 §"modify").
        a: false,
    };
    match sign_and_send(&session, &http, &events, req_id, action, 1).await {
        Ok(response) => {
            // `modify` has no `statuses[]` of its own on success — it mirrors `order`'s
            // shape when it fails validation, and returns `{"type":"default"}` when it
            // succeeds; `map_batch_outcome`'s fallback arm covers the latter.
            for outcome in map_batch_outcome(&response, 1, parse_order_status) {
                events.push(ack_event(req_id, asset, outcome));
            }
        }
        Err(message) => events.push(info_event(req_id, -6, &message)),
    }
}

// ---------------------------------------------------------------------------------
// pc_cancel_all
// ---------------------------------------------------------------------------------

/// No batch "cancel everything" action exists on the venue — `cancel_all` first reads
/// back every resting order via `openOrders` (queried with the **master** address per
/// docs/03 §5) and then submits one `cancel` batch naming each of them.
pub async fn cancel_all(
    session: Arc<Session>,
    http: HttpClient,
    events: EventQueue,
    registry: crate::universe::SharedRegistry,
    req_id: u64,
) {
    let master = crate::signer::format_address(&session.master_address().0);
    let body = serde_json::json!({"type": "openOrders", "user": master});
    let open = match http
        .post_info_with_retry(&body, |_| {})
        .await
        .map_err(describe_http_error)
    {
        Ok(v) => v,
        Err(message) => {
            events.push(info_event(
                req_id,
                -7,
                &format!("openOrders fetch failed: {message}"),
            ));
            return;
        }
    };
    let orders: Vec<crate::codec::info::OpenOrder> = match serde_json::from_value(open) {
        Ok(v) => v,
        Err(e) => {
            events.push(info_event(
                req_id,
                -7,
                &format!("openOrders decode failed: {e}"),
            ));
            return;
        }
    };
    if orders.is_empty() {
        events.push(info_event(req_id, 0, "no open orders to cancel"));
        return;
    }
    let cancels: Vec<(u32, Cancel)> = orders
        .iter()
        .map(|o| {
            let asset = registry.index_of(&o.coin);
            (asset, Cancel { a: asset, o: o.oid })
        })
        .collect();
    let n = cancels.len();
    let action = CancelAction {
        typ: "cancel",
        cancels: cancels
            .iter()
            .map(|(_, c)| Cancel { a: c.a, o: c.o })
            .collect(),
        f: false,
    };
    match sign_and_send(&session, &http, &events, req_id, action, n as i64).await {
        Ok(response) => {
            for ((asset, cancel), outcome) in
                cancels
                    .into_iter()
                    .zip(map_batch_outcome(&response, n, parse_cancel_status))
            {
                let mut outcome = outcome;
                if outcome.status == PC_ACK_SUCCESS {
                    outcome.oid = cancel.o;
                }
                events.push(ack_event(req_id, asset, outcome));
            }
        }
        Err(message) => events.push(info_event(req_id, -6, &message)),
    }
}

// ---------------------------------------------------------------------------------
// pc_set_leverage / pc_set_iso_margin
// ---------------------------------------------------------------------------------

pub async fn set_leverage(
    session: Arc<Session>,
    http: HttpClient,
    events: EventQueue,
    req_id: u64,
    asset: u32,
    is_cross: bool,
    leverage: u32,
) {
    let action = LeverageAction {
        typ: "updateLeverage",
        asset,
        is_cross,
        leverage,
    };
    match sign_and_send(&session, &http, &events, req_id, action, 1).await {
        Ok(response) => {
            for outcome in map_batch_outcome(&response, 1, parse_order_status) {
                if outcome.status == PC_ACK_ERR {
                    events.push(info_event(req_id, -8, &outcome.err));
                } else {
                    events.push(info_event(req_id, 0, "leverage updated"));
                }
            }
        }
        Err(message) => events.push(info_event(req_id, -6, &message)),
    }
}

pub async fn set_iso_margin(
    session: Arc<Session>,
    http: HttpClient,
    events: EventQueue,
    req_id: u64,
    asset: u32,
    delta_usd_1e8: i64,
) {
    let action = UpdateIsolatedMarginAction {
        typ: "updateIsolatedMargin",
        asset,
        // `isBuy` has no effect until hedge mode ships; the SDK hardcodes `true`
        // (03 §"updateIsolatedMargin").
        is_buy: true,
        ntli: exchange::scaled_to_ntli(delta_usd_1e8),
    };
    match sign_and_send(&session, &http, &events, req_id, action, 1).await {
        Ok(response) => {
            for outcome in map_batch_outcome(&response, 1, parse_order_status) {
                if outcome.status == PC_ACK_ERR {
                    events.push(info_event(req_id, -8, &outcome.err));
                } else {
                    events.push(info_event(req_id, 0, "isolated margin updated"));
                }
            }
        }
        Err(message) => events.push(info_event(req_id, -6, &message)),
    }
}

// ---------------------------------------------------------------------------------
// pc_schedule_cancel — the dead man's switch
// ---------------------------------------------------------------------------------

/// `deadline_ms == 0` clears a previously scheduled cancel; otherwise `deadline_ms`
/// must be at least 5 s in the future (03 §"scheduleCancel") and consumes one of the
/// 10 daily trigger slots tracked in `session.schedule_cancel_budget` — enforced
/// client-side so the budget cannot be burned by an automatic heartbeat loop calling
/// this every ~10 s (07 Phase 5's dead-man's-switch heartbeat).
pub async fn schedule_cancel(
    session: Arc<Session>,
    http: HttpClient,
    events: EventQueue,
    req_id: u64,
    deadline_ms: u64,
) {
    let now = now_ms();
    let time = if deadline_ms == 0 {
        None
    } else {
        if deadline_ms < now.saturating_add(5_000) {
            events.push(info_event(
                req_id,
                -9,
                "scheduleCancel deadline must be at least 5s in the future",
            ));
            return;
        }
        if !session.schedule_cancel_budget.try_consume(now) {
            events.push(info_event(
                req_id,
                -9,
                "scheduleCancel daily trigger budget (10 per UTC day) is exhausted",
            ));
            return;
        }
        Some(deadline_ms)
    };
    let action = ScheduleCancelAction {
        typ: "scheduleCancel",
        time,
    };
    match sign_and_send(&session, &http, &events, req_id, action, 1).await {
        Ok(response) => {
            for outcome in map_batch_outcome(&response, 1, parse_order_status) {
                if outcome.status == PC_ACK_ERR {
                    events.push(info_event(req_id, -8, &outcome.err));
                } else {
                    events.push(info_event(req_id, 0, "scheduleCancel updated"));
                }
            }
        }
        Err(message) => events.push(info_event(req_id, -6, &message)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn top_level_error_is_replicated_across_every_expected_slot() {
        let response = serde_json::json!({"status":"err","response":"L1 error: does not exist."});
        let outcomes = map_batch_outcome(&response, 3, parse_order_status);
        assert_eq!(outcomes.len(), 3);
        assert!(outcomes.iter().all(|o| o.status == PC_ACK_ERR));
        assert!(outcomes[0].err.contains("does not exist"));
    }

    #[test]
    fn bulk_result_maps_resting_filled_and_error_elements() {
        let response = serde_json::json!({"status":"ok","response":{"type":"order","data":{"statuses":[
            {"resting":{"oid":77738308}},
            {"filled":{"totalSz":"0.02","avgPx":"1891.4","oid":77747314}},
            {"error":"Order must have minimum value of $10."}
        ]}}});
        let outcomes = map_batch_outcome(&response, 3, parse_order_status);
        assert_eq!(outcomes.len(), 3);
        assert_eq!(outcomes[0].status, PC_ACK_RESTING);
        assert_eq!(outcomes[0].oid, 77738308);
        assert_eq!(outcomes[1].status, PC_ACK_FILLED);
        assert_eq!(outcomes[1].oid, 77747314);
        assert_eq!(outcomes[1].avg_px, parse_scaled("1891.4").unwrap());
        assert_eq!(outcomes[1].filled_sz, parse_scaled("0.02").unwrap());
        assert_eq!(outcomes[2].status, PC_ACK_ERR);
        assert!(outcomes[2].err.contains("minimum value"));
    }

    #[test]
    fn single_batch_error_is_duplicated_across_every_expected_slot() {
        // Pre-validation rejection shape (03 §8's "Pre-validation" note): one error,
        // no `statuses[]` array at all.
        let response = serde_json::json!({"status":"ok","response":{"type":"order",
            "data":{"status":{"error":"Order would have immediately matched"}}}});
        let outcomes = map_batch_outcome(&response, 2, parse_order_status);
        assert_eq!(outcomes.len(), 2);
        assert!(outcomes
            .iter()
            .all(|o| o.status == PC_ACK_ERR && o.err.contains("immediately matched")));
    }

    #[test]
    fn cancel_success_and_error_strings_map_correctly() {
        let response = serde_json::json!({"status":"ok","response":{"type":"cancel","data":{"statuses":[
            "success",
            {"error":"Order was never placed, already canceled, or filled."}
        ]}}});
        let outcomes = map_batch_outcome(&response, 2, parse_cancel_status);
        assert_eq!(outcomes[0].status, PC_ACK_SUCCESS);
        assert_eq!(outcomes[1].status, PC_ACK_ERR);
    }

    #[test]
    fn default_type_response_maps_to_a_single_success() {
        let response = serde_json::json!({"status":"ok","response":{"type":"default"}});
        let outcomes = map_batch_outcome(&response, 1, parse_order_status);
        assert_eq!(outcomes.len(), 1);
        assert_eq!(outcomes[0].status, PC_ACK_SUCCESS);
    }

    #[test]
    fn waiting_statuses_map_to_their_own_ack_codes() {
        let response = serde_json::json!({"status":"ok","response":{"type":"order","data":{"statuses":[
            "waitingForFill", "waitingForTrigger"
        ]}}});
        let outcomes = map_batch_outcome(&response, 2, parse_order_status);
        assert_eq!(outcomes[0].status, PC_ACK_WAITING_FOR_FILL);
        assert_eq!(outcomes[1].status, PC_ACK_WAITING_FOR_TRIGGER);
    }
}
