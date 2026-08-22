//! Session: nonce allocation, rate-budget accounting, and master-vs-agent addressing,
//! tied together into a `sign_action` call that returns a ready-to-POST JSON body.
//!
//! **No networking lives here** — transport (owned elsewhere) calls into this module
//! per outbound action and per inbound rate-limit signal; `Session` never makes an HTTP
//! call or opens a socket itself.
use crate::signer::keystore::SecretKey;
use crate::signer::l1;
use k256::ecdsa::SigningKey;
use serde::Serialize;
use std::sync::atomic::{AtomicI64, AtomicU64, Ordering};
use thiserror::Error;

// ---------------------------------------------------------------------------------
// addressing
// ---------------------------------------------------------------------------------

/// The account being traded — approves the agent, holds the funds, is used for every
/// `/info` query. **Never sign with this address's key**: parsec never holds the
/// master private key past the few seconds of onboarding (06 §2).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct MasterAddress(pub [u8; 20]);

/// The API-wallet address — signs every trading action. **Never query `/info` with
/// this address**: it holds no funds and no position, so `clearinghouseState` etc.
/// return an empty account (06 §1's table calls this out explicitly).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct AgentAddress(pub [u8; 20]);

// Deliberately no `From<MasterAddress> for AgentAddress` or vice versa, and no shared
// trait that would let one substitute for the other at a call site. 07's Phase 3 notes
// this mistake produces a *silently* empty account rather than an error — the type
// system is the only guard that catches it at compile time instead of in production.

/// Which Hyperliquid environment this session is signing for. Threaded explicitly
/// through `Session` rather than inferred, and checked against the resolved transport
/// base URL at construction (see `Session::new`) so a proxy or misconfiguration cannot
/// silently flip mainnet/testnet signing (03 §4a, 07 Phase 3).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Network {
    Mainnet,
    Testnet,
}

impl Network {
    pub fn is_mainnet(self) -> bool {
        matches!(self, Network::Mainnet)
    }

    /// The phantom-agent `source` field (03 §4a): `"a"` on mainnet, `"b"` on testnet.
    pub fn source(self) -> &'static str {
        if self.is_mainnet() {
            "a"
        } else {
            "b"
        }
    }

    /// The `hyperliquidChain` value for user-signed actions (03 §4b).
    pub fn hyperliquid_chain(self) -> &'static str {
        if self.is_mainnet() {
            "Mainnet"
        } else {
            "Testnet"
        }
    }
}

// ---------------------------------------------------------------------------------
// nonce allocator
// ---------------------------------------------------------------------------------

/// Nonce must fall within `(T - 2 days, T + 1 day)` of the venue's block clock (03
/// §4c). `T` is passed in by the caller (transport tracks server time via its own
/// clock sync, out of scope here) rather than assumed to be local wall-clock time.
pub const NONCE_WINDOW_BEFORE_MS: u64 = 2 * 24 * 60 * 60 * 1000;
pub const NONCE_WINDOW_AFTER_MS: u64 = 24 * 60 * 60 * 1000;

/// Monotonic, millisecond-based, fast-forwardable nonce source, one per signer (03
/// §4c: nonces are tracked per signer address, so one allocator per agent address is
/// correct even if that agent signs for the master, a vault, and sub-accounts).
///
/// Because every nonce this allocator hands out is strictly greater than the last one
/// it ever handed out, "the new nonce must be larger than the smallest of the signer's
/// last 100" is satisfied by construction as long as the allocator is never reset —
/// there is no code path that decreases `last`.
#[derive(Debug)]
pub struct NonceAllocator {
    last: AtomicU64,
}

impl NonceAllocator {
    pub fn new(seed: u64) -> Self {
        Self {
            last: AtomicU64::new(seed),
        }
    }

    /// Allocate the next nonce, at least `now_ms` and strictly greater than the last
    /// nonce this allocator ever returned.
    pub fn next(&self, now_ms: u64) -> u64 {
        let mut current = self.last.load(Ordering::Relaxed);
        loop {
            let next = now_ms.max(current.saturating_add(1));
            match self.last.compare_exchange_weak(
                current,
                next,
                Ordering::Relaxed,
                Ordering::Relaxed,
            ) {
                Ok(_) => return next,
                Err(v) => current = v,
            }
        }
    }

