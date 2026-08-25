pub mod actions;
pub mod fetch;
pub mod queue;
pub mod setup;
pub mod types;
use crate::session::{AgentAddress, MasterAddress, Network, Session};
use crate::signer::keystore::Keystore;
use crate::transport::http::HttpClient;
use crate::transport::ws::{self, BookAggregation, Command};
use crate::universe::SharedRegistry;
use queue::EventQueue;
use std::{
    ffi::{c_char, CStr},
    panic::{catch_unwind, AssertUnwindSafe},
    ptr,
    sync::{
        atomic::{AtomicU64, AtomicU8, Ordering},
        Arc, Mutex, RwLock,
    },
};
use tokio::sync::mpsc;
use types::*;
pub(crate) fn monotonic_ns() -> u64 {
    use std::sync::OnceLock;
    static START: OnceLock<std::time::Instant> = OnceLock::new();
    START
        .get_or_init(std::time::Instant::now)
        .elapsed()
        .as_nanos() as u64
}
pub struct PcEngine {
    runtime: tokio::runtime::Runtime,
    commands: mpsc::UnboundedSender<Command>,
    events: EventQueue,
    next_request: AtomicU64,
    last_error: Mutex<String>,
    /// Pooled REST client for `pc_fetch`'s one-shot `/info` pulls (docs/02 §4.1).
    http: HttpClient,
    /// Asset registry resolved from `meta`, backing `pc_asset_count`/`pc_asset_info`.
    /// Empty until the first successful `PC_FETCH_META` fetch.
    registry: SharedRegistry,
    /// `None` until an agent keystore is unlocked. Unlocking is Argon2id-SENSITIVE
    /// (~3.5 s, docs/06 §3) so it never runs on the calling thread — `pc_engine_create`
    /// kicks it off on a `spawn_blocking` task and this slot is filled in once that
    /// task finishes, which is why every authenticated call reads through a lock
    /// rather than holding an owned `Session` directly.
    session: Arc<RwLock<Option<Arc<Session>>>>,
    /// Coarse unlock state for the UI, mirroring `session` but observable *during* the
    /// ~3.5 s Argon2id derivation, which `session` cannot express — it is `None` both
    /// while unlocking and after a failure, and a passphrase dialog has to tell those
    /// two apart to know whether to show a spinner or an error.
    auth_status: Arc<AtomicU8>,
    /// The network this engine resolved its transport to. Re-checked against the
    /// keystore's own `network` field on every unlock (`unlock_keystore`), which is
    /// what stops a testnet keystore being used to sign against a live book.
    mainnet: bool,
    /// Keystore file to open when `pc_unlock` is called. Set at construction from
    /// `pc_config::keystore_path`; `None` means no keystore is configured, so there is
    /// nothing an unlock could act on.
    keystore_path: Mutex<Option<String>>,
    /// Cleartext `valid_until_ms` of the keystore at `keystore_path`, or 0 if unknown.
    /// Cached at construction (and on `pc_set_keystore_path`) so `pc_auth_status` — which
    /// a UI polls every frame — can answer "expired" without a file read per frame. The
    /// field is cleartext and AAD-bound in the keystore, so reading it needs no passphrase
    /// and it cannot be edited without making the file undecryptable (docs/06 §3).
    keystore_valid_until: AtomicU64,
}
fn c_string(ptr: *const c_char) -> Option<String> {
    if ptr.is_null() {
        None
    } else {
        unsafe { CStr::from_ptr(ptr).to_str().ok().map(str::to_owned) }
    }
}
/// Decode a fixed-size, NUL-padded `char[N]` FFI field into an owned `String`,
/// stopping at the first NUL (or the end of the buffer if there is none) rather than
/// trusting the whole array to be valid UTF-8 — unlike `c_string`, there is no
/// pointer to bounds-check here, just a fixed array that may not be fully populated.
fn c_array_string(buf: &[c_char]) -> Option<String> {
    let end = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
    if end == 0 {
        return None;
    }
    let bytes: Vec<u8> = buf[..end].iter().map(|&c| c as u8).collect();
    String::from_utf8(bytes).ok()
}
/// Current session for this call, or `None` if no keystore has finished unlocking yet
/// (absent config, still unlocking, or unlock failed — `pc_last_error`/the unlock's
/// own `PC_EV_ERROR` event carries the reason for the latter two).
fn session(engine: &PcEngine) -> Option<Arc<Session>> {
    engine.session.read().ok().and_then(|g| g.clone())
}
/// Push the standard "no session" rejection for an authenticated command that was
/// called before a keystore finished unlocking (or with none configured at all).
fn reject_unauthenticated(engine: &PcEngine, id: u64) {
    engine.events.push(PcEvent::error(
        -2,
        "authenticated exchange actions are unavailable until an agent keystore is unlocked",
        id,
    ));
}
/// Kick off keystore unlock on a blocking-pool task so `pc_engine_create` itself stays
/// non-blocking (docs/02 §5.1 rule 5) despite Argon2id-SENSITIVE taking ~3.5 s
/// (docs/06 §3). Reads the file, decrypts the agent key, checks expiry and the
/// stored network against `mainnet`, and — on success — installs a fresh `Session`
/// into `engine.session`. Every failure path pushes exactly one `PC_EV_ERROR` (req_id
/// 0, matching the convention `transport/ws.rs` already uses for connection-level
/// events that aren't a response to any one command) and leaves the session `None`,
/// which is the same state as "no keystore configured" — market data keeps working
/// either way.
#[allow(clippy::too_many_arguments)]
fn spawn_keystore_unlock(
    runtime: &tokio::runtime::Runtime,
    events: EventQueue,
    session_slot: Arc<RwLock<Option<Arc<Session>>>>,
    auth_status: Arc<AtomicU8>,
    http: HttpClient,
    registry: SharedRegistry,
    mainnet: bool,
    keystore_path: String,
    passphrase: zeroize::Zeroizing<String>,
) {
    auth_status.store(PC_AUTH_UNLOCKING, Ordering::Release);
    runtime.spawn(async move {
        let outcome = tokio::task::spawn_blocking(move || {
            unlock_keystore(mainnet, &keystore_path, &passphrase)
        })
        .await;
        match outcome {
            Ok(Ok(session)) => {
                let session = Arc::new(session);
                *session_slot.write().unwrap() = Some(session.clone());
                // Status is published *after* the session slot, so a UI that sees
                // PC_AUTH_UNLOCKED can rely on an authenticated call succeeding rather
                // than racing the slot it just learned about.
                auth_status.store(PC_AUTH_UNLOCKED, Ordering::Release);
                events.push(PcEvent::error(0, "agent keystore unlocked", 0));
                // The user socket has nothing to subscribe to before the master
                // address is known, so it only starts here — never at engine
                // creation, unlike the always-on market socket.
                tokio::spawn(crate::transport::user_ws::run(
                    events, http, registry, session, mainnet,
                ));
            }
            Ok(Err(message)) => {
                auth_status.store(PC_AUTH_FAILED, Ordering::Release);
                events.push(PcEvent::error(-10, &message, 0))
            }
            Err(_) => {
                auth_status.store(PC_AUTH_FAILED, Ordering::Release);
                events.push(PcEvent::error(-10, "keystore unlock task panicked", 0))
            }
        }
    });
}
/// The actual (blocking, CPU-heavy) unlock: read the file, decrypt with `passphrase`,
/// validate expiry and network, and build a `Session`. Never logs `passphrase` or any
/// decrypted key material (docs/06 §5).
fn unlock_keystore(mainnet: bool, path: &str, passphrase: &str) -> Result<Session, String> {
    let text =
        std::fs::read_to_string(path).map_err(|e| format!("cannot read keystore {path}: {e}"))?;
    let keystore: Keystore =
        serde_json::from_str(&text).map_err(|_| "keystore file is not valid JSON".to_string())?;
    let expected_network = if mainnet { "mainnet" } else { "testnet" };
    if keystore.network != expected_network {
        return Err(format!(
            "keystore is for network '{}' but engine was configured for '{expected_network}'",
            keystore.network
        ));
    }
    let now = actions::now_ms();
    if now >= keystore.valid_until_ms {
        return Err(format!(
            "agent key expired at {} (now {now}); re-run `parsec setup` to rotate it",
            keystore.valid_until_ms
        ));
    }
    let secret = keystore
        .open(passphrase)
        .map_err(|_| "wrong passphrase or corrupted keystore".to_string())?;
    let master = crate::signer::parse_address(&keystore.master_address)
        .map_err(|_| "keystore master_address is not a valid address".to_string())?;
    let agent = crate::signer::parse_address(&keystore.agent_address)
        .map_err(|_| "keystore agent_address is not a valid address".to_string())?;
    let network = if mainnet {
        Network::Mainnet
    } else {
        Network::Testnet
    };
    // Default starting buffer per docs/03 §7; corrected by `PC_FETCH_USER_RATE_LIMIT`
    // (`ffi::fetch::fetch_user_rate_limit`) the first time it is fetched.
    const DEFAULT_RATE_BUDGET: i64 = 10_000;
    Session::new(
        network,
        mainnet,
        MasterAddress(master),
        AgentAddress(agent),
        secret,
        now,
        DEFAULT_RATE_BUDGET,
    )
    .map_err(|e| e.to_string())
}
fn request(engine: &PcEngine) -> u64 {
    engine.next_request.fetch_add(1, Ordering::Relaxed)
}
fn command(engine: &PcEngine, value: Command) -> u64 {
    let id = request(engine);
    if engine.commands.send(value.with_id(id)).is_err() {
        engine
            .events
            .push(PcEvent::error(-1, "network task is not running", id));
    }
    id
}
#[no_mangle]
pub extern "C" fn pc_abi_version() -> u32 {
    3
}
/// # Safety
/// `cfg` must be null or point to a valid, readable/writable `PcConfig` for the
/// duration of this call (`docs/04 §5.5`); this function also zeroes its passphrase
/// field before returning.
#[no_mangle]
pub unsafe extern "C" fn pc_engine_create(cfg: *mut PcConfig) -> *mut PcEngine {
    catch_unwind(AssertUnwindSafe(|| {
        if cfg.is_null() {
            return ptr::null_mut();
        }
        let config = unsafe { &mut *cfg };
        if config.abi_version != 3 {
            return ptr::null_mut();
        }
        // rustls ships with NO default process-level `CryptoProvider` even when
        // exactly one backend feature is compiled in — every WS/TLS connection
        // panics at `rustls-0.23`'s `crypto::mod.rs:249` until one is installed.
        // Installed here, once, before any task (and therefore any TLS connection)
        // spawns. `ring` is the backend pinned in Cargo.toml specifically to avoid
        // pulling in `aws-lc-rs`'s cc/cmake build and to keep the macOS link surface
        // at `-lSystem -lc -lm -liconv` (docs/04 §1.2 — no Security/
        // SystemConfiguration/CoreFoundation frameworks).
        static TLS_PROVIDER: std::sync::Once = std::sync::Once::new();
        TLS_PROVIDER.call_once(|| {
            let _ = rustls::crypto::ring::default_provider().install_default();
        });
        // Capture the keystore path (not sensitive) and passphrase (very much
        // sensitive) before the passphrase field is zeroed a few lines down — this is
        // the only chance to read either one, since the zeroing below is the whole
        // point of the ABI contract documented on this function.
        let keystore_path = c_array_string(&config.keystore_path);
        let passphrase =
            zeroize::Zeroizing::new(c_array_string(&config.passphrase).unwrap_or_default());
        for c in config.passphrase.iter_mut() {
            *c = 0
        }
        let workers = config.io_worker_threads.clamp(1, 4) as usize;
        let runtime = match tokio::runtime::Builder::new_multi_thread()
            .worker_threads(workers)
            .enable_all()
            .build()
        {
            Ok(v) => v,
            Err(_) => return ptr::null_mut(),
        };
        let events = EventQueue::new(config.event_queue_capacity as usize);
        let (queue_tx, queue_rx) = mpsc::unbounded_channel();
        let mainnet = config.mainnet;
        let http = match HttpClient::new(mainnet) {
            Ok(v) => v,
            Err(_) => return ptr::null_mut(),
        };
        let registry = SharedRegistry::empty();
        runtime.spawn(ws::run(queue_rx, events.clone(), mainnet, registry.clone()));
        // Pre-warm the connection pool / TLS session off the critical path of the
        // first real `/info` call (docs/02 §4.1).
        runtime.spawn({
            let http = http.clone();
            async move { http.prewarm().await }
        });
        let session_slot: Arc<RwLock<Option<Arc<Session>>>> = Arc::new(RwLock::new(None));
        // Remembered so `pc_unlock` knows which file to open without the caller having
        // to hand the path back in alongside the passphrase.
        let mut pending_unlock: Option<String> = None;
        // Absent keystore path: leave `session_slot` `None` and keep running — market
        // data must work exactly as it does today with no keystore configured. A
        // present-but-unopenable keystore degrades the same way rather than failing
        // engine creation outright, so a typo'd path or wrong passphrase doesn't take
        // market data down with it; the failure reason arrives as a `PC_EV_ERROR`.
        let auth_status = Arc::new(AtomicU8::new(PC_AUTH_LOCKED));
        // A keystore path with an empty passphrase is the normal startup shape now that
        // unlocking is interactive (`pc_unlock`): leave the engine locked rather than
        // burning 3.5 s of Argon2id on a passphrase that is definitionally wrong. A
        // passphrase supplied up front (headless/test use) still unlocks eagerly.
        let mut valid_until_cache = 0u64;
        if let Some(path) = keystore_path {
            valid_until_cache = keystore_valid_until(&path).unwrap_or(0);
            // Remembered on both branches: an eager unlock that fails must still be
            // retryable through `pc_unlock`, and `pc_auth_addresses` / `pc_reset_accounts`
            // read this same field to find the file on disk.
            pending_unlock = Some(path.clone());
            if !passphrase.is_empty() {
                spawn_keystore_unlock(
                    &runtime,
                    events.clone(),
                    session_slot.clone(),
                    auth_status.clone(),
                    http.clone(),
                    registry.clone(),
                    mainnet,
                    path,
                    passphrase,
                );
            }
        }
        Box::into_raw(Box::new(PcEngine {
            runtime,
            commands: queue_tx,
            events,
            next_request: AtomicU64::new(1),
            last_error: Mutex::new(String::new()),
            http,
            registry,
            session: session_slot,
            auth_status,
            mainnet,
            keystore_path: Mutex::new(pending_unlock),
            keystore_valid_until: AtomicU64::new(valid_until_cache),
        }))
    }))
    .unwrap_or(ptr::null_mut())
}
/// # Safety
/// `engine` must be null or a pointer previously returned by `pc_engine_create` that
/// has not already been passed to `pc_engine_destroy`.
#[no_mangle]
pub unsafe extern "C" fn pc_engine_destroy(engine: *mut PcEngine) {
    if engine.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| unsafe { drop(Box::from_raw(engine)) }));
}
/// # Safety
/// `engine` must be null or a live `pc_engine_create` handle; `out` must be null or
/// point to at least `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn pc_last_error(engine: *mut PcEngine, out: *mut c_char, cap: u32) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() || out.is_null() || cap == 0 {
            return -1;
        }
        let message = unsafe { &*engine }.last_error.lock().unwrap().clone();
        let bytes = message.as_bytes();
        let length = bytes.len().min(cap as usize - 1);
        unsafe {
            ptr::copy_nonoverlapping(bytes.as_ptr() as *const c_char, out, length);
            *out.add(length) = 0
        };
        length as i32
    }))
    .unwrap_or(-1)
}
/// # Safety
/// `engine` must be null or a live `pc_engine_create` handle; `out` must be null or
/// point to at least `max` writable `PcEvent` slots.
#[no_mangle]
pub unsafe extern "C" fn pc_poll(
    engine: *mut PcEngine,
    out: *mut PcEvent,
    max: u32,
    timeout_ns: u64,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() || out.is_null() || max == 0 {
            return -1;
        }
        let engine = unsafe { &*engine };
        let first = match engine.events.pop() {
            Some(event) => Some(event),
            None => {
                if timeout_ns > 0 {
                    engine
                        .events
                        .wait(std::time::Duration::from_nanos(timeout_ns))
                }
                engine.events.pop()
            }
        };
        let mut n = 0usize;
        if let Some(event) = first {
            unsafe { *out = event };
            n = 1;
        }
        while n < max as usize {
            match engine.events.pop() {
                Some(event) => unsafe {
                    *out.add(n) = event;
                    n += 1
                },
                None => break,
            }
        }
        n as i32
    }))
    .unwrap_or(-1)
}
/// # Safety
/// `engine` must be null or a live `pc_engine_create` handle; `coin` must be null or a
/// valid NUL-terminated UTF-8 C string.
#[no_mangle]
pub unsafe extern "C" fn pc_subscribe(
    engine: *mut PcEngine,
    coin: *const c_char,
    mask: u32,
    interval: u8,
) -> u64 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() {
            return 0;
        }
        match c_string(coin) {
            Some(coin) => command(
                unsafe { &*engine },
                Command::Subscribe {
                    id: 0,
                    coin,
                    mask,
                    interval,
                    book: BookAggregation::default(),
                },
            ),
            None => 0,
        }
    }))
    .unwrap_or(0)
}
/// # Safety
/// `engine` must be null or a live `pc_engine_create` handle; `coin` must be null or a
/// valid NUL-terminated UTF-8 C string.
#[no_mangle]
pub unsafe extern "C" fn pc_unsubscribe(
    engine: *mut PcEngine,
    coin: *const c_char,
    mask: u32,
    interval: u8,
) -> u64 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() {
            return 0;
        }
        match c_string(coin) {
            Some(coin) => command(
                unsafe { &*engine },
                Command::Unsubscribe {
                    id: 0,
                    coin,
                    mask,
                    interval,
                    book: BookAggregation::default(),
                },
            ),
            None => 0,
        }
    }))
    .unwrap_or(0)
}
/// Book-granularity-aware subscribe/unsubscribe. `n_sig_figs` < 0 and `mantissa` == 0 mean
/// "send null", i.e. the venue's native price granularity; otherwise they are forwarded to the
/// `l2Book` subscriptions as-is so the venue aggregates the book server-side and still returns
/// its full depth at the coarser step. Separate entry points rather than extra parameters on
/// `pc_subscribe` so the existing ABI (and its layout test) is untouched.
///
/// # Safety
/// `engine` must be null or a live `pc_engine_create` handle; `coin` must be null or a
/// valid NUL-terminated UTF-8 C string.
#[no_mangle]
pub unsafe extern "C" fn pc_subscribe_book(
    engine: *mut PcEngine,
    coin: *const c_char,
    mask: u32,
    n_sig_figs: i8,
    mantissa: u8,
) -> u64 {
    book_command(engine, coin, mask, n_sig_figs, mantissa, true)
}
/// # Safety
/// Same contract as `pc_subscribe_book`.
#[no_mangle]
pub unsafe extern "C" fn pc_unsubscribe_book(
    engine: *mut PcEngine,
    coin: *const c_char,
    mask: u32,
    n_sig_figs: i8,
    mantissa: u8,
) -> u64 {
    book_command(engine, coin, mask, n_sig_figs, mantissa, false)
}

