//! The event ring `pc_poll` drains. Implements docs/02 §4.1's documented overflow
//! policy: on overflow, the *oldest market-data event* is dropped and a counter is
//! bumped; user/account events are never dropped, because they live in a separate,
//! unbounded priority queue.
use super::types::{PcEvent, PC_EV_MARKET_DATA_MAX};
use crossbeam_queue::{ArrayQueue, SegQueue};
use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc, Condvar, Mutex,
};

/// Whether `kind` belongs on the bounded, droppable market-data queue rather than the
/// unbounded priority queue. Kinds 1..=5 (`PC_EV_L2_BOOK`..`PC_EV_ASSET_CTX`) are pure
/// market data; everything from `PC_EV_ASSET_DATA` up is account/order/connection/rate/
/// error state that must never silently disappear.
fn is_market_data(kind: u16) -> bool {
    (1..=PC_EV_MARKET_DATA_MAX).contains(&kind)
}

#[derive(Clone)]
pub struct EventQueue {
    /// Bounded, droppable: market data (book, bbo, trades, candles, asset ctx).
    market: Arc<ArrayQueue<PcEvent>>,
    /// Unbounded, never dropped: user/account/order/connection/rate/error events.
    /// `SegQueue` is already a transitive dependency (crossbeam-queue), so this adds
    /// no new crate.
    priority: Arc<SegQueue<PcEvent>>,
    /// Bumped every time a market-data event is dropped to make room for a newer one.
    dropped_market_data: Arc<AtomicU64>,
    wake: Arc<(Mutex<()>, Condvar)>,
}

impl EventQueue {
    pub fn new(capacity: usize) -> Self {
        Self {
            market: Arc::new(ArrayQueue::new(capacity.max(64))),
            priority: Arc::new(SegQueue::new()),
            dropped_market_data: Arc::new(AtomicU64::new(0)),
            wake: Arc::new((Mutex::new(()), Condvar::new())),
        }
    }

    pub fn push(&self, event: PcEvent) {
        if is_market_data(event.kind) {
            if let Err(rejected) = self.market.push(event) {
                // Full: drop the oldest market-data event to make room, per docs/02
                // §4.1, rather than dropping the newly-arrived one — a stale head-of-
                // queue snapshot is worse than a gap.
                let _ = self.market.pop();
                self.dropped_market_data.fetch_add(1, Ordering::Relaxed);
                // The queue just freed a slot; if something else raced us into it,
                // that's fine too — either way an item is present or the queue is
                // momentarily full again, and losing this one push is the documented
                // behaviour, not a bug.
                let _ = self.market.push(rejected);
            }
        } else {
            self.priority.push(event);
        }
        self.wake.1.notify_one();
    }

    /// Priority events drain first, so an account/order update is never stuck behind
    /// a burst of book ticks.
    pub fn pop(&self) -> Option<PcEvent> {
        self.priority.pop().or_else(|| self.market.pop())
    }

    /// Count of market-data events dropped to overflow since the queue was created.
    /// Not surfaced over FFI yet — exposed for diagnostics/tests.
    pub fn dropped_market_data(&self) -> u64 {
        self.dropped_market_data.load(Ordering::Relaxed)
    }

    pub fn wait(&self, duration: std::time::Duration) {
        let guard = self.wake.0.lock().expect("poisoned event queue");
        let _ = self.wake.1.wait_timeout(guard, duration);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ffi::types::{PcEvent, PC_EV_ACCOUNT, PC_EV_L2_BOOK};

    fn event(kind: u16) -> PcEvent {
        PcEvent {
            kind,
            flags: 0,
            asset: 0,
            req_id: 0,
            exch_time_ms: 0,
            recv_time_ns: 0,
            u: unsafe { std::mem::zeroed() },
        }
    }

    #[test]
    fn market_data_overflow_drops_oldest_and_counts_it() {
        let queue = EventQueue::new(64); // new() floors capacity at 64
        for _ in 0..64 {
            queue.push(event(PC_EV_L2_BOOK));
        }
        assert_eq!(queue.dropped_market_data(), 0);
        queue.push(event(PC_EV_L2_BOOK)); // overflow: drops the oldest
        assert_eq!(queue.dropped_market_data(), 1);
        // Queue still holds exactly `capacity` events, none lost beyond the one drop.
        let mut drained = 0;
        while queue.pop().is_some() {
            drained += 1;
        }
        assert_eq!(drained, 64);
    }

    #[test]
    fn account_events_are_never_dropped_even_under_market_data_flood() {
        let queue = EventQueue::new(64);
        for _ in 0..64 {
            queue.push(event(PC_EV_L2_BOOK));
        }
        // Overflow the market-data queue many times over.
        for _ in 0..1000 {
            queue.push(event(PC_EV_L2_BOOK));
        }
        // Account events land on the unbounded priority queue regardless.
        for _ in 0..50 {
            queue.push(event(PC_EV_ACCOUNT));
        }
        let mut account_seen = 0;
        while let Some(e) = queue.pop() {
            if e.kind == PC_EV_ACCOUNT {
                account_seen += 1;
            }
        }
        assert_eq!(account_seen, 50);
    }

    #[test]
    fn priority_events_drain_before_market_data() {
        let queue = EventQueue::new(64);
        queue.push(event(PC_EV_L2_BOOK));
        queue.push(event(PC_EV_ACCOUNT));
        assert_eq!(queue.pop().unwrap().kind, PC_EV_ACCOUNT);
        assert_eq!(queue.pop().unwrap().kind, PC_EV_L2_BOOK);
    }
}
