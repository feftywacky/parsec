//! `POST /exchange` action bodies (03 §3), as explicit ordered `#[derive(Serialize)]`
//! structs — **never** a map or `serde_json::Value`.
//!
//! Msgpack field order is load-bearing: `signer::l1::action_hash` commits to the
//! insertion order of the msgpack map it serializes an action into, and that order
//! must match the venue's own canonical field order exactly. Routing an action through
//! `serde_json::Value` (a `BTreeMap` under the hood) silently sorts keys alphabetically
//! and produces a signature that recovers the wrong address — measured in 03 §4a and
//! pinned as a regression in `tests/signing_vectors.rs`. So every struct here declares
//! its fields in the exact order 03 §4a specifies, and stays that way.
use super::decimal::scaled_to_string;
use serde::Serialize;

/// `#[serde(skip_serializing_if = "is_false")]` predicate for the several `f`/`a`
/// optional boolean flags that the venue rejects outright if serialized as `false`
/// instead of omitted (03 §4a).
fn is_false(v: &bool) -> bool {
    !*v
}

// ---------------------------------------------------------------------------------
// order types
// ---------------------------------------------------------------------------------

#[derive(Serialize)]
pub struct Limit {
    pub tif: &'static str,
}

/// Field order `isMarket, triggerPx, tpsl` — required by 03 §4a's ordering table.
#[derive(Serialize)]
pub struct Trigger {
    #[serde(rename = "isMarket")]
    pub is_market: bool,
    #[serde(rename = "triggerPx")]
    pub trigger_px: String,
    pub tpsl: &'static str,
}

#[derive(Serialize)]
#[serde(untagged)]
pub enum OrderType {
    Limit { limit: Limit },
    Trigger { trigger: Trigger },
}

/// Field order `a, b, p, s, r, t, c?` (03 §4a).
#[derive(Serialize)]
pub struct OrderWire {
    pub a: u32,
    pub b: bool,
    pub p: String,
    pub s: String,
    pub r: bool,
    pub t: OrderType,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub c: Option<String>,
}

/// Builder fee recipient. `b` **must be lowercased** before signing (03 §4a); `f` is
/// tenths of a basis point (`f=10` -> 1bp), max 100 for perps.
#[derive(Serialize)]
pub struct Builder {
    pub b: String,
    pub f: u32,
}

/// `grouping` values parsec sends. The `{"p": <rate>}` priority-grouping object exists
/// in the API but is out of scope for v1 and not modeled here.
pub mod grouping {
    pub const NA: &str = "na";
    pub const NORMAL_TPSL: &str = "normalTpsl";
    pub const POSITION_TPSL: &str = "positionTpsl";
}

/// Field order `type, orders, grouping, builder?` (03 §4a).
#[derive(Serialize)]
pub struct OrderAction {
    #[serde(rename = "type")]
    pub typ: &'static str,
    pub orders: Vec<OrderWire>,
    pub grouping: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub builder: Option<Builder>,
}

// ---------------------------------------------------------------------------------
// cancel / cancelByCloid
// ---------------------------------------------------------------------------------

#[derive(Serialize)]
pub struct Cancel {
    pub a: u32,
    pub o: u64,
}

/// Field order `type, cancels, f?` (03 §4a). `f` (fast) must be omitted when false.
#[derive(Serialize)]
pub struct CancelAction {
    #[serde(rename = "type")]
    pub typ: &'static str,
    pub cancels: Vec<Cancel>,
    #[serde(skip_serializing_if = "is_false")]
    pub f: bool,
}

/// Note the asymmetry with `Cancel`: this uses `asset`/`cloid` spelled out, not `a`/`o`
/// (03 §3).
#[derive(Serialize)]
pub struct CancelByCloidEntry {
    pub asset: u32,
    pub cloid: String,
}

/// Field order `type, cancels, f?` (03 §4a).
#[derive(Serialize)]
pub struct CancelByCloidAction {
    #[serde(rename = "type")]
    pub typ: &'static str,
    pub cancels: Vec<CancelByCloidEntry>,
    #[serde(skip_serializing_if = "is_false")]
    pub f: bool,
}

// ---------------------------------------------------------------------------------
// modify / batchModify
// ---------------------------------------------------------------------------------