fn book_command(
    engine: *mut PcEngine,
    coin: *const c_char,
    mask: u32,
    n_sig_figs: i8,
    mantissa: u8,
    subscribe: bool,
) -> u64 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() {
            return 0;
        }
        let book = BookAggregation {
            n_sig_figs: if n_sig_figs < 0 {
                None
            } else {
                Some(n_sig_figs as u8)
            },
            mantissa: if mantissa == 0 { None } else { Some(mantissa) },
        };
        match c_string(coin) {
            Some(coin) => command(
                unsafe { &*engine },
                if subscribe {
                    Command::Subscribe {
                        id: 0,
                        coin,
                        mask,
                        interval: 0,
                        book,
                    }
                } else {
                    Command::Unsubscribe {
                        id: 0,
                        coin,
                        mask,
                        interval: 0,
                        book,
                    }
                },
            ),
            None => 0,
        }
    }))
    .unwrap_or(0)
}
/// # Safety
/// `engine` must be null or a live `pc_engine_create` handle; `req` must be null or
/// point to a valid, readable `pc_order_req` for the duration of this call.
#[no_mangle]
pub unsafe extern "C" fn pc_place_order(engine: *mut PcEngine, req: *const PcOrderReq) -> u64 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() || req.is_null() {
            return 0;
        }
        let engine = unsafe { &*engine };
        let id = request(engine);
        match session(engine) {
            None => reject_unauthenticated(engine, id),
            Some(session) => {
                let req = unsafe { *req }; // pc_order_req is `Copy`; this dereference
                                           // copies it off the caller's memory before
                                           // the async task (which may outlive this
                                           // call) ever touches it.
                engine.runtime.spawn(actions::place_order(
                    session,
                    engine.http.clone(),
                    engine.events.clone(),
                    id,
                    req,
                ));
            }
        }
        id
    }))
    .unwrap_or(0)
}