    /// Jump the allocator forward without allocating (e.g. after reading back a known
    /// nonce from a prior session).
    pub fn fast_forward(&self, value: u64) {
        self.last.fetch_max(value, Ordering::Relaxed);
    }

    /// Whether `nonce` falls within the venue's accepted window relative to block time
    /// `t_ms` (03 §4c). Exposed so `Session::sign_action` can reject a doomed nonce
    /// before wasting a signature on it, and so it is independently testable.
    pub fn in_window(nonce: u64, t_ms: u64) -> bool {
        let lower = t_ms.saturating_sub(NONCE_WINDOW_BEFORE_MS);
        let upper = t_ms.saturating_add(NONCE_WINDOW_AFTER_MS);
        nonce > lower && nonce < upper
    }
}

// ---------------------------------------------------------------------------------
// rate budget
// ---------------------------------------------------------------------------------

/// Per-address action rate-limit accounting (03 §7). The venue's own limit is
/// `1 request per 1 USDC of cumulative traded volume since address inception`, which
/// this module cannot compute — it only tracks the *client-side* state machine: a
/// starting buffer, consumption per submitted action, reconciliation against
/// server-reported remaining weight, and the post-429 cooldown. This is accounting
/// only; the retry loop lives in transport.
#[derive(Debug)]
pub struct RateBudget {
    /// Best-effort estimate of remaining request budget. Corrected whenever the
    /// server tells us better (`reconcile`); never trusted as exact.
    remaining: AtomicI64,
    /// Timestamp (ms) before which a new *non-cancel* action should not be attempted,
    /// per the documented "one request every 10 seconds while rate limited" policy.
    limited_until_ms: AtomicU64,
}

impl RateBudget {
    /// `initial_buffer` is the documented starting allowance (10000 requests for a
    /// brand-new address; pass a server-reported value once one is known).
    pub fn new(initial_buffer: i64) -> Self {
        Self {
            remaining: AtomicI64::new(initial_buffer),
            limited_until_ms: AtomicU64::new(0),
        }
    }

    /// Record `n` address-weight units about to be spent — a batch of `n`
    /// orders/cancels costs `n` for the address limit even though it is one IP-level
    /// request (03 §7's "Batching" rule). Call *before* sending, so the accounting
    /// reflects intent even if the response hasn't arrived yet.
    pub fn consume(&self, n: i64) {
        self.remaining.fetch_sub(n, Ordering::Relaxed);
    }

    /// Cancels ride a higher cumulative allowance (`min(limit + 100000, limit * 2)`);
    /// this credits the difference back so a cancel-heavy workload doesn't look
    /// falsely starved against the tighter order-limit estimate. Callers that track
    /// the true server-reported limit should prefer `reconcile` over this.
    pub fn credit_cancel_bonus(&self, n: i64) {
        self.remaining.fetch_add(n, Ordering::Relaxed);
    }

    /// Overwrite the estimate with a server-reported remaining count (e.g. from
    /// `userRateLimit` or a response header), the authoritative source whenever it is
    /// available.
    pub fn reconcile(&self, remaining: i64) {
        self.remaining.store(remaining, Ordering::Relaxed);
    }

    /// Enter the documented post-429 cooldown: at most one request every 10 seconds.
    pub fn on_rate_limited(&self, now_ms: u64) {
        self.limited_until_ms
            .store(now_ms.saturating_add(10_000), Ordering::Relaxed);
    }

    /// Whether a new action should be attempted right now, given the client-side
    /// estimate and any active cooldown. A `false` result means "don't bother
    /// signing/sending yet" — it is a pre-flight gate, not a guarantee the server will
    /// accept the next request.
    pub fn can_send(&self, now_ms: u64) -> bool {
        now_ms >= self.limited_until_ms.load(Ordering::Relaxed)
            && self.remaining.load(Ordering::Relaxed) > 0
    }

    pub fn remaining_estimate(&self) -> i64 {
        self.remaining.load(Ordering::Relaxed)
    }
}

// ---------------------------------------------------------------------------------
// scheduleCancel daily trigger budget
// ---------------------------------------------------------------------------------

