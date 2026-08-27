//! Serde models for `POST /info` responses (docs/03-hyperliquid-api.md §2).
//!
//! Per §W8's general rule, every numeric-looking field is a decimal string on the
//! wire — fields here stay `String` (never `f64`) and get parsed into scaled `i64`
//! only at the call site that knows whether the field is order-reachable
//! (`codec::decimal::parse_scaled`) or display-only (`parse_scaled_stat_rounded`).
use serde::Deserialize;

// ---------------------------------------------------------------------------------
// meta / metaAndAssetCtxs
// ---------------------------------------------------------------------------------

/// One `meta.universe[]` entry. `only_isolated`, `is_delisted` and `margin_mode` are
/// absent from most entries on the real wire (docs/09 §3), so they must be `Option`.
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UniverseEntry {
    pub name: String,
    pub sz_decimals: u8,
    pub max_leverage: u32,
    #[serde(default)]
    pub margin_table_id: Option<u32>,
    #[serde(default)]
    pub only_isolated: Option<bool>,
    #[serde(default)]
    pub is_delisted: Option<bool>,
    #[serde(default)]
    pub margin_mode: Option<String>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MarginTier {
    pub lower_bound: String,
    pub max_leverage: u32,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MarginTable {
    #[serde(default)]
    pub description: String,
    pub margin_tiers: Vec<MarginTier>,
}

/// The `meta` response. **The array index into `universe` is the asset ID** — this is
/// the sole source of truth `universe.rs`'s registry is built from, replacing the
/// unstable subscription-order derivation `ws.rs::asset_for` used before.
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Meta {
    pub universe: Vec<UniverseEntry>,
    #[serde(default)]
    pub margin_tables: Vec<(u32, MarginTable)>,
    #[serde(default)]
    pub collateral_token: Option<i64>,
}

/// `metaAndAssetCtxs`'s per-asset context, `ctxs[i]` <-> `universe[i]`. Includes the
/// undocumented fields from docs/03 §W8 #5: `premium`, `impact_pxs`, `day_base_vlm`.
/// `mid_px` and `premium` may be `null` (empty book / no premium yet).
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct AssetCtx {
    pub day_ntl_vlm: String,
    pub funding: String,
    #[serde(default)]
    pub impact_pxs: Option<Vec<String>>,
    pub mark_px: String,
    pub mid_px: Option<String>,
    pub open_interest: String,
    pub oracle_px: String,
    pub premium: Option<String>,
    pub prev_day_px: String,
    #[serde(default)]
    pub day_base_vlm: Option<String>,
}

/// `[meta, ctxs]` — the literal 2-tuple `metaAndAssetCtxs` returns.
pub type MetaAndAssetCtxs = (Meta, Vec<AssetCtx>);

// ---------------------------------------------------------------------------------
// l2Book
// ---------------------------------------------------------------------------------

#[derive(Debug, Clone, Deserialize)]
pub struct L2Level {
    pub px: String,
    pub sz: String,
    pub n: u32,
}

/// `levels[0]` = bids (descending), `levels[1]` = asks (ascending). Modeled as
/// `Vec<Vec<L2Level>>` (not a fixed 2-tuple) because that's the literal JSON shape;
/// callers should treat any length other than 2 as a malformed response.
#[derive(Debug, Clone, Deserialize)]
pub struct L2Book {
    pub coin: String,
    pub time: u64,
    pub levels: Vec<Vec<L2Level>>,
}

// ---------------------------------------------------------------------------------
// candleSnapshot — docs/03 §W8 #2/#3: numerics are strings, and the wire sends a
// bare array of objects (not the documented `Candle[]` wrapper distinction matters
// here only in that we deserialize directly into `Vec<Candle>`).
// ---------------------------------------------------------------------------------

#[derive(Debug, Clone, Deserialize)]
pub struct Candle {
    #[serde(rename = "t")]
    pub open_ms: u64,
    #[serde(rename = "T")]
    pub close_ms: u64,
    #[serde(rename = "s")]
    pub coin: String,
    #[serde(rename = "i")]
    pub interval: String,
    #[serde(rename = "o")]
    pub open: String,
    #[serde(rename = "c")]
    pub close: String,
    #[serde(rename = "h")]
    pub high: String,
    #[serde(rename = "l")]
    pub low: String,
    #[serde(rename = "v")]
    pub volume: String,
    #[serde(rename = "n")]
    pub trade_count: u32,
}

// ---------------------------------------------------------------------------------
// clearinghouseState
// ---------------------------------------------------------------------------------

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CumFunding {
    pub all_time: String,
    pub since_change: String,
    pub since_open: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Leverage {
    #[serde(rename = "type")]
    pub kind: String, // "cross" | "isolated"
    pub value: u32,
    /// Present only when `kind == "isolated"` (docs/03 §2).
    #[serde(default)]
    pub raw_usd: Option<String>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Position {
    pub coin: String,
    pub cum_funding: CumFunding,
    pub entry_px: Option<String>,
    pub leverage: Leverage,
    pub liquidation_px: Option<String>,
    pub margin_used: String,
    pub max_leverage: u32,
    pub position_value: String,
    pub return_on_equity: String,
    pub szi: String,
    pub unrealized_pnl: String,
}

#[derive(Debug, Clone, Deserialize)]
pub struct AssetPosition {
    pub position: Position,
    #[serde(rename = "type")]
    pub kind: String, // "oneWay"
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MarginSummary {
    pub account_value: String,
    pub total_margin_used: String,
    pub total_ntl_pos: String,
    pub total_raw_usd: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ClearinghouseState {
    pub asset_positions: Vec<AssetPosition>,
    pub cross_maintenance_margin_used: String,
    pub cross_margin_summary: MarginSummary,
    pub margin_summary: MarginSummary,
    pub time: u64,
    pub withdrawable: String,
}

// ---------------------------------------------------------------------------------
// spotClearinghouseState
// ---------------------------------------------------------------------------------

/// One spot token row. `total` is the whole balance; `hold` is the part the venue has
/// locked (resting spot orders, and — for USDC — the part deployed as perp collateral).
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SpotBalance {
    pub coin: String,
    pub token: u32,
    pub total: String,
    pub hold: String,
    #[serde(default)]
    pub entry_ntl: Option<String>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SpotClearinghouseState {
    pub balances: Vec<SpotBalance>,
}

// ---------------------------------------------------------------------------------
// openOrders / frontendOpenOrders
// ---------------------------------------------------------------------------------

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct OpenOrder {
    pub coin: String,
    pub limit_px: String,
    pub oid: u64,
    pub side: String, // "B" | "A"
    pub sz: String,
    pub timestamp: u64,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FrontendOpenOrder {
    pub coin: String,
    pub is_position_tpsl: bool,
    pub is_trigger: bool,
    pub limit_px: String,
    pub oid: u64,
    pub order_type: String,
    pub orig_sz: String,
    pub reduce_only: bool,
    pub side: String,
    pub sz: String,
    pub timestamp: u64,
    pub trigger_condition: String,
    pub trigger_px: String,
    #[serde(default)]
    pub cloid: Option<String>,
}

// ---------------------------------------------------------------------------------
// userFills / userFillsByTime
// ---------------------------------------------------------------------------------

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Fill {
    pub closed_pnl: String,
    pub coin: String,
    pub crossed: bool,
    pub dir: String,
    pub hash: String,
    pub oid: u64,
    pub px: String,
    pub side: String,
    pub start_position: String,
    pub sz: String,
    pub time: u64,
    pub fee: String,
    pub fee_token: String,
    #[serde(default)]
    pub builder_fee: Option<String>,
    pub tid: u64,
    /// docs/03 §W8 #7 — undocumented, present on the WS `fill` variant; kept optional
    /// here too since the REST shape may or may not carry it.
    #[serde(default)]
    pub twap_id: Option<u64>,
}

// ---------------------------------------------------------------------------------
// userFunding
// ---------------------------------------------------------------------------------

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FundingDelta {
    pub coin: String,
    pub funding_rate: String,
    pub szi: String,
    #[serde(rename = "type")]
    pub kind: String, // "funding"
    pub usdc: String,
    #[serde(default)]
    pub n_samples: Option<u32>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct FundingEntry {
    pub delta: FundingDelta,
    pub hash: String,
    pub time: u64,
}

// ---------------------------------------------------------------------------------
// historicalOrders / orderStatus — shared `order` shape
// ---------------------------------------------------------------------------------

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct OrderDetail {
    pub coin: String,
    pub side: String,
    pub limit_px: String,
    pub sz: String,
    pub oid: u64,
    pub timestamp: u64,
    pub trigger_condition: String,
    pub is_trigger: bool,
    pub trigger_px: String,
    #[serde(default)]
    pub children: Vec<serde_json::Value>,
    pub is_position_tpsl: bool,
    pub reduce_only: bool,
    pub order_type: String,
    pub orig_sz: String,
    #[serde(default)]
    pub tif: Option<String>,
    #[serde(default)]
    pub cloid: Option<String>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct HistoricalOrder {
    pub order: OrderDetail,
    pub status: String,
    #[serde(rename = "statusTimestamp")]
    pub status_timestamp: u64,
}

/// Maps a venue order-status string to the ABI's `PC_ORD_*` codes (`include/parsec.h`).
/// Anything not modeled arrives as `PC_ORD_UNKNOWN`, matching the header's own comment.
pub fn order_status_code(status: &str) -> u16 {
    use crate::ffi::types::*;
    match status {
        "open" | "scheduledCancel" => PC_ORD_OPEN,
        "filled" => PC_ORD_FILLED,
        "canceled"
        | "vaultWithdrawalCanceled"
        | "openInterestCapCanceled"
        | "selfTradeCanceled"
        | "reduceOnlyCanceled"
        | "siblingFilledCanceled"
        | "delistedCanceled"
        | "liquidatedCanceled" => PC_ORD_CANCELED,
        "triggered" => PC_ORD_TRIGGERED,
        "rejected"
        | "tickRejected"
        | "minTradeNtlRejected"
        | "perpMarginRejected"
        | "reduceOnlyRejected"
        | "badAloPxRejected"
        | "iocCancelRejected"
        | "badTriggerPxRejected"
        | "marketOrderNoLiquidityRejected"
        | "positionIncreaseAtOpenInterestCapRejected"
        | "positionFlipAtOpenInterestCapRejected"
        | "tooAggressiveAtOpenInterestCapRejected"
        | "openInterestIncreaseRejected"
        | "insufficientSpotBalanceRejected"
        | "oracleRejected"
        | "perpMaxPositionRejected" => PC_ORD_REJECTED,
        "marginCanceled" => PC_ORD_MARGIN_CANCELED,
        _ => PC_ORD_UNKNOWN,
    }
}

// ---------------------------------------------------------------------------------
// activeAssetData
// ---------------------------------------------------------------------------------

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ActiveAssetLeverage {
    #[serde(rename = "type")]
    pub kind: String,
    pub value: u32,
}

/// docs/03 §W8 #6: arrays are `[String, String]`, not `[number, number]`, and an
/// undocumented `markPx` rides along.
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ActiveAssetData {
    pub user: String,
    pub coin: String,
    pub leverage: ActiveAssetLeverage,
    pub max_trade_szs: [String; 2],
    pub available_to_trade: [String; 2],
    pub mark_px: String,
}

// `userFees` contains a large schedule and daily-volume history. The UI only needs the
// account's effective perp rates, so keep this edge model intentionally narrow. The rates
// are decimal strings on the wire and are converted to the shared 1e8 fixed-point grid by
// `ffi::fetch`.
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UserFees {
    pub user_cross_rate: String,
    pub user_add_rate: String,
}

// ---------------------------------------------------------------------------------
// userRateLimit
// ---------------------------------------------------------------------------------

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UserRateLimit {
    pub cum_vlm: String,
    pub n_requests_used: i64,
    pub n_requests_cap: i64,
    pub n_requests_surplus: i64,
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::codec::decimal::{parse_scaled, parse_scaled_stat_rounded};
    use std::fs;

    fn fixture(name: &str) -> String {
        // Fixtures live at the repo root's tests/fixtures/info/, not rust/tests/ — the
        // crate's own tests/ only holds signing_vectors.rs.
        let path = format!(
            "{}/../tests/fixtures/info/{name}",
            env!("CARGO_MANIFEST_DIR")
        );
        fs::read_to_string(&path).unwrap_or_else(|e| panic!("read {path}: {e}"))
    }

    #[test]
    fn meta_fixture_has_210_assets_and_optional_fields_parse() {
        let meta: Meta = serde_json::from_str(&fixture("meta.json")).unwrap();
        assert_eq!(meta.universe.len(), 210);
        let btc = meta.universe.iter().find(|u| u.name == "BTC").unwrap();
        assert_eq!(btc.sz_decimals, 5);
        assert_eq!(btc.max_leverage, 40);
        // Most entries omit onlyIsolated/isDelisted/marginMode (docs/09 §3).
        assert!(meta
            .universe
            .iter()
            .any(|u| u.is_delisted == Some(true) || u.only_isolated.is_none()));
    }

    #[test]
    fn meta_and_asset_ctxs_fixture_round_trips_and_matches_universe_length() {
        let (meta, ctxs): MetaAndAssetCtxs = serde_json::from_str(&fixture("ctxs.json")).unwrap();
        assert_eq!(meta.universe.len(), ctxs.len());
        let sol_ctx = &ctxs[0];
        assert_eq!(sol_ctx.funding, "0.0000189066");
        assert_eq!(sol_ctx.open_interest, "6559.18");
        assert_eq!(sol_ctx.day_ntl_vlm, "368211.7243199999");
        assert_eq!(sol_ctx.premium.as_deref(), Some("0.0010074904"));
        assert_eq!(sol_ctx.day_base_vlm.as_deref(), Some("4255.99"));
        assert_eq!(
            sol_ctx.impact_pxs.as_deref(),
            Some(["87.5331".to_string(), "87.7312".to_string()].as_slice())
        );
        // These 10-decimal statistics fields must go through the rounding parser, not
        // the strict one (docs/09 §2) — confirm both behave as designed.
        assert!(parse_scaled(&sol_ctx.funding).is_err());
        assert_eq!(parse_scaled_stat_rounded(&sol_ctx.funding).unwrap(), 1891);
        // mark/oracle/mid/prevDay stay within 6 decimals, so the strict parser works.
        assert!(parse_scaled(&sol_ctx.mark_px).is_ok());
    }

    #[test]
    fn candle_fixture_parses_string_ohlcv() {
        let candles: Vec<Candle> = serde_json::from_str(&fixture("candles.json")).unwrap();
        assert!(!candles.is_empty());
        let c = &candles[0];
        assert_eq!(c.coin, "BTC");
        assert_eq!(c.interval, "1m");
        assert!(parse_scaled(&c.open).is_ok());
        assert!(parse_scaled(&c.volume).is_ok());
    }

    #[test]
    fn l2_fixture_parses_bids_and_asks() {
        let book: L2Book = serde_json::from_str(&fixture("l2.json")).unwrap();
        assert_eq!(book.coin, "BTC");
        assert_eq!(book.levels.len(), 2);
        assert!(!book.levels[0].is_empty());
        assert!(parse_scaled(&book.levels[0][0].px).is_ok());
    }

    #[test]
    fn clearinghouse_state_matches_doc_example() {
        let json = r#"{
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
        }"#;
        let state: ClearinghouseState = serde_json::from_str(json).unwrap();
        assert_eq!(state.asset_positions.len(), 1);
        assert_eq!(state.asset_positions[0].position.coin, "ETH");
        assert_eq!(
            state.asset_positions[0]
                .position
                .leverage
                .raw_usd
                .as_deref(),
            Some("-95.059824")
        );
    }

    /// Live mainnet shape. `tokenToAvailableAfterMaintenance` rides along undocumented and is
    /// ignored; note that it equals USDC `total` minus `crossMaintenanceMarginUsed`, which is
    /// the venue itself treating the whole spot USDC balance as perp collateral.
    #[test]
    fn spot_clearinghouse_state_decodes_the_usdc_row() {
        let json = r#"{
          "balances":[
            {"coin":"USDC","token":0,"total":"11256.53161","hold":"9821.917863",
             "entryNtl":"0.0"},
            {"coin":"USDH","token":360,"total":"0.0","hold":"0.0","entryNtl":"0.0"}
          ],
          "tokenToAvailableAfterMaintenance":[[0,"10522.568095"]]
        }"#;
        let state: SpotClearinghouseState = serde_json::from_str(json).unwrap();
        let usdc = state.balances.iter().find(|b| b.coin == "USDC").unwrap();
        assert_eq!(usdc.token, 0);
        assert_eq!(parse_scaled(&usdc.total).unwrap(), 1_125_653_161_000);
        assert_eq!(parse_scaled(&usdc.hold).unwrap(), 982_191_786_300);
    }

    #[test]
    fn open_orders_and_frontend_open_orders_match_doc_examples() {
        let plain: Vec<OpenOrder> = serde_json::from_str(
            r#"[{"coin":"BTC","limitPx":"29792.0","oid":91490942,"side":"A","sz":"0.0",
                 "timestamp":1681247412573}]"#,
        )
        .unwrap();
        assert_eq!(plain[0].oid, 91490942);

        let frontend: Vec<FrontendOpenOrder> = serde_json::from_str(
            r#"[{"coin":"BTC","isPositionTpsl":false,"isTrigger":false,"limitPx":"29792.0",
                 "oid":91490942,"orderType":"Limit","origSz":"5.0","reduceOnly":false,
                 "side":"A","sz":"5.0","timestamp":1681247412573,
                 "triggerCondition":"N/A","triggerPx":"0.0"}]"#,
        )
        .unwrap();
        assert_eq!(frontend[0].order_type, "Limit");
    }

    #[test]
    fn user_fills_match_doc_example() {
        let fills: Vec<Fill> = serde_json::from_str(
            r#"[{"closedPnl":"0.0","coin":"AVAX","crossed":false,"dir":"Open Long",
                 "hash":"0xa166e3fa","oid":90542681,"px":"18.435","side":"B",
                 "startPosition":"26.86","sz":"93.53","time":1681222254710,
                 "fee":"0.01","feeToken":"USDC","builderFee":"0.01","tid":118906512037719}]"#,
        )
        .unwrap();
        assert_eq!(fills[0].tid, 118906512037719);
        assert_eq!(fills[0].builder_fee.as_deref(), Some("0.01"));
    }

    #[test]
    fn user_funding_matches_doc_example() {
        let entries: Vec<FundingEntry> = serde_json::from_str(
            r#"[{"delta":{"coin":"ETH","fundingRate":"0.0000417","szi":"49.1477","type":"funding",
                          "usdc":"-3.625312","nSamples":null},
                 "hash":"0xa166...","time":1681222254710}]"#,
        )
        .unwrap();
        assert_eq!(entries[0].delta.kind, "funding");
        assert_eq!(entries[0].delta.n_samples, None);
    }

    #[test]
    fn historical_order_status_maps_to_pc_ord_codes() {
        assert_eq!(order_status_code("open"), 1);
        assert_eq!(order_status_code("filled"), 2);
        assert_eq!(order_status_code("canceled"), 3);
        assert_eq!(order_status_code("triggered"), 4);
        assert_eq!(order_status_code("tickRejected"), 5);
        assert_eq!(order_status_code("marginCanceled"), 6);
        assert_eq!(order_status_code("unknownOid"), 0);

        let entry: HistoricalOrder = serde_json::from_str(
            r#"{"order":{"coin":"ETH","side":"A","limitPx":"2412.7","sz":"0.0","oid":1,
                         "timestamp":1724361546645,"triggerCondition":"N/A","isTrigger":false,
                         "triggerPx":"0.0","children":[],"isPositionTpsl":false,
                         "reduceOnly":true,"orderType":"Market","origSz":"0.0076",
                         "tif":"FrontendMarket","cloid":null},
                "status":"filled","statusTimestamp":1724361546645}"#,
        )
        .unwrap();
        assert_eq!(order_status_code(&entry.status), 2);
    }

    #[test]
    fn active_asset_data_matches_doc_example() {
        let data: ActiveAssetData = serde_json::from_str(
            r#"{"user":"0xb658...","coin":"APT","leverage":{"type":"cross","value":3},
                 "maxTradeSzs":["24836370.44","24836370.44"],
                 "availableToTrade":["37019438.03","37019438.03"],"markPx":"4.4716"}"#,
        )
        .unwrap();
        assert_eq!(data.coin, "APT");
        assert_eq!(data.max_trade_szs[0], "24836370.44");
    }

    #[test]
    fn user_fees_matches_effective_rate_fields() {
        let fees: UserFees =
            serde_json::from_str(r#"{"userCrossRate":"0.00045","userAddRate":"-0.00015"}"#)
                .unwrap();
        assert_eq!(fees.user_cross_rate, "0.00045");
        assert_eq!(fees.user_add_rate, "-0.00015");
    }

    #[test]
    fn user_rate_limit_matches_doc_example() {
        let limit: UserRateLimit = serde_json::from_str(
            r#"{"cumVlm":"2854574.593578","nRequestsUsed":2890,"nRequestsCap":2864574,
                 "nRequestsSurplus":0}"#,
        )
        .unwrap();
        assert_eq!(limit.n_requests_cap, 2864574);
    }
}