/// # Safety
/// `engine` must be null or a live `pc_engine_create` handle.
#[no_mangle]
pub unsafe extern "C" fn pc_cancel_order(engine: *mut PcEngine, asset: u32, oid: u64) -> u64 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() {
            return 0;
        }
        let engine = unsafe { &*engine };
        let id = request(engine);
        match session(engine) {
            None => reject_unauthenticated(engine, id),
            Some(session) => {
                engine.runtime.spawn(actions::cancel_order(
                    session,
                    engine.http.clone(),
                    engine.events.clone(),
                    id,
                    asset,
                    oid,
                ));
            }
        }
        id
    }))
    .unwrap_or(0)
}

/// # Safety
/// `engine` must be null or a live `pc_engine_create` handle; `cloid` must be null or
/// point to 16 valid, readable bytes.
#[no_mangle]
pub unsafe extern "C" fn pc_cancel_by_cloid(
    engine: *mut PcEngine,
    asset: u32,
    cloid: *const u8,
) -> u64 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() || cloid.is_null() {
            return 0;
        }
        let engine = unsafe { &*engine };
        let id = request(engine);
        match session(engine) {
            None => reject_unauthenticated(engine, id),
            Some(session) => {
                let mut bytes = [0u8; 16];
                unsafe { ptr::copy_nonoverlapping(cloid, bytes.as_mut_ptr(), 16) };
                engine.runtime.spawn(actions::cancel_by_cloid(
                    session,
                    engine.http.clone(),
                    engine.events.clone(),
                    id,
                    asset,
                    bytes,
                ));
            }
        }
        id
    }))
    .unwrap_or(0)
}

