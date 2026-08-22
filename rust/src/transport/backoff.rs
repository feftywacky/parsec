//! Jittered exponential backoff for WebSocket reconnection: 0.5 s -> 30 s (02 §8).
//!
//! The jitter is not decoration. Every parsec instance that loses connectivity to the same
//! venue at the same moment would otherwise retry in lockstep, and a synchronised retry storm
//! is exactly what a recovering endpoint least needs.

/// Deterministic per-process jitter source. A full RNG dependency is not worth it here: the
/// only requirement is that two processes do not pick identical delays.
fn jitter_seed() -> u64 {
    use std::time::{SystemTime, UNIX_EPOCH};
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.subsec_nanos() as u64)
        .unwrap_or(0)
        | 1
}

pub const BASE_MS: u64 = 500;
pub const MAX_MS: u64 = 30_000;

#[derive(Debug)]
pub struct Backoff {
    attempt: u32,
    state: u64,
}

impl Default for Backoff {
    fn default() -> Self {
        Self {
            attempt: 0,
            state: jitter_seed(),
        }
    }
}

impl Backoff {
    /// Next delay: `500ms * 2^attempt`, capped at 30 s, then multiplied by a random factor in
    /// [0.5, 1.0] ("decorrelated"/full jitter). Never returns 0.
    pub fn next_ms(&mut self) -> u64 {
        let exp = BASE_MS
            .saturating_mul(1u64 << self.attempt.min(6))
            .min(MAX_MS);
        self.attempt = self.attempt.saturating_add(1);

        // xorshift64* -- tiny, dependency-free, and more than good enough for jitter.
        self.state ^= self.state << 13;
        self.state ^= self.state >> 7;
        self.state ^= self.state << 17;
        let half = exp / 2;
        (half + (self.state % half.max(1))).max(1)
    }

    pub fn reset(&mut self) {
        self.attempt = 0;
    }

    pub fn attempts(&self) -> u32 {
        self.attempt
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn grows_then_caps_and_always_jitters_within_bounds() {
        let mut b = Backoff::default();
        let mut last_ceiling = 0;
        for i in 0..12 {
            let ms = b.next_ms();
            let ceiling = BASE_MS.saturating_mul(1u64 << i.min(6)).min(MAX_MS);
            assert!(
                ms >= ceiling / 2,
                "attempt {i}: {ms} below half of {ceiling}"
            );
            assert!(ms <= ceiling, "attempt {i}: {ms} above ceiling {ceiling}");
            assert!(ms > 0);
            assert!(ceiling >= last_ceiling);
            last_ceiling = ceiling;
        }
        assert_eq!(last_ceiling, MAX_MS);
    }

    #[test]
    fn reset_returns_to_the_base_delay() {
        let mut b = Backoff::default();
        for _ in 0..6 {
            b.next_ms();
        }
        b.reset();
        assert!(b.next_ms() <= BASE_MS);
    }
}
