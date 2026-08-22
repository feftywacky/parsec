//! The asset registry: `coin name <-> dense asset index`, resolved from `meta` at
//! startup (docs/03 "Asset ID encoding — critical").
//!
//! **Why this exists**: `transport/ws.rs::asset_for` currently derives the asset
//! index by sorting the *current subscription set*'s coin names and taking a
//! position in that sorted list — the index shifts as subscriptions come and go, so
//! events silently get attributed to the wrong asset. The correct asset ID is the
//! entry's **position in `meta.universe`**, which is stable for the life of the
//! process (barring a rare re-list). This module is that stable source.
//!
//! `ws.rs` is owned by another agent; wiring this registry into `asset_for` is a
//! follow-up there. The lookup API below (`SharedRegistry`) is what that call site
//! needs: cheap to clone into the WS task, cheap to query per message, and it never
//! blocks on a write for longer than a `RwLock` read/write pair.
use crate::codec::info::Meta;
use std::collections::HashMap;
use std::sync::{Arc, RwLock};

/// Per-asset metadata resolved from `meta.universe[i]`.
#[derive(Debug, Clone)]
pub struct AssetInfo {
    pub name: String,
    pub sz_decimals: u8,
    pub max_leverage: u32,
    pub only_isolated: bool,
}

/// `PC_ASSET_NONE` (`0xFFFFFFFF`) — mirrors `include/parsec/parsec.h`. Duplicated here
/// (rather than imported from `ffi::types`) to keep `universe.rs` usable without
/// pulling in the FFI module; the two are asserted equal in tests below.
pub const ASSET_NONE: u32 = u32::MAX;

/// The registry itself: dense index -> info, and name -> index for the reverse
/// lookup. Immutable once built — `SharedRegistry` handles being replaced wholesale
/// on a re-fetch (e.g. after a re-list) via `RwLock`.
#[derive(Debug, Clone, Default)]
pub struct AssetRegistry {
    by_index: Vec<AssetInfo>,
    by_name: HashMap<String, u32>,
}

impl AssetRegistry {
    pub fn from_meta(meta: &Meta) -> Self {
        let mut by_index = Vec::with_capacity(meta.universe.len());
        let mut by_name = HashMap::with_capacity(meta.universe.len());
        for (i, entry) in meta.universe.iter().enumerate() {
            let index = i as u32;
            by_name.insert(entry.name.clone(), index);
            by_index.push(AssetInfo {
                name: entry.name.clone(),
                sz_decimals: entry.sz_decimals,
                max_leverage: entry.max_leverage,
                only_isolated: entry.only_isolated.unwrap_or(false),
            });
        }
        Self { by_index, by_name }
    }

    pub fn len(&self) -> usize {
        self.by_index.len()
    }

    pub fn is_empty(&self) -> bool {
        self.by_index.is_empty()
    }

    /// The dense asset index for `coin` (its position in `meta.universe`), or
    /// `ASSET_NONE` if unknown to this registry.
    pub fn index_of(&self, coin: &str) -> u32 {
        self.by_name.get(coin).copied().unwrap_or(ASSET_NONE)
    }

    pub fn info(&self, index: u32) -> Option<&AssetInfo> {
        self.by_index.get(index as usize)
    }
}

/// Cheaply-cloneable handle to an `AssetRegistry` that can be shared with the WS task
/// and swapped wholesale on refresh. Every method locks briefly and returns owned
/// data (or an index) — callers never hold the lock.
///
/// **API for the `ws.rs` owner**: clone a `SharedRegistry` into the WS task at
/// startup (or pass it into `ws::run`) and replace `asset_for`'s subscription-sort
/// logic with `registry.index_of(coin)`.
#[derive(Debug, Clone)]
pub struct SharedRegistry(Arc<RwLock<AssetRegistry>>);

impl SharedRegistry {
    pub fn empty() -> Self {
        Self(Arc::new(RwLock::new(AssetRegistry::default())))
    }

    pub fn from_meta(meta: &Meta) -> Self {
        Self(Arc::new(RwLock::new(AssetRegistry::from_meta(meta))))
    }

    /// Replace the registry wholesale, e.g. after a fresh `PC_FETCH_META` pull.
    pub fn set(&self, registry: AssetRegistry) {
        if let Ok(mut guard) = self.0.write() {
            *guard = registry;
        }
    }

    /// `coin -> dense asset index`, or `ASSET_NONE` if the registry hasn't been
    /// populated yet or doesn't know this coin. Never blocks on a writer for more
    /// than a lock acquisition.
    pub fn index_of(&self, coin: &str) -> u32 {
        self.0
            .read()
            .map(|guard| guard.index_of(coin))
            .unwrap_or(ASSET_NONE)
    }

    /// A cloned snapshot of one asset's metadata, or `None` if `index` is out of
    /// range or the registry is empty.
    pub fn info(&self, index: u32) -> Option<AssetInfo> {
        self.0.read().ok()?.info(index).cloned()
    }

    pub fn len(&self) -> usize {
        self.0.read().map(|g| g.len()).unwrap_or(0)
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::codec::info::Meta;
    use std::fs;

    fn meta_fixture() -> Meta {
        let path = format!(
            "{}/../tests/fixtures/info/meta.json",
            env!("CARGO_MANIFEST_DIR")
        );
        let text = fs::read_to_string(&path).unwrap_or_else(|e| panic!("read {path}: {e}"));
        serde_json::from_str(&text).unwrap()
    }

    #[test]
    fn asset_none_matches_the_abi_constant() {
        assert_eq!(ASSET_NONE, crate::ffi::types::PC_ASSET_NONE);
    }

    #[test]
    fn resolves_btc_to_a_stable_index_from_the_real_testnet_universe() {
        let meta = meta_fixture();
        let registry = AssetRegistry::from_meta(&meta);
        assert_eq!(registry.len(), 210);

        let expected_index = meta.universe.iter().position(|u| u.name == "BTC").unwrap() as u32;
        assert_eq!(registry.index_of("BTC"), expected_index);
        assert_eq!(registry.info(expected_index).unwrap().name, "BTC");
        assert_eq!(registry.info(expected_index).unwrap().sz_decimals, 5);
        assert_eq!(registry.info(expected_index).unwrap().max_leverage, 40);

        // Stability: the index does not depend on subscription order, unlike the old
        // `ws.rs::asset_for` — querying twice, or querying a different coin first,
        // must not perturb BTC's index.
        let _ = registry.index_of("ETH");
        assert_eq!(registry.index_of("BTC"), expected_index);
    }

    #[test]
    fn unknown_coin_resolves_to_asset_none() {
        let registry = AssetRegistry::from_meta(&meta_fixture());
        assert_eq!(registry.index_of("NOT_A_REAL_COIN"), ASSET_NONE);
    }

    #[test]
    fn shared_registry_is_cloneable_and_reflects_updates_across_clones() {
        let shared = SharedRegistry::empty();
        let clone = shared.clone();
        assert_eq!(clone.index_of("BTC"), ASSET_NONE);

        shared.set(AssetRegistry::from_meta(&meta_fixture()));
        // The clone sees the update because both share the same underlying lock.
        assert_ne!(clone.index_of("BTC"), ASSET_NONE);
    }

    #[test]
    fn only_isolated_defaults_to_false_when_absent_from_the_wire() {
        let registry = AssetRegistry::from_meta(&meta_fixture());
        let btc = registry.info(registry.index_of("BTC")).unwrap();
        assert!(!btc.only_isolated);
    }
}
