//! Throwaway verification harness for the network-edge task (not part of the
//! library's public surface, not wired into the C++ build). Run with:
//!
//!     cargo run --release --example verify_phase1
//!
//! Confirms, against live testnet, in one process:
//! 1. the rustls TLS panic is gone and a real WebSocket connection is established,
//!    with market-data messages actually arriving;
//! 2. `HttpClient::post_info` fetches real `meta`, and `AssetRegistry` resolves
//!    `BTC` to a stable dense asset index (the position in `meta.universe`, not a
//!    subscription-order artifact).
use parsec_rs::codec::info::Meta;
use parsec_rs::ffi::queue::EventQueue;
use parsec_rs::transport::http::HttpClient;
use parsec_rs::transport::ws::{self, Command};
use parsec_rs::universe::AssetRegistry;
use std::time::Duration;
use tokio::sync::mpsc;

#[tokio::main]
async fn main() {
    // Same provider install `pc_engine_create` performs, exercised here standalone.
    rustls::crypto::ring::default_provider()
        .install_default()
        .expect("install rustls crypto provider");

    println!("== step 1: REST meta fetch + asset registry ==");
    let http = HttpClient::new(false).expect("build http client"); // testnet
    let value = http
        .post_info(&serde_json::json!({"type": "meta", "dex": ""}))
        .await
        .expect("meta request");
    let meta: Meta = serde_json::from_value(value).expect("decode meta");
    println!("meta.universe entries: {}", meta.universe.len());
    let registry = AssetRegistry::from_meta(&meta);
    let shared = parsec_rs::universe::SharedRegistry::from_meta(&meta);
    let btc_index = registry.index_of("BTC");
    assert_ne!(
        btc_index,
        parsec_rs::universe::ASSET_NONE,
        "BTC must resolve"
    );
    println!("BTC resolved to dense asset index {btc_index}");
    // Resolve again, and resolve a different coin first, to demonstrate the index
    // is stable and NOT derived from subscription order (the bug this replaces).
    let _ = registry.index_of("ETH");
    let btc_index_again = registry.index_of("BTC");
    assert_eq!(
        btc_index, btc_index_again,
        "index must be stable across lookups"
    );
    println!("BTC index stable across lookups: {btc_index_again}");

    println!("\n== step 2: live WebSocket connection + message arrival ==");
    let events = EventQueue::new(8192);
    let (tx, rx) = mpsc::unbounded_channel();
    // Hand the ws task the registry built from the real `meta` above, so asset indices come
    // from the venue's universe rather than being fabricated from subscription order.
    tokio::spawn(ws::run(rx, events.clone(), false, shared.clone())); // testnet
    tx.send(
        Command::Subscribe {
            id: 0,
            coin: "BTC".to_string(),
            mask: 1 | 2 | 8 | 16, // BBO | L2 | TRADES | ASSET_CTX
            interval: 0,
            book: Default::default(), // the venue's native price granularity
        }
        .with_id(1),
    )
    .expect("send subscribe");

    let mut counts = std::collections::HashMap::new();
    let deadline = tokio::time::Instant::now() + Duration::from_secs(15);
    let mut connected = false;
    let mut ctx_sample: Option<parsec_rs::ffi::types::PcAssetCtx> = None;
    let mut ctx_asset = parsec_rs::universe::ASSET_NONE;
    while tokio::time::Instant::now() < deadline {
        if let Some(event) = events.pop() {
            *counts.entry(event.kind).or_insert(0u32) += 1;
            if event.kind == 12 {
                connected = true; // PC_EV_CONN
            }
            if event.kind == 5 && ctx_sample.is_none() {
                // PC_EV_ASSET_CTX
                ctx_asset = event.asset;
                ctx_sample = Some(unsafe { event.u.asset_ctx });
            }
        } else {
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
    }
    println!("connected event observed: {connected}");
    println!("event counts by kind: {counts:?}");
    assert!(connected, "must observe a PC_EV_CONN connected event");
    assert!(
        counts.values().sum::<u32>() > 1,
        "must receive more than just the connection event"
    );

    // Step 3 guards the decimal-precision bug recorded in docs/09-measurements.md §2:
    // `funding`, `openInterest` and `dayNtlVlm` arrive with 10 decimal places, which the
    // strict parser rejects -- and the rejection used to silently become 0, blanking the
    // instrument strip. These must be non-zero on a live feed.
    println!("\n== step 3: statistics fields survive the decimal parser ==");
    let ctx = ctx_sample.expect("must receive at least one activeAssetCtx");
    let scale = 1e8_f64;
    println!("  asset index   {ctx_asset}");
    println!("  mark          {:.4}", ctx.mark as f64 / scale);
    println!("  oracle        {:.4}", ctx.oracle as f64 / scale);
    println!("  funding       {:.10}", ctx.funding_1e8 as f64 / scale);
    println!("  openInterest  {:.6}", ctx.open_interest as f64 / scale);
    println!("  dayNtlVlm     {:.4}", ctx.day_ntl_vlm as f64 / scale);
    assert_ne!(ctx.mark, 0, "mark must parse");
    assert_ne!(
        ctx.funding_1e8, 0,
        "funding must not be zeroed by the strict parser"
    );
    assert_ne!(ctx.open_interest, 0, "openInterest must not be zeroed");
    assert_ne!(ctx.day_ntl_vlm, 0, "dayNtlVlm must not be zeroed");

    println!("\nOK: no TLS panic, WebSocket connected, messages arrived, meta resolved,");
    println!("    and 10-decimal statistics fields parsed non-zero.");
}