/// `oid` is a resting order id **or** a cloid string (03 §3).
#[derive(Serialize)]
#[serde(untagged)]
pub enum OidOrCloid {
    Oid(u64),
    Cloid(String),
}

/// Field order `type, oid, order, a?` — top-level `a` is `always_place`, must be
/// omitted when false (03 §3).
#[derive(Serialize)]
pub struct ModifyAction {
    #[serde(rename = "type")]
    pub typ: &'static str,
    pub oid: OidOrCloid,
    pub order: OrderWire,
    #[serde(skip_serializing_if = "is_false")]
    pub a: bool,
}

#[derive(Serialize)]
pub struct BatchModifyEntry {
    pub oid: OidOrCloid,
    pub order: OrderWire,
}

/// Field order `type, modifies, a?` (03 §4a).
#[derive(Serialize)]
pub struct BatchModifyAction {
    #[serde(rename = "type")]
    pub typ: &'static str,
    pub modifies: Vec<BatchModifyEntry>,
    #[serde(skip_serializing_if = "is_false")]
    pub a: bool,
}

// ---------------------------------------------------------------------------------
// leverage / isolated margin
// ---------------------------------------------------------------------------------

/// Field order `type, asset, isCross, leverage` (03 §4a).
#[derive(Serialize)]
pub struct LeverageAction {
    #[serde(rename = "type")]
    pub typ: &'static str,
    pub asset: u32,
    #[serde(rename = "isCross")]
    pub is_cross: bool,
    pub leverage: u32,
}

/// Field order `type, asset, isBuy, ntli` (03 §4a). `ntli` is `round(usd * 1e6)` as a
/// **signed integer**, not a decimal string like every other wire number.
#[derive(Serialize)]
pub struct UpdateIsolatedMarginAction {
    #[serde(rename = "type")]
    pub typ: &'static str,
    pub asset: u32,
    #[serde(rename = "isBuy")]
    pub is_buy: bool,
    pub ntli: i64,
}

/// Convert parsec's fixed-point `i64 @ 1e8` USD amount into the `ntli` integer scale
/// (`1e6`), i.e. `round(usd * 1e6)`. Exact integer division by 100 with round-half-away
/// -from-zero on the two dropped decimal digits — no float intermediate.
pub fn scaled_to_ntli(usd_1e8: i64) -> i64 {
    let negative = usd_1e8 < 0;
    let magnitude = usd_1e8.unsigned_abs();
    let rounded = (magnitude + 50) / 100;
    if negative {
        -(rounded as i64)
    } else {
        rounded as i64
    }
}

// ---------------------------------------------------------------------------------
// scheduleCancel / noop
// ---------------------------------------------------------------------------------

/// Dead man's switch. `time` omitted clears a previously scheduled cancel.
#[derive(Serialize)]
pub struct ScheduleCancelAction {
    #[serde(rename = "type")]
    pub typ: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub time: Option<u64>,
}

/// Exists specifically to burn/invalidate a pending nonce (03 §4c).
#[derive(Serialize)]
pub struct NoopAction {
    #[serde(rename = "type")]
    pub typ: &'static str,
}

// ---------------------------------------------------------------------------------
// approveAgent (user-signed, but posted through the same envelope)
// ---------------------------------------------------------------------------------

/// The wire body for `approveAgent`. This is a *user-signed* action (03 §4b) — its
/// signature comes from `signer::agent`/`signer::user_signed`, not `signer::l1` — but
/// it is still POSTed through the same `/exchange` envelope, so its JSON shape lives
/// here alongside the L1 actions.
///
/// `agent_name: None` reproduces a documented Python-SDK quirk exactly: an unnamed
/// approval is *signed* with `agentName: ""` (see `signer::agent::ApproveAgentRequest`)
/// but the field is then omitted from the action actually posted.
#[derive(Serialize)]
pub struct ApproveAgentAction {
    #[serde(rename = "type")]
    pub typ: &'static str,
    #[serde(rename = "hyperliquidChain")]
    pub hyperliquid_chain: &'static str,
    #[serde(rename = "signatureChainId")]
    pub signature_chain_id: String,
    #[serde(rename = "agentAddress")]
    pub agent_address: String,
    #[serde(rename = "agentName", skip_serializing_if = "Option::is_none")]
    pub agent_name: Option<String>,
    pub nonce: u64,
}