/// # Safety
/// `engine` must be null or a live `pc_engine_create` handle; `order` must be null or
/// point to a valid, readable `pc_order` for the duration of this call.
#[no_mangle]
pub unsafe extern "C" fn pc_modify_order(
    engine: *mut PcEngine,
    oid: u64,
    order: *const PcOrder,
) -> u64 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() || order.is_null() {
            return 0;
        }
        let engine = unsafe { &*engine };
        let id = request(engine);
        match session(engine) {
            None => reject_unauthenticated(engine, id),
            Some(session) => {
                let order = unsafe { *order }; // `Copy`; see pc_place_order's note.
                engine.runtime.spawn(actions::modify_order(
                    session,
                    engine.http.clone(),
                    engine.events.clone(),
                    id,
                    oid,
                    order,
                ));
            }
        }
        id
    }))
    .unwrap_or(0)
}

/// # Safety
/// `engine` must be null or a live `pc_engine_create` handle.
#[no_mangle]
pub unsafe extern "C" fn pc_cancel_all(engine: *mut PcEngine) -> u64 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() {
            return 0;
        }
        let engine = unsafe { &*engine };
        let id = request(engine);
        match session(engine) {
            None => reject_unauthenticated(engine, id),
            Some(session) => {
                engine.runtime.spawn(actions::cancel_all(
                    session,
                    engine.http.clone(),
                    engine.events.clone(),
                    engine.registry.clone(),
                    id,
                ));
            }
        }
        id
    }))
    .unwrap_or(0)
}

/// # Safety
/// `engine` must be null or a live `pc_engine_create` handle.
#[no_mangle]
pub unsafe extern "C" fn pc_set_leverage(
    engine: *mut PcEngine,
    asset: u32,
    is_cross: bool,
    leverage: u32,
) -> u64 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() {
            return 0;
        }
        let engine = unsafe { &*engine };
        let id = request(engine);
        match session(engine) {
            None => reject_unauthenticated(engine, id),
            Some(session) => {
                engine.runtime.spawn(actions::set_leverage(
                    session,
                    engine.http.clone(),
                    engine.events.clone(),
                    id,
                    asset,
                    is_cross,
                    leverage,
                ));
            }
        }
        id
    }))
    .unwrap_or(0)
}

/// # Safety
/// `engine` must be null or a live `pc_engine_create` handle.
#[no_mangle]
pub unsafe extern "C" fn pc_set_iso_margin(engine: *mut PcEngine, asset: u32, delta: i64) -> u64 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() {
            return 0;
        }
        let engine = unsafe { &*engine };
        let id = request(engine);
        match session(engine) {
            None => reject_unauthenticated(engine, id),
            Some(session) => {
                engine.runtime.spawn(actions::set_iso_margin(
                    session,
                    engine.http.clone(),
                    engine.events.clone(),
                    id,
                    asset,
                    delta,
                ));
            }
        }
        id
    }))
    .unwrap_or(0)
}

/// # Safety
/// `engine` must be null or a live `pc_engine_create` handle. `deadline` of `0` clears
/// a previously scheduled cancel rather than scheduling one (see `actions::schedule_cancel`).
#[no_mangle]
pub unsafe extern "C" fn pc_schedule_cancel(engine: *mut PcEngine, deadline: u64) -> u64 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() {
            return 0;
        }
        let engine = unsafe { &*engine };
        let id = request(engine);
        match session(engine) {
            None => reject_unauthenticated(engine, id),
            Some(session) => {
                engine.runtime.spawn(actions::schedule_cancel(
                    session,
                    engine.http.clone(),
                    engine.events.clone(),
                    id,
                    deadline,
                ));
            }
        }
        id
    }))
    .unwrap_or(0)
}

fn spawn_fetch(engine: &PcEngine, what: u32, coin_ptr: *const c_char, interval: u8) -> u64 {
    let id = request(engine);
    let coin = c_string(coin_ptr); // None is valid for fetches such as PC_FETCH_META.
    engine.runtime.spawn(fetch::dispatch(
        what,
        coin,
        interval,
        id,
        engine.http.clone(),
        engine.events.clone(),
        engine.registry.clone(),
        session(engine),
    ));
    id
}

/// # Safety
/// `engine` must be null or a live `pc_engine_create` handle; `coin` must be null or a
/// valid NUL-terminated UTF-8 C string.
/// Dispatch a one-shot `/info` pull (`PC_FETCH_*`); the result arrives as ordinary
/// events carrying this call's `req_id`. Market-data fetches (`PC_FETCH_META`,
/// `PC_FETCH_CANDLE_SNAPSHOT`) need no keystore — `/info` reads require no signature —
/// but the account-scoped fetches need the session's master address and fail with a
/// clear error if no keystore is unlocked yet (`ffi::fetch::dispatch`). `coin` may be
/// null (e.g. `PC_FETCH_META`, which fetches the whole universe rather than one asset).
#[no_mangle]
pub unsafe extern "C" fn pc_fetch(engine: *mut PcEngine, what: u32, coin: *const c_char) -> u64 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() {
            return 0;
        }
        spawn_fetch(unsafe { &*engine }, what, coin, PC_IV_1M)
    }))
    .unwrap_or(0)
}

