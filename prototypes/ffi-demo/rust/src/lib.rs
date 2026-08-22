use std::os::raw::{c_char, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

/// POD event pushed across the boundary. repr(C), no String/Vec.
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct HlBookLevel {
    pub px: f64,
    pub sz: f64,
    pub n: u32,
    pub _pad: u32,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct HlBookUpdate {
    pub coin: [c_char; 16],   // fixed-size, NUL-padded: no heap across FFI
    pub time_ms: u64,
    pub n_bids: u32,
    pub n_asks: u32,
    pub levels: [HlBookLevel; 40], // bids then asks
}

/// C++ callback. Invoked from a tokio worker thread -> must be reentrancy-safe.
pub type HlBookCb = extern "C" fn(user: *mut c_void, ev: *const HlBookUpdate);

pub struct HlClient {
    rt: tokio::runtime::Runtime,
    seq: Arc<AtomicU64>,
}

#[no_mangle]
pub extern "C" fn hl_client_new() -> *mut HlClient {
    let r = catch_unwind(|| {
        let rt = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .ok()?;
        Some(Box::into_raw(Box::new(HlClient { rt, seq: Arc::new(AtomicU64::new(0)) })))
    });
    match r {
        Ok(Some(p)) => p,
        _ => std::ptr::null_mut(),
    }
}

#[no_mangle]
pub extern "C" fn hl_client_free(p: *mut HlClient) {
    if p.is_null() { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| unsafe { drop(Box::from_raw(p)) }));
}

/// Spawns a task that invokes `cb` from a tokio worker thread `count` times.
#[no_mangle]
pub extern "C" fn hl_subscribe_book(
    client: *mut HlClient,
    cb: HlBookCb,
    user: *mut c_void,
    count: u32,
) -> i32 {
    if client.is_null() { return -1; }
    let c = unsafe { &*client };
    let seq = c.seq.clone();
    // raw pointer is not Send; wrap in a newtype we assert is safe to move
    struct SendPtr(*mut c_void);
    unsafe impl Send for SendPtr {}
    let up = SendPtr(user);

    c.rt.spawn(async move {
        let up = up;
        for _ in 0..count {
            let mut ev = HlBookUpdate {
                coin: [0; 16],
                time_ms: seq.fetch_add(1, Ordering::Relaxed),
                n_bids: 1,
                n_asks: 1,
                levels: [HlBookLevel { px: 0.0, sz: 0.0, n: 0, _pad: 0 }; 40],
            };
            for (i, b) in b"ETH".iter().enumerate() { ev.coin[i] = *b as c_char; }
            ev.levels[0] = HlBookLevel { px: 3000.5, sz: 1.25, n: 3, _pad: 0 };
            ev.levels[1] = HlBookLevel { px: 3000.7, sz: 2.50, n: 5, _pad: 0 };
            // never let a Rust panic unwind into C++
            let _ = catch_unwind(AssertUnwindSafe(|| cb(up.0, &ev as *const _)));
            tokio::time::sleep(std::time::Duration::from_millis(5)).await;
        }
    });
    0
}