// ---------------------------------------------------------------------------------
// convenience constructors
// ---------------------------------------------------------------------------------

/// TIF codes used by `limit_order`/`pc_order.tif` at the FFI boundary (04 §4's
/// `PC_TIF_*`), duplicated here as plain constants since this module must not depend
/// on `ffi::types`.
pub const TIF_GTC: u8 = 0;
pub const TIF_IOC: u8 = 1;
pub const TIF_ALO: u8 = 2;
pub const TIF_FRONTEND_MARKET: u8 = 3;

fn tif_str(tif: u8) -> &'static str {
    match tif {
        TIF_IOC => "Ioc",
        TIF_ALO => "Alo",
        TIF_FRONTEND_MARKET => "FrontendMarket",
        _ => "Gtc",
    }
}

/// `cloid` of all zero bytes means "no cloid" and is omitted, matching the optional
/// `c` field (03 §3: `^0x[a-fA-F0-9]{32}$` when present).
fn cloid_string(cloid: [u8; 16]) -> Option<String> {
    if cloid.iter().all(|b| *b == 0) {
        None
    } else {
        Some(format!("0x{}", hex::encode(cloid)))
    }
}

pub fn limit_order(
    asset: u32,
    is_buy: bool,
    price: i64,
    size: i64,
    reduce_only: bool,
    tif: u8,
    cloid: [u8; 16],
) -> OrderWire {
    OrderWire {
        a: asset,
        b: is_buy,
        p: scaled_to_string(price),
        s: scaled_to_string(size),
        r: reduce_only,
        t: OrderType::Limit {
            limit: Limit { tif: tif_str(tif) },
        },
        c: cloid_string(cloid),
    }
}

/// `tpsl` must be `"tp"` or `"sl"` — pass exactly one of `TPSL_TP`/`TPSL_SL`.
pub const TPSL_TP: &str = "tp";
pub const TPSL_SL: &str = "sl";

#[allow(clippy::too_many_arguments)]
pub fn trigger_order(
    asset: u32,
    is_buy: bool,
    limit_price: i64,
    trigger_price: i64,
    size: i64,
    reduce_only: bool,
    is_market: bool,
    tpsl: &'static str,
    cloid: [u8; 16],
) -> OrderWire {
    OrderWire {
        a: asset,
        b: is_buy,
        p: scaled_to_string(limit_price),
        s: scaled_to_string(size),
        r: reduce_only,
        t: OrderType::Trigger {
            trigger: Trigger {
                is_market,
                trigger_px: scaled_to_string(trigger_price),
                tpsl,
            },
        },
        c: cloid_string(cloid),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::signer::l1::action_hash;

    #[test]
    fn ordered_wire_matches_oracle() {
        let w = limit_order(1, true, 1800_00000000, 2_000000, false, TIF_GTC, [0; 16]);
        let h = action_hash(
            &OrderAction {
                typ: "order",
                orders: vec![w],
                grouping: grouping::NA,
                builder: None,
            },
            1690393044548,
            None,
            None,
        );
        assert_eq!(
            hex::encode(h),
            "b8ed9e5a4ea7b2aebdb09fd794221c2599487f6dc8d3bf8f38656ab295f36120"
        );
    }

    #[test]
    fn ntli_conversion_matches_doc_example() {
        // 1000000 = 1 USDC (03 §3 example: ntli=1000000).
        assert_eq!(scaled_to_ntli(1_00000000), 1_000_000);
        assert_eq!(scaled_to_ntli(-1_00000000), -1_000_000);
        assert_eq!(scaled_to_ntli(0), 0);
        // Rounding on the two dropped decimal digits (1e8 -> 1e6 scale).
        assert_eq!(scaled_to_ntli(1_00000049), 1_000_000); // rounds down
        assert_eq!(scaled_to_ntli(1_00000050), 1_000_001); // rounds up (half-away-from-zero)
    }

    #[test]
    fn cloid_all_zero_is_omitted() {
        assert_eq!(cloid_string([0; 16]), None);
        assert!(cloid_string([1; 16]).is_some());
    }
}