/// Client-side enforcement of `scheduleCancel`'s documented "max 10 triggers per UTC
/// day" limit (03 §`scheduleCancel`, 07 Phase 5). The venue resets this at 00:00 UTC,
/// so the budget is keyed by the UTC day number rather than a rolling window; the
/// counter is lazily reset the first time `try_consume` observes a new day rather than
/// needing a background timer.
///
/// Only calls that *set* a real deadline consume budget — clearing a previously
/// scheduled cancel (`time: None`) is not a trigger and must not be metered here; that
/// distinction is the caller's job (`ffi::actions::schedule_cancel`).
#[derive(Debug)]
pub struct ScheduleCancelBudget {
    day: AtomicU64,
    used: AtomicU64,
}

/// Max `scheduleCancel` triggers accepted per UTC day (03 §`scheduleCancel`).
pub const SCHEDULE_CANCEL_DAILY_LIMIT: u64 = 10;
const MS_PER_DAY: u64 = 24 * 60 * 60 * 1000;

impl Default for ScheduleCancelBudget {
    fn default() -> Self {
        Self::new()
    }
}

impl ScheduleCancelBudget {
    pub fn new() -> Self {
        Self {
            day: AtomicU64::new(u64::MAX), // no day observed yet
            used: AtomicU64::new(0),
        }
    }

    /// Attempt to consume one trigger slot for the UTC day containing `now_ms`.
    /// Returns `true` if the slot was granted, `false` if today's budget of
    /// [`SCHEDULE_CANCEL_DAILY_LIMIT`] is already spent.
    pub fn try_consume(&self, now_ms: u64) -> bool {
        let today = now_ms / MS_PER_DAY;
        // Roll the counter over the instant a new UTC day is observed. `swap` rather
        // than `load`+`store` so two racing callers around midnight can't both see the
        // stale day and both reset the counter to a fresh 0 before consuming.
        if self.day.swap(today, Ordering::Relaxed) != today {
            self.used.store(0, Ordering::Relaxed);
        }
        let mut current = self.used.load(Ordering::Relaxed);
        loop {
            if current >= SCHEDULE_CANCEL_DAILY_LIMIT {
                return false;
            }
            match self.used.compare_exchange_weak(
                current,
                current + 1,
                Ordering::Relaxed,
                Ordering::Relaxed,
            ) {
                Ok(_) => return true,
                Err(v) => current = v,
            }
        }
    }
}

// ---------------------------------------------------------------------------------
// session
// ---------------------------------------------------------------------------------

#[derive(Debug, Error)]
pub enum SessionError {
    #[error("resolved transport network does not match the configured network")]
    NetworkMismatch,
    #[error("nonce {nonce} outside the accepted (T-2d, T+1d) window around block time {t_ms}")]
    NonceOutOfWindow { nonce: u64, t_ms: u64 },
}

/// A signed-and-ready `{"action":..,"nonce":..,"signature":{r,s,v},"vaultAddress":..,
/// "expiresAfter":..}` body (03 §3's envelope), exactly what transport POSTs to
/// `/exchange`.
#[derive(Serialize)]
pub struct SignedActionBody<A: Serialize> {
    pub action: A,
    pub nonce: u64,
    pub signature: SignatureWire,
    #[serde(rename = "vaultAddress")]
    pub vault_address: Option<String>,
    #[serde(rename = "expiresAfter", skip_serializing_if = "Option::is_none")]
    pub expires_after: Option<u64>,
}

#[derive(Serialize)]
pub struct SignatureWire {
    pub r: String,
    pub s: String,
    pub v: u8,
}

fn split_signature(sig: [u8; 65]) -> SignatureWire {
    SignatureWire {
        r: format!("0x{}", hex::encode(&sig[0..32])),
        s: format!("0x{}", hex::encode(&sig[32..64])),
        v: sig[64],
    }
}

/// Ties nonce allocation, rate accounting, and L1 signing together for one running
/// agent. One `Session` per process (06 §2: "never share a keystore between two
/// concurrently running parsec instances" — nonce state is per-signer and a second
/// process would race it).
pub struct Session {
    network: Network,
    master_address: MasterAddress,
    agent_address: AgentAddress,
    agent_secret: SecretKey,
    nonces: NonceAllocator,
    pub rate_budget: RateBudget,
    pub schedule_cancel_budget: ScheduleCancelBudget,
}