/// # Safety
/// `engine` must be null or a live `pc_engine_create` handle; `coin` must be null or a
/// valid NUL-terminated UTF-8 C string. `interval` must be one of the supported `PC_IV_*`
/// values.
#[no_mangle]
pub unsafe extern "C" fn pc_fetch_candle_snapshot(
    engine: *mut PcEngine,
    coin: *const c_char,
    interval: u8,
) -> u64 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() || coin.is_null() || interval >= PC_IV_COUNT {
            return 0;
        }
        spawn_fetch(
            unsafe { &*engine },
            PC_FETCH_CANDLE_SNAPSHOT,
            coin,
            interval,
        )
    }))
    .unwrap_or(0)
}

/// # Safety
/// `engine` must be null or a live `pc_engine_create` handle.
#[no_mangle]
pub unsafe extern "C" fn pc_asset_count(engine: *mut PcEngine) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() {
            return -1;
        }
        unsafe { &*engine }.registry.len() as i32
    }))
    .unwrap_or(-1)
}

/// # Safety
/// `engine` must be null or a live `pc_engine_create` handle; every non-null output
/// pointer (`name_out` with capacity `cap`, `sz_decimals`, `max_leverage`,
/// `only_isolated`) must be valid and writable for this call.
#[no_mangle]
pub unsafe extern "C" fn pc_asset_info(
    engine: *mut PcEngine,
    asset: u32,
    name_out: *mut c_char,
    cap: u32,
    sz_decimals: *mut u8,
    max_leverage: *mut u32,
    only_isolated: *mut u8,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() {
            return -1;
        }
        let engine = unsafe { &*engine };
        let Some(info) = engine.registry.info(asset) else {
            return -1;
        };
        if !name_out.is_null() && cap > 0 {
            let bytes = info.name.as_bytes();
            let length = bytes.len().min(cap as usize - 1);
            unsafe {
                ptr::copy_nonoverlapping(bytes.as_ptr() as *const c_char, name_out, length);
                *name_out.add(length) = 0;
            }
        }
        if !sz_decimals.is_null() {
            unsafe { *sz_decimals = info.sz_decimals };
        }
        if !max_leverage.is_null() {
            unsafe { *max_leverage = info.max_leverage };
        }
        if !only_isolated.is_null() {
            unsafe { *only_isolated = info.only_isolated as u8 };
        }
        0
    }))
    .unwrap_or(-1)
}

// ---------------------------------------------------------------------------------
// interactive unlock (docs/06 §2, §5.7)
// ---------------------------------------------------------------------------------

/// `pc_auth_status` values. Kept as plain constants rather than an enum so the C header
/// and this file cannot drift into disagreeing about the numeric values.
pub const PC_AUTH_NO_KEYSTORE: u8 = 0;
pub const PC_AUTH_LOCKED: u8 = 1;
pub const PC_AUTH_UNLOCKING: u8 = 2;
pub const PC_AUTH_UNLOCKED: u8 = 3;
pub const PC_AUTH_FAILED: u8 = 4;
/// The keystore exists and is readable but its agent approval has lapsed. Distinct from
/// `PC_AUTH_FAILED` because no passphrase can fix it: the venue stops honouring the agent
/// key at `valid_until_ms`, so the only way forward is to approve a fresh agent. Reported
/// from the cleartext header, before any passphrase is asked for — prompting for one and
/// then reporting "wrong passphrase" would be a lie the user cannot act on.
pub const PC_AUTH_EXPIRED: u8 = 5;


/// Current authentication state. `PC_AUTH_NO_KEYSTORE` means no keystore is configured
/// at all — a fresh install that has not run `parsec setup` — which a UI should surface
/// as "connect an account", not as a locked keystore waiting for a passphrase.
///
/// # Safety
/// `engine` must be null or a live `pc_engine_create` handle.
#[no_mangle]
pub unsafe extern "C" fn pc_auth_status(engine: *mut PcEngine) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() {
            return -1;
        }
        let engine = unsafe { &*engine };
        let status = engine.auth_status.load(Ordering::Acquire);
        if status == PC_AUTH_LOCKED && engine.keystore_path.lock().unwrap().is_none() {
            return PC_AUTH_NO_KEYSTORE as i32;
        }
        // Expiry outranks LOCKED and FAILED alike: both would put a passphrase prompt on
        // screen, and for a lapsed agent every passphrase — including the right one — is
        // refused by `unlock_keystore` before it ever reaches the KDF. Compared against
        // the clock on each call, not latched, so a keystore that lapses while the app
        // sits at the prompt flips over on its own.
        if status == PC_AUTH_LOCKED || status == PC_AUTH_FAILED {
            let valid_until = engine.keystore_valid_until.load(Ordering::Relaxed);
            if valid_until > 0 && actions::now_ms() >= valid_until {
                return PC_AUTH_EXPIRED as i32;
            }
        }
        status as i32
    }))
    .unwrap_or(-1)
}

/// Try to unlock the configured keystore with `passphrase`.
///
/// **Asynchronous and non-blocking.** Argon2id at the SENSITIVE tier costs ~3.5 s
/// (docs/06 §3), which is far too long to spend on a UI thread, so this returns
/// immediately after handing the work to a blocking-pool task. Poll `pc_auth_status`
/// for the outcome; the failure reason also arrives as a `PC_EV_ERROR` event.
///
/// `passphrase` is **zeroed before this function returns**, exactly like
/// `pc_engine_create`'s config field — the caller's buffer is wiped here rather than
/// depending on the caller to remember.
///
/// Returns 0 if an attempt was started, -1 otherwise (no keystore configured, or an
/// unlock is already in flight — a second concurrent Argon2id derivation would just
/// compete for memory with the first).
///
/// # Safety
/// `engine` must be null or a live handle; `passphrase` must be null or a valid
/// NUL-terminated, writable C string buffer.
#[no_mangle]
pub unsafe extern "C" fn pc_unlock(engine: *mut PcEngine, passphrase: *mut c_char) -> i32 {
    let result = catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() || passphrase.is_null() {
            return -1;
        }
        let engine = unsafe { &*engine };
        // Already unlocked, or an attempt is in flight: reject rather than stacking a
        // second derivation. Unlocking twice is not harmful but it is never what the
        // caller meant, and each attempt reserves 1 GiB.
        let status = engine.auth_status.load(Ordering::Acquire);
        if status == PC_AUTH_UNLOCKING || status == PC_AUTH_UNLOCKED {
            return -1;
        }
        let Some(path) = engine.keystore_path.lock().unwrap().clone() else {
            *engine.last_error.lock().unwrap() =
                "no keystore is configured; run `parsec setup` first".to_string();
            return -1;
        };
        // SAFETY: caller contract — null-checked, NUL-terminated.
        let pass = match unsafe { CStr::from_ptr(passphrase) }.to_str() {
            Ok(v) => zeroize::Zeroizing::new(v.to_string()),
            Err(_) => return -1,
        };
        spawn_keystore_unlock(
            &engine.runtime,
            engine.events.clone(),
            engine.session.clone(),
            engine.auth_status.clone(),
            engine.http.clone(),
            engine.registry.clone(),
            engine.mainnet,
            path,
            pass,
        );
        0
    }))
    .unwrap_or(-1);
    // Unconditional, including on every early return and on panic. Volatile so it is not
    // elided as a dead store (docs/06 §4).
    if !passphrase.is_null() {
        // SAFETY: caller contract — a NUL-terminated writable buffer.
        unsafe {
            let mut p = passphrase;
            while ptr::read_volatile(p) != 0 {
                ptr::write_volatile(p, 0);
                p = p.add(1);
            }
        }
    }
    result
}

