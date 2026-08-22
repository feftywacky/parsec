pub mod actions;
pub mod fetch;
pub mod queue;
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
        atomic::{AtomicU64, Ordering},
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
    http: HttpClient,
    registry: SharedRegistry,
    mainnet: bool,
    keystore_path: String,
    passphrase: zeroize::Zeroizing<String>,
) {
    runtime.spawn(async move {
        let outcome = tokio::task::spawn_blocking(move || {
            unlock_keystore(mainnet, &keystore_path, &passphrase)
        })
        .await;
        match outcome {
            Ok(Ok(session)) => {
                let session = Arc::new(session);
                *session_slot.write().unwrap() = Some(session.clone());
                events.push(PcEvent::error(0, "agent keystore unlocked", 0));
                // The user socket has nothing to subscribe to before the master
                // address is known, so it only starts here — never at engine
                // creation, unlike the always-on market socket.
                tokio::spawn(crate::transport::user_ws::run(
                    events, http, registry, session, mainnet,
                ));
            }
            Ok(Err(message)) => events.push(PcEvent::error(-10, &message, 0)),
            Err(_) => events.push(PcEvent::error(-10, "keystore unlock task panicked", 0)),
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
    1
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
        if config.abi_version != 1 {
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
        // Absent keystore path: leave `session_slot` `None` and keep running — market
        // data must work exactly as it does today with no keystore configured. A
        // present-but-unopenable keystore degrades the same way rather than failing
        // engine creation outright, so a typo'd path or wrong passphrase doesn't take
        // market data down with it; the failure reason arrives as a `PC_EV_ERROR`.
        if let Some(path) = keystore_path {
            spawn_keystore_unlock(
                &runtime,
                events.clone(),
                session_slot.clone(),
                http.clone(),
                registry.clone(),
                mainnet,
                path,
                passphrase,
            );
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
                    Command::Subscribe { id: 0, coin, mask, interval: 0, book }
                } else {
                    Command::Unsubscribe { id: 0, coin, mask, interval: 0, book }
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