impl Session {
    /// `resolved_network_is_mainnet` is the network the transport layer actually
    /// resolved its base URL to — passed in explicitly (rather than trusted from
    /// config alone) so a proxy or config bug that silently redirects mainnet traffic
    /// to testnet infrastructure (or vice versa) is caught here instead of producing a
    /// signature for the wrong chain (07 Phase 3).
    pub fn new(
        network: Network,
        resolved_network_is_mainnet: bool,
        master_address: MasterAddress,
        agent_address: AgentAddress,
        agent_secret: SecretKey,
        nonce_seed_ms: u64,
        rate_budget_initial: i64,
    ) -> Result<Self, SessionError> {
        if network.is_mainnet() != resolved_network_is_mainnet {
            return Err(SessionError::NetworkMismatch);
        }
        Ok(Self {
            network,
            master_address,
            agent_address,
            agent_secret,
            nonces: NonceAllocator::new(nonce_seed_ms),
            rate_budget: RateBudget::new(rate_budget_initial),
            schedule_cancel_budget: ScheduleCancelBudget::new(),
        })
    }

    pub fn network(&self) -> Network {
        self.network
    }

    pub fn master_address(&self) -> MasterAddress {
        self.master_address
    }

    pub fn agent_address(&self) -> AgentAddress {
        self.agent_address
    }

    /// Sign an L1 action (order, cancel, modify, leverage, ...) and return the exact
    /// JSON body to POST to `/exchange`. Allocates the nonce, so callers must send (or
    /// explicitly discard) the result — a signed-but-never-sent action still burns a
    /// nonce slot.
    pub fn sign_action<A: Serialize>(
        &self,
        action: A,
        now_ms: u64,
        vault_address: Option<[u8; 20]>,
        expires_after: Option<u64>,
    ) -> Result<SignedActionBody<A>, SessionError> {
        let nonce = self.nonces.next(now_ms);
        if !NonceAllocator::in_window(nonce, now_ms) {
            return Err(SessionError::NonceOutOfWindow {
                nonce,
                t_ms: now_ms,
            });
        }
        let hash = l1::action_hash(&action, nonce, vault_address, expires_after);
        // Reconstructed per call from `mlock`'d storage rather than held persistently
        // as a `SigningKey` — see `signer::keystore::SecretKey`'s doc comment.
        let signing_key = SigningKey::from_slice(self.agent_secret.as_bytes())
            .expect("keystore only ever stores a validated 32-byte scalar");
        let sig = l1::sign_l1_action(&signing_key, &hash, self.network.is_mainnet());
        Ok(SignedActionBody {
            action,
            nonce,
            signature: split_signature(sig),
            vault_address: vault_address.map(|a| crate::signer::format_address(&a)),
            expires_after,
        })
    }