/// The unlocked account's addresses and agent expiry, for the header badge and the
/// 14-day expiry warning (docs/06 §1). Returns -1 until a keystore is unlocked.
///
/// Both addresses are returned because they are not interchangeable and the UI shows
/// them differently: `master` is the account that holds the funds and is what every
/// `/info` query uses, `agent` is the fundless key that signs. Confusing the two
/// silently returns an empty account rather than erroring (docs/06 §1), so the header
/// labels them explicitly.
///
/// # Safety
/// `engine` must be null or a live handle; each `out` pointer must be null or point to
/// its stated capacity in writable bytes; `valid_until_ms` must be null or writable.
#[no_mangle]
pub unsafe extern "C" fn pc_auth_addresses(
    engine: *mut PcEngine,
    master_out: *mut c_char,
    master_cap: u32,
    agent_out: *mut c_char,
    agent_cap: u32,
    valid_until_ms: *mut u64,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() {
            return -1;
        }
        let engine = unsafe { &*engine };
        let Some(session) = session(engine) else {
            return -1;
        };
        let master = crate::signer::format_address(&session.master_address().0);
        let agent = crate::signer::format_address(&session.agent_address().0);
        if !master_out.is_null() && setup::write_c_string(&master, master_out, master_cap) < 0 {
            return -1;
        }
        if !agent_out.is_null() && setup::write_c_string(&agent, agent_out, agent_cap) < 0 {
            return -1;
        }
        if !valid_until_ms.is_null() {
            // Read back from the keystore header rather than cached on the session:
            // `Session` deliberately holds no expiry, since it is a keystore fact, not
            // a signing one.
            let expiry = engine
                .keystore_path
                .lock()
                .unwrap()
                .as_deref()
                .and_then(keystore_valid_until)
                .unwrap_or(0);
            unsafe { *valid_until_ms = expiry };
        }
        0
    }))
    .unwrap_or(-1)
}

/// Read a keystore's `valid_until_ms` without decrypting it — the header is cleartext
/// (and AAD-bound, so it cannot be tampered with undetected).
fn keystore_valid_until(path: &str) -> Option<u64> {
    let text = std::fs::read_to_string(path).ok()?;
    let keystore: Keystore = serde_json::from_str(&text).ok()?;
    Some(keystore.valid_until_ms)
}

/// Read the cleartext header of a keystore file *without* a passphrase, so an unlock
/// prompt can name the account it is about to unlock and warn about an agent nearing
/// expiry before the user has typed anything.
///
/// Safe to expose: every field here is already cleartext in the file and is bound as
/// AEAD associated data (docs/06 §3), so it cannot be altered without making the
/// keystore undecryptable. No key material is involved.
///
/// # Safety
/// `path` must be a valid NUL-terminated C string; each `out` pointer must be null or
/// point to its stated capacity in writable bytes; the scalar out-params must be null
/// or writable.
#[no_mangle]
pub unsafe extern "C" fn pc_keystore_peek(
    path: *const c_char,
    master_out: *mut c_char,
    master_cap: u32,
    agent_out: *mut c_char,
    agent_cap: u32,
    valid_until_ms: *mut u64,
    is_mainnet: *mut bool,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        let Some(path) = c_string(path) else {
            return -1;
        };
        let Ok(text) = std::fs::read_to_string(&path) else {
            return -1;
        };
        let Ok(keystore) = serde_json::from_str::<Keystore>(&text) else {
            return -1;
        };
        if !master_out.is_null()
            && setup::write_c_string(&keystore.master_address, master_out, master_cap) < 0
        {
            return -1;
        }
        if !agent_out.is_null()
            && setup::write_c_string(&keystore.agent_address, agent_out, agent_cap) < 0
        {
            return -1;
        }
        if !valid_until_ms.is_null() {
            unsafe { *valid_until_ms = keystore.valid_until_ms };
        }
        if !is_mainnet.is_null() {
            unsafe { *is_mainnet = keystore.network == "mainnet" };
        }
        0
    }))
    .unwrap_or(-1)
}

#[cfg(test)]
mod auth_tests {
    use super::*;
    use std::ffi::CString;

    /// Build a `PcConfig` pointing at `path` with no passphrase — the shape
    /// `Engine::start` actually produces now that unlocking is interactive.
    fn config(path: &str, mainnet: bool) -> PcConfig {
        let mut cfg = PcConfig {
            abi_version: 3,
            mainnet,
            keystore_path: [0; 512],
            passphrase: [0; 256],
            event_queue_capacity: 256,
            io_worker_threads: 1,
        };
        for (slot, byte) in cfg.keystore_path.iter_mut().zip(path.as_bytes()) {
            *slot = *byte as c_char;
        }
        cfg
    }

    fn poll_until_settled(engine: *mut PcEngine) -> i32 {
        for _ in 0..600 {
            let status = unsafe { pc_auth_status(engine) };
            if status != PC_AUTH_UNLOCKING as i32 {
                return status;
            }
            std::thread::sleep(std::time::Duration::from_millis(50));
        }
        panic!("unlock never settled");
    }

