//! `POST /info` and `POST /exchange` against the Hyperliquid REST API. Connection
//! pooled, HTTP/2, keep-alive, pre-warmed at startup per `docs/02 §4.1`. rustls only —
//! no native-tls, so no macOS Security/SystemConfiguration frameworks get linked
//! (`docs/04 §1.2`; verified via `rustc --print native-static-libs`, see the top-level
//! report for this task).
//!
//! Testnet only for now: mainnet stays gated per `docs/07`'s standing rule, even
//! though `HttpClient::new` accepts a `mainnet` flag for when that gate lifts.
//!
//! **Never logs the request or response body** — both can carry account data, and a
//! signed `/exchange` body carries a signature over key-derived data. Errors here
//! carry only status codes and short, non-body-derived messages.
use crate::transport::backoff::Backoff;
use serde_json::Value;
use std::time::Duration;
use thiserror::Error;

pub const MAINNET_BASE: &str = "https://api.hyperliquid.xyz";
pub const TESTNET_BASE: &str = "https://api.hyperliquid-testnet.xyz";

/// How many times `post_with_retry` will back off and retry a 429 before giving up.
const MAX_RATE_LIMIT_RETRIES: u32 = 4;

#[derive(Debug, Error)]
pub enum HttpError {
    #[error("request failed: {0}")]
    Request(String),
    #[error("rate limited (HTTP 429)")]
    RateLimited { retry_after_ms: u64 },
    #[error("server error: HTTP {0}")]
    Server(u16),
    #[error("client error: HTTP {0}")]
    Client(u16),
    #[error("malformed response body")]
    Decode,
}

#[derive(Clone)]
pub struct HttpClient {
    client: reqwest::Client,
    base: &'static str,
}

impl HttpClient {
    /// Build a pooled, keep-alive client. Does not perform any I/O itself — call
    /// `prewarm` once at startup to pay the first TLS handshake off the critical path.
    pub fn new(mainnet: bool) -> Result<Self, HttpError> {
        let base = if mainnet { MAINNET_BASE } else { TESTNET_BASE };
        let client = reqwest::Client::builder()
            .pool_idle_timeout(Duration::from_secs(90))
            .pool_max_idle_per_host(4)
            .tcp_keepalive(Duration::from_secs(60))
            .timeout(Duration::from_secs(10))
            .build()
            .map_err(|e| HttpError::Request(e.to_string()))?;
        Ok(Self { client, base })
    }

    /// Fire a cheap `meta` pull to warm the connection pool and TLS session before
    /// anything real is on the critical path (`docs/02 §4.1`). Errors are swallowed —
    /// a failed prewarm just means the *next* real call pays the handshake cost, it
    /// is not itself an actionable failure.
    pub async fn prewarm(&self) {
        let _ = self
            .post_info(&serde_json::json!({"type": "meta", "dex": ""}))
            .await;
    }

    pub async fn post_info(&self, body: &Value) -> Result<Value, HttpError> {
        self.post(&format!("{}/info", self.base), body).await
    }

    pub async fn post_exchange(&self, body: &Value) -> Result<Value, HttpError> {
        self.post(&format!("{}/exchange", self.base), body).await
    }

    /// Single attempt, no retry. A 429 comes back as `HttpError::RateLimited` rather
    /// than being retried internally, so callers that need to surface a `PC_EV_RATE`
    /// event (docs/03 §7) can do so before deciding whether to retry at all.
    async fn post(&self, url: &str, body: &Value) -> Result<Value, HttpError> {
        let response = self
            .client
            .post(url)
            .json(body)
            .send()
            .await
            .map_err(|e| HttpError::Request(e.to_string()))?;
        let status = response.status();
        if status.as_u16() == 429 {
            // The docs never mention a machine-readable 429 body (docs/03 §7) — back
            // off on the status code alone, using Retry-After only as an optional hint.
            let retry_after_ms = response
                .headers()
                .get(reqwest::header::RETRY_AFTER)
                .and_then(|v| v.to_str().ok())
                .and_then(|s| s.parse::<u64>().ok())
                .map(|secs| secs.saturating_mul(1000))
                .unwrap_or(1000);
            return Err(HttpError::RateLimited { retry_after_ms });
        }
        if status.is_server_error() {
            return Err(HttpError::Server(status.as_u16()));
        }
        if status.is_client_error() {
            return Err(HttpError::Client(status.as_u16()));
        }
        let text = response
            .text()
            .await
            .map_err(|e| HttpError::Request(e.to_string()))?;
        serde_json::from_str(&text).map_err(|_| HttpError::Decode)
    }

    /// `post_info`/`post_exchange` with the documented "back off and retry" behaviour
    /// for 429s (docs/03 §7). `on_rate_limited` is called once per 429 encountered
    /// (before sleeping) so the caller can emit a `PC_EV_RATE` event; the queue/event
    /// wiring itself lives in `ffi`, not here, so this module stays decoupled from the
    /// event ring.
    pub async fn post_info_with_retry(
        &self,
        body: &Value,
        on_rate_limited: impl FnMut(u64),
    ) -> Result<Value, HttpError> {
        Self::with_retry(on_rate_limited, || self.post_info(body)).await
    }

    /// `post_exchange` with the same 429 backoff-and-retry behaviour as
    /// `post_info_with_retry`. **Retrying a signed action is safe here only because
    /// the caller already allocated a fresh nonce before calling this** — a 429 means
    /// the request never reached matching, so replaying the identical signed body is
    /// not a double-submit risk the way retrying after a timeout would be.
    pub async fn post_exchange_with_retry(
        &self,
        body: &Value,
        on_rate_limited: impl FnMut(u64),
    ) -> Result<Value, HttpError> {
        Self::with_retry(on_rate_limited, || self.post_exchange(body)).await
    }

    async fn with_retry<'a, F, Fut>(
        mut on_rate_limited: impl FnMut(u64),
        mut attempt_once: F,
    ) -> Result<Value, HttpError>
    where
        F: FnMut() -> Fut,
        Fut: std::future::Future<Output = Result<Value, HttpError>> + 'a,
    {
        let mut backoff = Backoff::default();
        for attempt in 0..=MAX_RATE_LIMIT_RETRIES {
            match attempt_once().await {
                Err(HttpError::RateLimited { retry_after_ms }) => {
                    on_rate_limited(retry_after_ms);
                    if attempt == MAX_RATE_LIMIT_RETRIES {
                        return Err(HttpError::RateLimited { retry_after_ms });
                    }
                    let wait = retry_after_ms.max(backoff.next_ms());
                    tokio::time::sleep(Duration::from_millis(wait)).await;
                }
                other => return other,
            }
        }
        unreachable!("loop always returns on its last iteration")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn testnet_is_the_default_base() {
        let client = HttpClient::new(false).unwrap();
        assert_eq!(client.base, TESTNET_BASE);
    }

    #[test]
    fn mainnet_flag_selects_mainnet_base() {
        let client = HttpClient::new(true).unwrap();
        assert_eq!(client.base, MAINNET_BASE);
    }
}