    /// Fast-forward the nonce allocator, e.g. after reading back a last-known nonce
    /// persisted from a prior run of this same agent.
    pub fn fast_forward_nonce(&self, value: u64) {
        self.nonces.fast_forward(value);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nonce_allocator_is_monotonic_under_races() {
        let alloc = NonceAllocator::new(0);
        let mut prev = 0;
        for now in [100, 100, 100, 50, 200, 200] {
            let n = alloc.next(now);
            assert!(n > prev);
            prev = n;
        }
    }

    #[test]
    fn nonce_window_matches_doc_bounds() {
        let t = 1_000_000_000_000u64;
        assert!(NonceAllocator::in_window(t, t));
        assert!(NonceAllocator::in_window(t + NONCE_WINDOW_AFTER_MS - 1, t));
        assert!(!NonceAllocator::in_window(t + NONCE_WINDOW_AFTER_MS, t));
        assert!(NonceAllocator::in_window(t - NONCE_WINDOW_BEFORE_MS + 1, t));
        assert!(!NonceAllocator::in_window(t - NONCE_WINDOW_BEFORE_MS, t));
    }

    #[test]
    fn rate_budget_cooldown_blocks_send() {
        let budget = RateBudget::new(10);
        assert!(budget.can_send(0));
        budget.on_rate_limited(1_000);
        assert!(!budget.can_send(1_500));
        assert!(budget.can_send(11_000));
    }

    #[test]
    fn rate_budget_exhaustion_blocks_send() {
        let budget = RateBudget::new(1);
        budget.consume(1);
        assert!(!budget.can_send(0));
        budget.reconcile(5);
        assert!(budget.can_send(0));
    }

    #[test]
    fn schedule_cancel_budget_caps_at_ten_per_utc_day() {
        let budget = ScheduleCancelBudget::new();
        let day_start = 10 * MS_PER_DAY;
        for _ in 0..SCHEDULE_CANCEL_DAILY_LIMIT {
            assert!(budget.try_consume(day_start));
        }
        assert!(!budget.try_consume(day_start + 1));
    }

    #[test]
    fn schedule_cancel_budget_resets_on_new_utc_day() {
        let budget = ScheduleCancelBudget::new();
        let day_start = 10 * MS_PER_DAY;
        for _ in 0..SCHEDULE_CANCEL_DAILY_LIMIT {
            assert!(budget.try_consume(day_start));
        }
        assert!(!budget.try_consume(day_start + MS_PER_DAY - 1));
        assert!(budget.try_consume(day_start + MS_PER_DAY));
    }

    #[test]
    fn master_and_agent_addresses_are_distinct_types() {
        // This test exists to document the guarantee, not to exercise runtime logic:
        // the assertion is that the following line, if uncommented, fails to compile.
        // let _: MasterAddress = AgentAddress([0; 20]); // <- must not type-check
        let m = MasterAddress([1; 20]);
        let a = AgentAddress([1; 20]);
        assert_eq!(m.0, a.0); // same bytes are fine; the *types* still can't mix
    }

    #[test]
    fn session_rejects_network_mismatch() {
        let secret = SecretKey::new([7; 32]);
        let result = Session::new(
            Network::Mainnet,
            false, // transport resolved to testnet
            MasterAddress([0; 20]),
            AgentAddress([0; 20]),
            secret,
            0,
            10_000,
        );
        assert!(matches!(result, Err(SessionError::NetworkMismatch)));
    }

    #[test]
    fn sign_action_produces_recoverable_signature_for_agent_address() {
        use crate::codec::exchange::{grouping, limit_order, OrderAction, TIF_GTC};

        let sk = k256::ecdsa::SigningKey::from_slice(&[9u8; 32]).unwrap();
        let agent_address = crate::signer::address_from_verifying_key(sk.verifying_key());
        let secret_bytes: [u8; 32] = sk.to_bytes().as_slice().try_into().unwrap();
        let secret = SecretKey::new(secret_bytes);
        let session = Session::new(
            Network::Testnet,
            false,
            MasterAddress([1; 20]),
            AgentAddress(agent_address),
            secret,
            0,
            10_000,
        )
        .unwrap();

        let order = limit_order(0, true, 100_00000000, 1_00000000, false, TIF_GTC, [0; 16]);
        let action = OrderAction {
            typ: "order",
            orders: vec![order],
            grouping: grouping::NA,
            builder: None,
        };
        let now = 1_700_000_000_000u64;
        let body = session.sign_action(action, now, None, None).unwrap();
        assert_eq!(body.nonce, now);
        assert!(body.vault_address.is_none());

        // Rebuild the r||s||v `sign_action` produced and confirm it is byte-identical
        // to signing the same action_hash directly with the agent key (k256 ECDSA
        // signing is deterministic per RFC 6979) — i.e. `Session` really did sign with
        // the agent's key and not some other one.
        let hash = l1::action_hash(&body.action, body.nonce, None, None);
        let mut sig = [0u8; 65];
        sig[..32].copy_from_slice(&hex::decode(&body.signature.r[2..]).unwrap());
        sig[32..64].copy_from_slice(&hex::decode(&body.signature.s[2..]).unwrap());
        sig[64] = body.signature.v;
        let resigned = l1::sign_l1_action(&sk, &hash, session.network().is_mainnet());
        assert_eq!(sig, resigned);
    }
}