    /// The exact sequence the unlock dialog drives: create locked, submit a wrong
    /// passphrase, observe FAILED, retry with the right one, observe UNLOCKED — and
    /// confirm the passphrase buffer is zeroed by `pc_unlock` itself both times.
    ///
    /// Ignored for the same reason as `setup::tests::keystore_round_trips_*`: each
    /// attempt is a full SENSITIVE-tier Argon2id derivation.
    ///
    ///     cargo test --release -- --ignored
    #[test]
    #[ignore = "two SENSITIVE-tier Argon2id derivations; run with --release -- --ignored"]
    fn unlock_reports_failure_then_success_and_zeroes_the_passphrase() {
        let dir = std::env::temp_dir().join(format!("parsec-auth-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("keystore-testnet.json");

        // Seal a keystore for a known agent, the way `parsec setup` would.
        let agent = k256::ecdsa::SigningKey::from_slice(&[21u8; 32]).unwrap();
        let agent_address = crate::signer::address_from_verifying_key(agent.verifying_key());
        let mut key = [0u8; 32];
        key.copy_from_slice(&agent.to_bytes());
        let keystore = Keystore::seal(
            "testnet".into(),
            "0x000000000000000000000000000000000000dead".into(),
            crate::signer::format_address(&agent_address),
            "parsec-test".into(),
            actions::now_ms() + 86_400_000,
            actions::now_ms(),
            key,
            "right",
        )
        .unwrap();
        std::fs::write(&path, serde_json::to_string(&keystore).unwrap()).unwrap();

        let mut cfg = config(path.to_str().unwrap(), false);
        let engine = unsafe { pc_engine_create(&mut cfg) };
        assert!(!engine.is_null());

        // A keystore exists, so the engine starts LOCKED — not NO_KEYSTORE, and not
        // UNLOCKED. This is what makes the dialog appear at all.
        assert_eq!(unsafe { pc_auth_status(engine) }, PC_AUTH_LOCKED as i32);

        // Wrong passphrase: FAILED, and the caller's buffer comes back zeroed.
        let mut wrong: Vec<c_char> = CString::new("wrong")
            .unwrap()
            .into_bytes_with_nul()
            .into_iter()
            .map(|b| b as c_char)
            .collect();
        assert_eq!(unsafe { pc_unlock(engine, wrong.as_mut_ptr()) }, 0);
        assert!(
            wrong.iter().all(|&c| c == 0),
            "passphrase left in caller buffer"
        );
        assert_eq!(poll_until_settled(engine), PC_AUTH_FAILED as i32);

        // A failed attempt must leave the engine retryable, not wedged — otherwise a
        // single typo would cost a restart.
        let mut right: Vec<c_char> = CString::new("right")
            .unwrap()
            .into_bytes_with_nul()
            .into_iter()
            .map(|b| b as c_char)
            .collect();
        assert_eq!(unsafe { pc_unlock(engine, right.as_mut_ptr()) }, 0);
        assert!(right.iter().all(|&c| c == 0));
        assert_eq!(poll_until_settled(engine), PC_AUTH_UNLOCKED as i32);

        // Once unlocked, the addresses the header badge shows are available, and the
        // agent address is the one that was sealed.
        let mut master_buf = [0 as c_char; 64];
        let mut agent_buf = [0 as c_char; 64];
        let mut valid_until = 0u64;
        assert_eq!(
            unsafe {
                pc_auth_addresses(
                    engine,
                    master_buf.as_mut_ptr(),
                    64,
                    agent_buf.as_mut_ptr(),
                    64,
                    &mut valid_until,
                )
            },
            0
        );
        assert_eq!(
            unsafe { CStr::from_ptr(agent_buf.as_ptr()) }
                .to_str()
                .unwrap(),
            crate::signer::format_address(&agent_address)
        );
        assert!(valid_until > 0);

        // A second unlock on an already-unlocked engine is refused rather than starting
        // another 1 GiB derivation.
        let mut again: Vec<c_char> = CString::new("right")
            .unwrap()
            .into_bytes_with_nul()
            .into_iter()
            .map(|b| b as c_char)
            .collect();
        assert_eq!(unsafe { pc_unlock(engine, again.as_mut_ptr()) }, -1);

        unsafe { pc_engine_destroy(engine) };
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// No keystore file means "connect an account", which is a different UI state from
    /// "locked" — a fresh install must not be shown a passphrase prompt it cannot satisfy.
    #[test]
    fn absent_keystore_reports_no_keystore_and_cannot_be_unlocked() {
        let mut cfg = config("", false);
        let engine = unsafe { pc_engine_create(&mut cfg) };
        assert!(!engine.is_null());
        assert_eq!(
            unsafe { pc_auth_status(engine) },
            PC_AUTH_NO_KEYSTORE as i32
        );

        let mut pass: Vec<c_char> = CString::new("anything")
            .unwrap()
            .into_bytes_with_nul()
            .into_iter()
            .map(|b| b as c_char)
            .collect();
        assert_eq!(unsafe { pc_unlock(engine, pass.as_mut_ptr()) }, -1);
        assert!(
            pass.iter().all(|&c| c == 0),
            "passphrase must be zeroed even on refusal"
        );
        unsafe { pc_engine_destroy(engine) };
    }

    /// A keystore whose header says it lapsed, written by hand. Nothing here needs to be
    /// decryptable: expiry is decided from the cleartext header, which is exactly what lets
    /// these tests skip the ~3.5 s Argon2id that forces the round-trip tests to be
    /// `#[ignore]`d.
    fn write_keystore(path: &std::path::Path, network: &str, valid_until_ms: u64) {
        let json = serde_json::json!({
            "version": 1,
            "network": network,
            "master_address": "0x000000000000000000000000000000000000dead",
            "agent_address": "0x000000000000000000000000000000000000beef",
            "agent_name": "parsec-test",
            "valid_until_ms": valid_until_ms,
            "created_ms": 0,
            "kdf": { "name": "argon2id", "m_cost_kib": 1, "t_cost": 1, "p_cost": 1,
                     "salt": "AAAAAAAAAAAAAAAAAAAAAA==" },
            "cipher": { "name": "xchacha20poly1305", "nonce": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" },
            "ciphertext": "AAAA",
        });
        std::fs::write(path, serde_json::to_string(&json).unwrap()).unwrap();
    }

    fn temp_dir(tag: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "parsec-{tag}-{}-{}",
            std::process::id(),
            actions::now_ms()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    /// The regression this whole path exists for. A lapsed keystore used to report LOCKED,
    /// which put a passphrase prompt on screen; the unlock then failed inside
    /// `unlock_keystore`'s expiry check and surfaced as PC_AUTH_FAILED, which the dialog
    /// renders as "wrong passphrase". The user was told to fix something that was not broken
    /// and given no way to fix what was.
    #[test]
    fn lapsed_keystore_reports_expired_not_locked() {
        let dir = temp_dir("expired");
        let path = dir.join("keystore-testnet.json");
        write_keystore(&path, "testnet", actions::now_ms() - 1);

        let mut cfg = config(path.to_str().unwrap(), false);
        let engine = unsafe { pc_engine_create(&mut cfg) };
        assert!(!engine.is_null());
        assert_eq!(unsafe { pc_auth_status(engine) }, PC_AUTH_EXPIRED as i32);

        unsafe { pc_engine_destroy(engine) };
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// The complementary half: a keystore with time left must still be LOCKED, or the expiry
    /// check would lock every user out of a perfectly good keystore.
    #[test]
    fn unexpired_keystore_still_reports_locked() {
        let dir = temp_dir("unexpired");
        let path = dir.join("keystore-testnet.json");
        write_keystore(&path, "testnet", actions::now_ms() + 86_400_000);

        let mut cfg = config(path.to_str().unwrap(), false);
        let engine = unsafe { pc_engine_create(&mut cfg) };
        assert!(!engine.is_null());
        assert_eq!(unsafe { pc_auth_status(engine) }, PC_AUTH_LOCKED as i32);

        unsafe { pc_engine_destroy(engine) };
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// Reset deletes *both* networks' keystores and leaves the engine reporting
    /// NO_KEYSTORE — the state that makes the UI open its connect flow. Leaving the other
    /// network's file behind would mean a "reset" that still prompts for a passphrase the
    /// next time `mainnet` is flipped.
    #[test]
    fn reset_deletes_every_keystore_and_reopens_onboarding() {
        let dir = temp_dir("reset");
        let testnet = dir.join("keystore-testnet.json");
        let mainnet = dir.join("keystore-mainnet.json");
        write_keystore(&testnet, "testnet", actions::now_ms() + 86_400_000);
        write_keystore(&mainnet, "mainnet", actions::now_ms() + 86_400_000);

        let mut cfg = config(testnet.to_str().unwrap(), false);
        let engine = unsafe { pc_engine_create(&mut cfg) };
        assert!(!engine.is_null());
        assert_eq!(unsafe { pc_auth_status(engine) }, PC_AUTH_LOCKED as i32);

        assert_eq!(unsafe { pc_reset_accounts(engine) }, 2);
        assert!(!testnet.exists());
        assert!(!mainnet.exists(), "the other network's keystore survived a reset");
        assert_eq!(
            unsafe { pc_auth_status(engine) },
            PC_AUTH_NO_KEYSTORE as i32
        );

        // And an unlock has nothing to act on, rather than reopening the deleted file.
        let mut pass: Vec<c_char> = CString::new("anything")
            .unwrap()
            .into_bytes_with_nul()
            .into_iter()
            .map(|b| b as c_char)
            .collect();
        assert_eq!(unsafe { pc_unlock(engine, pass.as_mut_ptr()) }, -1);

        // Idempotent: resetting again is a no-op, not an error. The UI can offer the button
        // whenever it likes without tracking whether it already ran.
        assert_eq!(unsafe { pc_reset_accounts(engine) }, 0);

        unsafe { pc_engine_destroy(engine) };
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// Reset is the escape hatch from an *expired* keystore, so it has to work in exactly
    /// that state — the one where the engine reports EXPIRED rather than LOCKED.
    #[test]
    fn expired_keystore_can_be_reset() {
        let dir = temp_dir("expired-reset");
        let path = dir.join("keystore-testnet.json");
        write_keystore(&path, "testnet", actions::now_ms() - 1);

        let mut cfg = config(path.to_str().unwrap(), false);
        let engine = unsafe { pc_engine_create(&mut cfg) };
        assert_eq!(unsafe { pc_auth_status(engine) }, PC_AUTH_EXPIRED as i32);
        assert_eq!(unsafe { pc_reset_accounts(engine) }, 1);
        assert_eq!(
            unsafe { pc_auth_status(engine) },
            PC_AUTH_NO_KEYSTORE as i32
        );

        unsafe { pc_engine_destroy(engine) };
        let _ = std::fs::remove_dir_all(&dir);
    }
}

/// Point the engine at a keystore that did not exist when it was created — the in-app
/// "connect account" flow writes one at runtime, and the engine would otherwise keep
/// reporting `PC_AUTH_NO_KEYSTORE` until a restart.
///
/// Refused once a session is unlocked: swapping the keystore under a live session would
/// leave `pc_auth_addresses` describing one account while `Session` signs for another.
///
/// # Safety
/// `engine` must be null or a live handle; `path` must be a valid NUL-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn pc_set_keystore_path(engine: *mut PcEngine, path: *const c_char) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() {
            return -1;
        }
        let engine = unsafe { &*engine };
        let status = engine.auth_status.load(Ordering::Acquire);
        if status == PC_AUTH_UNLOCKED || status == PC_AUTH_UNLOCKING {
            return -1;
        }
        let Some(path) = c_string(path) else {
            return -1;
        };
        if !std::path::Path::new(&path).exists() {
            return -1;
        }
        // Cached before the status flips, so a poll that sees LOCKED never reads a stale
        // expiry belonging to the keystore being replaced.
        engine
            .keystore_valid_until
            .store(keystore_valid_until(&path).unwrap_or(0), Ordering::Relaxed);
        *engine.keystore_path.lock().unwrap() = Some(path);
        // Back to LOCKED regardless of any earlier failure: a newly supplied keystore is a
        // fresh chance, and leaving PC_AUTH_FAILED would make the UI show a stale error.
        engine.auth_status.store(PC_AUTH_LOCKED, Ordering::Release);
        0
    }))
    .unwrap_or(-1)
}

/// Delete every local keystore and return the engine to its fresh-install state, so the
/// next thing the UI shows is "connect an account" rather than a prompt for a passphrase
/// that no longer opens anything.
///
/// Removes the configured keystore plus the `keystore-mainnet.json` /
/// `keystore-testnet.json` siblings in the same directory — "reset all accounts" means
/// both networks, not just the one this engine happens to be pointed at. Returns the
/// number of files removed, or -1 if the reset was refused or a file could not be deleted
/// (reason in `pc_last_error`).
///
/// **This is local only.** The agent approval it forgets still exists at the venue until it
/// lapses on its own; parsec cannot revoke it, because revoking needs the master key that
/// parsec deliberately never stores (docs/06 §1). Nothing is at risk either way — the
/// deleted key was fundless and could not withdraw — but the approval is not "undone".
///
/// Refused while unlocked or unlocking: a live `Session` and its user websocket are already
/// signing for the old account, and pulling the keystore out from under them would leave the
/// UI describing an account the engine is still trading. Reset before unlocking, or restart.
///
/// # Safety
/// `engine` must be null or a live `pc_engine_create` handle.
#[no_mangle]
pub unsafe extern "C" fn pc_reset_accounts(engine: *mut PcEngine) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() {
            return -1;
        }
        let engine = unsafe { &*engine };
        let status = engine.auth_status.load(Ordering::Acquire);
        if status == PC_AUTH_UNLOCKED || status == PC_AUTH_UNLOCKING {
            *engine.last_error.lock().unwrap() =
                "cannot reset accounts while a keystore is unlocked; restart parsec first"
                    .to_string();
            return -1;
        }

        let configured = engine.keystore_path.lock().unwrap().clone();
        // Both networks' keystores live beside each other under ~/.parsec, and a reset that
        // left the other network's file behind would not be a reset — the user would be
        // asked for its passphrase the next time they flipped `mainnet`.
        let mut targets: Vec<std::path::PathBuf> = Vec::new();
        if let Some(path) = configured.as_deref() {
            let path = std::path::PathBuf::from(path);
            if let Some(dir) = path.parent().map(|d| d.to_path_buf()) {
                targets.push(dir.join("keystore-mainnet.json"));
                targets.push(dir.join("keystore-testnet.json"));
            }
            targets.push(path);
        }
        targets.sort();
        targets.dedup();

        let mut removed = 0;
        for target in &targets {
            match std::fs::remove_file(target) {
                Ok(()) => removed += 1,
                // Absent is the desired end state, not a failure: only one of the two
                // network keystores usually exists.
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                Err(e) => {
                    *engine.last_error.lock().unwrap() =
                        format!("could not delete {}: {e}", target.display());
                    return -1;
                }
            }
        }

        // Only now that the files are gone: forget the path and drop any failed-unlock
        // state, which together make `pc_auth_status` report `PC_AUTH_NO_KEYSTORE` and the
        // UI open its connect flow. The session slot is already `None` — the refusal above
        // guarantees nothing was unlocked.
        *engine.keystore_path.lock().unwrap() = None;
        engine.keystore_valid_until.store(0, Ordering::Relaxed);
        engine.auth_status.store(PC_AUTH_LOCKED, Ordering::Release);
        engine.events.push(PcEvent::error(
            0,
            "local keystores deleted; connect an account to trade again",
            0,
        ));
        removed
    }))
    .unwrap_or(-1)
}
