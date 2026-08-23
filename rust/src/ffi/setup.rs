//! Agent-wallet onboarding across the C ABI (docs/06 §2) — the `parsec setup` flow.
//!
//! **Engine-free by construction.** Onboarding runs before any `pc_engine` exists: there
//! is no market socket, no event queue, and no keystore to unlock yet. So this module
//! owns a small, self-contained `pc_setup` handle with its own single-threaded tokio
//! runtime for the one `POST /exchange` it makes, rather than bolting an onboarding mode
//! onto `PcEngine`.
//!
//! **The handle is what keeps key material off the ABI.** `pc_setup_begin` generates the
//! agent keypair and holds it *inside* the handle for the whole flow; the C caller only
//! ever sees the agent's public address. The private key leaves the handle exactly once,
//! into `Keystore::seal` (docs/06 §5 rule 1). There is deliberately no
//! `pc_setup_agent_private_key` and there must never be one.
//!
//! The master key's exposure window (docs/06 §2 steps 2-5) is one call —
//! `pc_setup_sign_with_master` parses it, signs, and zeroes it before returning, and the
//! caller's buffer is zeroed too. The master address is then *recovered from that
//! signature* rather than derived from the key, so nothing downstream depends on the key
//! still being around.
use crate::codec::exchange::ApproveAgentAction;
use crate::session::{SignatureWire, SignedActionBody};
use crate::signer::agent::{self, ApproveAgentRequest};
use crate::signer::keystore::Keystore;
use crate::signer::{format_address, recover_address};
use crate::transport::http::HttpClient;
use k256::ecdsa::SigningKey;
use rand_core::{OsRng, RngCore};
use std::ffi::{c_char, CStr};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::sync::Mutex;
use zeroize::Zeroizing;

/// Default agent lifetime. The venue's own maximum is 180 days (docs/06 §1) and a
/// longer `valid_until` is silently rejected, so this is both the default and the cap.
pub const MAX_AGENT_LIFETIME_MS: u64 = 180 * 24 * 60 * 60 * 1000;

/// Hard cap on the agent name, enforced by the venue: an over-long name is rejected with
/// `"Extra agent name must be between 1 and 16 characters long."` — observed against live
/// mainnet, not documented anywhere in the API reference.
///
/// The cap applies to the *base* name only; the ` valid_until <ms>` suffix that
/// `signed_agent_name` appends is parsed off by the venue and does not count against it.
pub const MAX_AGENT_NAME_LEN: usize = 16;

/// `signatureChainId` for the `approveAgent` EIP-712 domain. This only picks the chain
/// the *wallet* signs on — `hyperliquidChain` inside the message is what actually
/// separates mainnet from testnet (docs/03 §4b). Mainnet uses Arbitrum One (42161),
/// testnet Arbitrum Sepolia (421614), matching what MetaMask will be connected to in
/// each case so the signing prompt names a chain the user recognizes.
const SIGNATURE_CHAIN_ID_MAINNET: u64 = 0xa4b1;
const SIGNATURE_CHAIN_ID_TESTNET: u64 = 0x66eee;

/// Onboarding state machine. One instance per `parsec setup` run.
///
/// The `signature`/`master_address` pair is filled by exactly one of the two approval
/// paths and is what `submit` and `write_keystore` gate on — neither can run against a
/// half-finished flow, which is the property that stops a keystore being written for an
/// agent the venue never approved.
pub struct PcSetup {
    runtime: tokio::runtime::Runtime,
    mainnet: bool,
    /// The agent private key. Never leaves this struct except into `Keystore::seal`.
    agent_key: SigningKey,
    agent_address: [u8; 20],
    /// Base name only (`parsec-<8 hex>`, at most `MAX_AGENT_NAME_LEN`). The
    /// ` valid_until <ms>` suffix the venue
    /// actually sees is reconstructed by `signed_agent_name` from `valid_until_ms`, so
    /// the two can never drift apart — and the keystore stores the base name, which is
    /// what a later rotation must re-approve to deregister this agent (docs/06 §7).
    agent_name: String,
    valid_until_ms: u64,
    nonce: u64,
    signature: Option<[u8; 65]>,
    master_address: Option<[u8; 20]>,
    submitted: bool,
    last_error: Mutex<String>,
}

impl PcSetup {
    fn signed_agent_name(&self) -> String {
        format!("{} valid_until {}", self.agent_name, self.valid_until_ms)
    }

    fn signature_chain_id(&self) -> u64 {
        if self.mainnet {
            SIGNATURE_CHAIN_ID_MAINNET
        } else {
            SIGNATURE_CHAIN_ID_TESTNET
        }
    }

    fn hyperliquid_chain(&self) -> &'static str {
        if self.mainnet {
            "Mainnet"
        } else {
            "Testnet"
        }
    }

    fn request(&self) -> ApproveAgentRequest {
        ApproveAgentRequest {
            hyperliquid_chain: self.hyperliquid_chain(),
            signature_chain_id: self.signature_chain_id(),
            agent_address: self.agent_address,
            agent_name: Some(self.signed_agent_name()),
            nonce: self.nonce,
        }
    }

    fn fail(&self, message: impl Into<String>) -> i32 {
        *self.last_error.lock().unwrap() = message.into();
        -1
    }
}

/// `parsec-<8 hex>`: named (never unnamed) so parsec cannot silently deregister another
/// tool's unnamed agent, and randomly suffixed so two machines trading the same account get
/// distinct agents rather than repeatedly evicting each other (docs/06 §1).
fn default_agent_name() -> String {
    let mut id = [0u8; 4];
    OsRng.fill_bytes(&mut id);
    // "parsec-" + 8 hex = 15 chars, inside the venue's 16-char cap with a byte to spare.
    //
    // The host name used to be folded in here (docs/06 §1 wanted two machines trading the
    // same account to get distinct agents rather than evicting each other). It does not fit
    // in 16 characters alongside a useful random suffix, and it was not what provided the
    // distinctness anyway — the random id does. Dropped rather than truncated to three
    // letters, which would have been unreadable *and* still collision-prone.
    format!("parsec-{}", hex::encode(id))
}

/// Copy `value` into a NUL-terminated C buffer. Returns the length written, or -1 if the
/// buffer is too small — never truncates an address or a name silently, since a truncated
/// address that still looks like an address is exactly the kind of value a caller would
/// go on to use.
pub(crate) fn write_c_string(value: &str, out: *mut c_char, cap: u32) -> i32 {
    if out.is_null() || cap == 0 {
        return -1;
    }
    let bytes = value.as_bytes();
    if bytes.len() + 1 > cap as usize {
        return -1;
    }
    // SAFETY: `out` is non-null with at least `cap` writable bytes (caller contract) and
    // the length check above proves `bytes.len() + 1 <= cap`.
    unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr() as *const c_char, out, bytes.len());
        *out.add(bytes.len()) = 0;
    }
    bytes.len() as i32
}

fn handle<'a>(setup: *mut PcSetup) -> Option<&'a PcSetup> {
    if setup.is_null() {
        None
    } else {
        // SAFETY: caller contract — a live `pc_setup_begin` handle, not yet freed.
        Some(unsafe { &*setup })
    }
}

/// Begin onboarding: generate a fresh agent keypair and fix the approval's parameters.
///
/// A **fresh** keypair every time, with no import path (docs/06 §1): after an agent is
/// deregistered the venue may prune its nonce state, which would make previously signed
/// actions replayable if the address were ever reused.
///
/// `agent_name` may be null for the default `parsec-<8 hex>`, and is truncated to
/// [`MAX_AGENT_NAME_LEN`]. `lifetime_ms` may be 0
/// for the 180-day default and is clamped to [`MAX_AGENT_LIFETIME_MS`], which the venue
/// rejects beyond.
///
/// # Safety
/// `agent_name` must be null or a valid NUL-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn pc_setup_begin(
    mainnet: bool,
    agent_name: *const c_char,
    lifetime_ms: u64,
) -> *mut PcSetup {
    catch_unwind(AssertUnwindSafe(|| {
        let name = if agent_name.is_null() {
            default_agent_name()
        } else {
            // SAFETY: caller contract — null-checked above, NUL-terminated.
            match unsafe { CStr::from_ptr(agent_name) }.to_str() {
                Ok(s) if !s.is_empty() => s.to_string(),
                _ => default_agent_name(),
            }
        };
        // Truncated here rather than left for the venue to reject after the user has already
        // typed their private key and waited for a network round trip. Truncation (not an
        // error) because the name is cosmetic — it identifies the agent in the account's
        // agent list and has no bearing on what the agent can do.
        let name: String = name.chars().take(MAX_AGENT_NAME_LEN).collect();
        let runtime = match tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
        {
            Ok(v) => v,
            Err(_) => return ptr::null_mut(),
        };
        // rustls has no default CryptoProvider even with exactly one backend compiled
        // in; without this every TLS connection panics. Same `Once` rationale as
        // `pc_engine_create` — setup may run in a process that never creates an engine.
        static TLS_PROVIDER: std::sync::Once = std::sync::Once::new();
        TLS_PROVIDER.call_once(|| {
            let _ = rustls::crypto::ring::default_provider().install_default();
        });
        let keypair = agent::generate();
        let now = crate::ffi::actions::now_ms();
        let lifetime = if lifetime_ms == 0 {
            MAX_AGENT_LIFETIME_MS
        } else {
            lifetime_ms.min(MAX_AGENT_LIFETIME_MS)
        };
        Box::into_raw(Box::new(PcSetup {
            runtime,
            mainnet,
            agent_key: keypair.signing_key,
            agent_address: keypair.address,
            agent_name: name,
            valid_until_ms: now.saturating_add(lifetime),
            // The approval's nonce and the envelope's nonce are the same value (docs/03
            // §`approveAgent`), so it is fixed once here rather than re-read at submit
            // time — a nonce that changed between signing and posting would invalidate
            // the signature.
            nonce: now,
            signature: None,
            master_address: None,
            submitted: false,
            last_error: Mutex::new(String::new()),
        }))
    }))
    .unwrap_or(ptr::null_mut())
}

/// # Safety
/// `setup` must be null or a live `pc_setup_begin` handle not already freed.
#[no_mangle]
pub unsafe extern "C" fn pc_setup_free(setup: *mut PcSetup) {
    if setup.is_null() {
        return;
    }
    // Dropping the handle drops the `SigningKey`, whose own `Drop` zeroizes the scalar
    // (k256 zeroizes on drop by default).
    let _ = catch_unwind(AssertUnwindSafe(|| unsafe { drop(Box::from_raw(setup)) }));
}

/// Last failure message for this handle. Never contains key material: every error path
/// below is built from fixed strings and non-secret values (docs/06 §5 rule 2).
///
/// # Safety
/// `setup` must be null or a live handle; `out` must be null or point to `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn pc_setup_last_error(
    setup: *mut PcSetup,
    out: *mut c_char,
    cap: u32,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        let Some(setup) = handle(setup) else {
            return -1;
        };
        let message = setup.last_error.lock().unwrap().clone();
        // Truncate rather than fail: an error message is for display, and a caller with
        // a small buffer wants the first line of the reason, not a second error.
        if out.is_null() || cap == 0 {
            return -1;
        }
        let bytes = message.as_bytes();
        let length = bytes.len().min(cap as usize - 1);
        // SAFETY: `length < cap` and `out` has `cap` writable bytes.
        unsafe {
            ptr::copy_nonoverlapping(bytes.as_ptr() as *const c_char, out, length);
            *out.add(length) = 0;
        }
        length as i32
    }))
    .unwrap_or(-1)
}

/// The generated agent's address, lowercase `0x...` (43 bytes with the NUL).
///
/// # Safety
/// `setup` must be null or a live handle; `out` must be null or point to `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn pc_setup_agent_address(
    setup: *mut PcSetup,
    out: *mut c_char,
    cap: u32,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| match handle(setup) {
        Some(setup) => write_c_string(&format_address(&setup.agent_address), out, cap),
        None => -1,
    }))
    .unwrap_or(-1)
}

/// The `agentName` exactly as it is signed and posted, including the
/// ` valid_until <ms>` suffix — this is the string the user will see in MetaMask.
///
/// # Safety
/// `setup` must be null or a live handle; `out` must be null or point to `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn pc_setup_agent_name(
    setup: *mut PcSetup,
    out: *mut c_char,
    cap: u32,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| match handle(setup) {
        Some(setup) => write_c_string(&setup.signed_agent_name(), out, cap),
        None => -1,
    }))
    .unwrap_or(-1)
}

/// The master address, known only after one of the two approval paths has run (it is
/// *recovered from the signature*, never supplied by the caller). Returns -1 before then.
///
/// # Safety
/// `setup` must be null or a live handle; `out` must be null or point to `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn pc_setup_master_address(
    setup: *mut PcSetup,
    out: *mut c_char,
    cap: u32,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| match handle(setup) {
        Some(setup) => match setup.master_address {
            Some(address) => write_c_string(&format_address(&address), out, cap),
            None => -1,
        },
        None => -1,
    }))
    .unwrap_or(-1)
}

/// Expiry of the agent being approved, in epoch ms. 0 for a null handle.
///
/// # Safety
/// `setup` must be null or a live handle.
#[no_mangle]
pub unsafe extern "C" fn pc_setup_valid_until_ms(setup: *mut PcSetup) -> u64 {
    catch_unwind(AssertUnwindSafe(|| {
        handle(setup).map(|s| s.valid_until_ms).unwrap_or(0)
    }))
    .unwrap_or(0)
}

/// Sign the approval with the master key, in-process (docs/06 §2 steps 2-5).
///
/// `master_sk_hex` is a mutable buffer holding the key as hex, with or without `0x`. It
/// is **zeroed before this function returns**, on every path including every error path,
/// so the caller's copy dies here rather than living until the caller remembers to wipe
/// it. The parsed scalar is held in `Zeroizing` storage and the `SigningKey` built from
/// it is dropped (and zeroized) before returning — the master key's whole lifetime is
/// this one call.
///
/// The resulting master address is *recovered from the signature*, not derived from the
/// key, so it is validated the same way in both approval paths.
///
/// # Safety
/// `setup` must be null or a live handle; `master_sk_hex` must be null or a valid
/// NUL-terminated, writable C string buffer.
#[no_mangle]
pub unsafe extern "C" fn pc_setup_sign_with_master(
    setup: *mut PcSetup,
    master_sk_hex: *mut c_char,
) -> i32 {
    let result = catch_unwind(AssertUnwindSafe(|| {
        let Some(this) = handle(setup) else {
            return -1;
        };
        if master_sk_hex.is_null() {
            return this.fail("no master private key supplied");
        }
        // SAFETY: caller contract — null-checked, NUL-terminated.
        let text = match unsafe { CStr::from_ptr(master_sk_hex) }.to_str() {
            Ok(v) => Zeroizing::new(v.trim().to_string()),
            Err(_) => return this.fail("master private key is not valid UTF-8"),
        };
        let hex_part = text.strip_prefix("0x").unwrap_or(&text);
        if hex_part.len() != 64 {
            return this.fail("master private key must be 64 hex characters (32 bytes)");
        }
        let mut raw = Zeroizing::new([0u8; 32]);
        if hex::decode_to_slice(hex_part, &mut *raw).is_err() {
            return this.fail("master private key is not valid hex");
        }
        let Ok(master) = SigningKey::from_slice(&*raw) else {
            return this.fail("master private key is not a valid secp256k1 scalar");
        };
        let request = this.request();
        let signature = request.sign(&master);
        drop(master); // zeroized by k256's Drop; `raw`/`text` by Zeroizing at scope end.
        let Ok(address) = recover_address(&request.digest(), &signature) else {
            return this.fail("signature did not recover to a valid address");
        };
        // SAFETY: `setup` is a live handle and `&this` is dropped before this reborrow is
        // used. A `pc_setup` handle belongs to one `parsec setup` run on one thread, so
        // there is no concurrent access to alias with.
        let this = unsafe { &mut *setup };
        this.signature = Some(signature);
        this.master_address = Some(address);
        0
    }))
    .unwrap_or(-1);
    // Zero the caller's buffer unconditionally — including on panic, a bad-hex early
    // return, or a null handle. Volatile so it cannot be elided as a dead store
    // (docs/06 §4 measured clang deleting exactly this kind of memset).
    if !master_sk_hex.is_null() {
        // SAFETY: caller contract — a NUL-terminated writable buffer.
        unsafe {
            let mut p = master_sk_hex;
            while ptr::read_volatile(p) != 0 {
                ptr::write_volatile(p, 0);
                p = p.add(1);
            }
        }
    }
    result
}

/// POST the signed `approveAgent` action to `/exchange`. **Blocks** until the venue
/// answers — this is a CLI step, not a UI one, and there is nothing useful to do
/// concurrently.
///
/// Returns 0 only on `{"status":"ok"}`. Anything else is left as a failure with the
/// venue's own message in `pc_setup_last_error`, so a rejected approval can never be
/// mistaken for a successful one and followed by a keystore write.
///
/// # Safety
/// `setup` must be null or a live handle.
#[no_mangle]
pub unsafe extern "C" fn pc_setup_submit(setup: *mut PcSetup) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        let Some(this) = handle(setup) else {
            return -1;
        };
        let Some(signature) = this.signature else {
            return this.fail("approval is not signed yet");
        };
        let action = ApproveAgentAction {
            typ: "approveAgent",
            hyperliquid_chain: this.hyperliquid_chain(),
            signature_chain_id: format!("0x{:x}", this.signature_chain_id()),
            agent_address: format_address(&this.agent_address),
            agent_name: Some(this.signed_agent_name()),
            nonce: this.nonce,
        };
        // The same envelope as every L1 action (docs/03 §`approveAgent`): the action's
        // own nonce is repeated at the top level, and `vaultAddress` is present-and-null
        // exactly as the Python SDK posts it.
        let body = SignedActionBody {
            action,
            nonce: this.nonce,
            signature: SignatureWire {
                r: format!("0x{}", hex::encode(&signature[0..32])),
                s: format!("0x{}", hex::encode(&signature[32..64])),
                v: signature[64],
            },
            vault_address: None,
            expires_after: None,
        };
        let json = match serde_json::to_value(&body) {
            Ok(v) => v,
            Err(_) => return this.fail("could not serialize the approveAgent action"),
        };
        let http = match HttpClient::new(this.mainnet) {
            Ok(v) => v,
            Err(e) => return this.fail(format!("could not build an HTTP client: {e}")),
        };
        let response = this.runtime.block_on(http.post_exchange(&json));
        let response = match response {
            Ok(v) => v,
            Err(e) => return this.fail(format!("approveAgent request failed: {e}")),
        };
        match response.get("status").and_then(|v| v.as_str()) {
            Some("ok") => {
                // SAFETY: see `pc_setup_sign_with_master`.
                unsafe { &mut *setup }.submitted = true;
                0
            }
            _ => {
                // The venue's rejection reason lives in `response`, which for an error
                // is a short string like "User or API Wallet does not exist." — not
                // account data, and worth showing verbatim so the user can act on it.
                let detail = response
                    .get("response")
                    .and_then(|v| v.as_str())
                    .map(str::to_string)
                    .unwrap_or_else(|| response.to_string());
                this.fail(format!("venue rejected approveAgent: {detail}"))
            }
        }
    }))
    .unwrap_or(-1)
}

/// Seal the agent key into an encrypted keystore and write it to `path` with mode 0600
/// (parent directory 0700), per docs/06 §3.
///
/// Refuses unless `pc_setup_submit` has succeeded: a keystore for an agent the venue
/// never approved is a file that looks like a working account and silently is not.
///
/// Argon2id at the SENSITIVE tier takes ~3.5 s and **blocks** — call it off any UI thread.
///
/// # Safety
/// `setup` must be null or a live handle; `passphrase` and `path` must be valid
/// NUL-terminated C strings.
#[no_mangle]
pub unsafe extern "C" fn pc_setup_write_keystore(
    setup: *mut PcSetup,
    passphrase: *const c_char,
    path: *const c_char,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        let Some(setup) = handle(setup) else {
            return -1;
        };
        if !setup.submitted {
            return setup.fail("agent has not been approved yet — nothing safe to store");
        }
        let Some(master_address) = setup.master_address else {
            return setup.fail("master address is unknown — approval did not complete");
        };
        if passphrase.is_null() || path.is_null() {
            return setup.fail("passphrase and path are required");
        }
        // SAFETY: caller contract — both null-checked, both NUL-terminated.
        let pass = match unsafe { CStr::from_ptr(passphrase) }.to_str() {
            Ok(v) => Zeroizing::new(v.to_string()),
            Err(_) => return setup.fail("passphrase is not valid UTF-8"),
        };
        // SAFETY: as above.
        let path = match unsafe { CStr::from_ptr(path) }.to_str() {
            Ok(v) => v.to_string(),
            Err(_) => return setup.fail("keystore path is not valid UTF-8"),
        };
        if pass.is_empty() {
            return setup.fail("passphrase must not be empty");
        }
        // Checked before the ~3.5s Argon2id derivation, not after: `write_keystore_file`
        // refuses to clobber an existing keystore anyway, and making the user wait 3.5s to
        // be told that is a needlessly bad way to deliver the news. The `create_new` in
        // `write_keystore_file` is still the real guard — this is only the fast path.
        if std::path::Path::new(&path).exists() {
            return setup.fail(format!(
                "{path} already exists; move it aside first (rotating an agent is a \
                 deliberate act — see docs/06 §7)"
            ));
        }
        let mut key = Zeroizing::new([0u8; 32]);
        key.copy_from_slice(&setup.agent_key.to_bytes());
        let keystore = match Keystore::seal(
            if setup.mainnet { "mainnet" } else { "testnet" }.to_string(),
            format_address(&master_address),
            format_address(&setup.agent_address),
            setup.agent_name.clone(),
            setup.valid_until_ms,
            crate::ffi::actions::now_ms(),
            *key,
            &pass,
        ) {
            Ok(v) => v,
            Err(e) => return setup.fail(format!("could not seal the keystore: {e}")),
        };
        let text = match serde_json::to_string_pretty(&keystore) {
            Ok(v) => v,
            Err(_) => return setup.fail("could not serialize the keystore"),
        };
        match write_keystore_file(&path, &text) {
            Ok(()) => 0,
            Err(e) => setup.fail(format!("could not write {path}: {e}")),
        }
    }))
    .unwrap_or(-1)
}

/// Write the keystore with restrictive permissions established *before* any content is
/// written (docs/06 §5 rule 4). Order matters: creating the file 0600 up front, rather
/// than writing it and chmod'ing after, means there is no window in which the ciphertext
/// exists at the process umask's permissions.
fn write_keystore_file(path: &str, text: &str) -> std::io::Result<()> {
    use std::io::Write;
    use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
    let path = std::path::Path::new(path);
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() && !parent.exists() {
            std::fs::DirBuilder::new()
                .recursive(true)
                .mode(0o700)
                .create(parent)?;
        }
    }
    // `create_new` refuses to clobber an existing keystore: overwriting one destroys the
    // only copy of an agent key that may still be approved at the venue. Rotation is a
    // deliberate act (docs/06 §7), so it moves the old file aside first.
    let mut file = std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(path)?;
    file.write_all(text.as_bytes())?;
    file.sync_all()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;

    fn begin(mainnet: bool) -> *mut PcSetup {
        unsafe { pc_setup_begin(mainnet, ptr::null(), 0) }
    }

    #[test]
    fn agent_address_is_fresh_per_handle_and_never_reused() {
        let mut seen = std::collections::HashSet::new();
        for _ in 0..8 {
            let s = begin(false);
            let mut buf = [0i8; 64];
            assert_eq!(
                unsafe { pc_setup_agent_address(s, buf.as_mut_ptr(), 64) },
                42
            );
            let address = unsafe { CStr::from_ptr(buf.as_ptr()) }
                .to_str()
                .unwrap()
                .to_string();
            assert!(address.starts_with("0x"));
            // docs/06 §1: rotation must always generate a fresh keypair, never reuse.
            assert!(seen.insert(address), "agent address was reused");
            unsafe { pc_setup_free(s) };
        }
    }

    /// The master key buffer is wiped on the way out on *every* path, so a caller that
    /// forgets to zero its own buffer is still safe (docs/06 §2 step 5).
    #[test]
    fn master_key_buffer_is_zeroed_even_on_failure() {
        let s = begin(false);
        // Success path.
        let mut good: Vec<i8> = CString::new(hex::encode([5u8; 32]))
            .unwrap()
            .into_bytes_with_nul()
            .into_iter()
            .map(|b| b as i8)
            .collect();
        assert_eq!(
            unsafe { pc_setup_sign_with_master(s, good.as_mut_ptr()) },
            0
        );
        assert!(
            good.iter().all(|&c| c == 0),
            "master key left in caller buffer"
        );

        // Failure path: malformed hex must wipe the buffer too.
        let mut bad: Vec<i8> = CString::new("not-a-key")
            .unwrap()
            .into_bytes_with_nul()
            .into_iter()
            .map(|b| b as i8)
            .collect();
        assert_eq!(
            unsafe { pc_setup_sign_with_master(s, bad.as_mut_ptr()) },
            -1
        );
        assert!(bad.iter().all(|&c| c == 0), "buffer left intact on failure");
        unsafe { pc_setup_free(s) };
    }

    /// A keystore must never be written for an agent the venue has not approved.
    #[test]
    fn keystore_write_refuses_before_submit() {
        let s = begin(false);
        let pass = CString::new("passphrase").unwrap();
        let path = CString::new("/tmp/parsec-test-should-not-exist.json").unwrap();
        assert_eq!(
            unsafe { pc_setup_write_keystore(s, pass.as_ptr(), path.as_ptr()) },
            -1
        );
        assert!(!std::path::Path::new("/tmp/parsec-test-should-not-exist.json").exists());
        unsafe { pc_setup_free(s) };
    }

    /// The chain every session depends on: seal the agent key, write it with the right
    /// permissions, read the header back without a passphrase, and unlock it into a real
    /// `Session` whose agent address matches the one that was approved.
    ///
    /// Only the venue round trip is bypassed (`submitted` is set directly) — everything
    /// from `Keystore::seal` onwards is the production path, including the 0600 mode and
    /// the refusal to clobber an existing file.
    ///
    /// Slow by design: `Keystore::seal`/`open` hardcode the SENSITIVE Argon2id tier
    /// (docs/06 §3), so this pays two ~3.5s derivations in release and considerably more in
    /// a debug build — too slow for the default loop, which runs on every edit. Everything
    /// it asserts is security-critical, so run it before touching anything in
    /// `signer::keystore` or this module:
    ///
    ///     cargo test --release -- --ignored
    #[test]
    #[ignore = "two SENSITIVE-tier Argon2id derivations; run with --release -- --ignored"]
    fn keystore_round_trips_from_seal_through_unlock() {
        use std::os::unix::fs::PermissionsExt;

        let s = begin(false);
        let master_bytes = [13u8; 32];
        let master = SigningKey::from_slice(&master_bytes).unwrap();
        let mut key_hex: Vec<c_char> = CString::new(hex::encode(master_bytes))
            .unwrap()
            .into_bytes_with_nul()
            .into_iter()
            .map(|b| b as c_char)
            .collect();
        assert_eq!(
            unsafe { pc_setup_sign_with_master(s, key_hex.as_mut_ptr()) },
            0
        );
        // Stand in for a successful `pc_setup_submit`; the venue call is the one part of
        // this chain a unit test cannot make.
        unsafe { &mut *s }.submitted = true;

        let dir = std::env::temp_dir().join(format!("parsec-ks-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let path = dir.join("keystore-testnet.json");
        let c_path = CString::new(path.to_str().unwrap()).unwrap();
        // A weak passphrase is fine; the KDF cost is what makes this slow, and `seal`
        // deliberately hardcodes the SENSITIVE tier, so this test pays ~3.5s once.
        let pass = CString::new("correct horse battery staple").unwrap();
        assert_eq!(
            unsafe { pc_setup_write_keystore(s, pass.as_ptr(), c_path.as_ptr()) },
            0
        );

        // docs/06 §5 rule 4: keystore 0600, parent directory 0700, established at
        // creation rather than chmod'd afterwards.
        let mode = std::fs::metadata(&path).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode, 0o600, "keystore must be 0600");
        let dir_mode = std::fs::metadata(&dir).unwrap().permissions().mode() & 0o777;
        assert_eq!(dir_mode, 0o700, "~/.parsec must be 0700");

        // Overwriting would destroy the only copy of a key the venue may still consider
        // approved, so a second write must refuse — and must refuse *fast*, before paying
        // for a derivation whose result is going to be thrown away.
        let refused_at = std::time::Instant::now();
        assert_eq!(
            unsafe { pc_setup_write_keystore(s, pass.as_ptr(), c_path.as_ptr()) },
            -1
        );
        assert!(
            refused_at.elapsed() < std::time::Duration::from_secs(1),
            "overwrite must be refused before the Argon2id derivation, not after"
        );

        // The cleartext header reads back without a passphrase, naming both addresses.
        let mut master_buf = [0i8; 64];
        let mut agent_buf = [0i8; 64];
        let mut valid_until = 0u64;
        let mut is_mainnet = true;
        assert_eq!(
            unsafe {
                crate::ffi::pc_keystore_peek(
                    c_path.as_ptr(),
                    master_buf.as_mut_ptr(),
                    64,
                    agent_buf.as_mut_ptr(),
                    64,
                    &mut valid_until,
                    &mut is_mainnet,
                )
            },
            0
        );
        let expected_master = format_address(&crate::signer::address_from_verifying_key(
            master.verifying_key(),
        ));
        assert_eq!(
            unsafe { CStr::from_ptr(master_buf.as_ptr()) }
                .to_str()
                .unwrap(),
            expected_master
        );
        assert!(!is_mainnet);
        assert_eq!(valid_until, unsafe { pc_setup_valid_until_ms(s) });

        // And it unlocks into a Session bound to the agent that was approved.
        let text = std::fs::read_to_string(&path).unwrap();
        let keystore: Keystore = serde_json::from_str(&text).unwrap();
        let secret = keystore.open("correct horse battery staple").unwrap();
        let agent_address = unsafe { &*s }.agent_address;
        assert_eq!(
            crate::signer::address_from_verifying_key(
                SigningKey::from_slice(secret.as_bytes())
                    .unwrap()
                    .verifying_key()
            ),
            agent_address,
            "unlocked key is not the agent that was approved"
        );
        // A testnet keystore must not open against a mainnet engine — the network field is
        // AAD-bound precisely so this cannot be edited into agreement (docs/06 §3).
        assert!(crate::ffi::unlock_keystore(
            true,
            path.to_str().unwrap(),
            "correct horse battery staple"
        )
        .is_err());

        let _ = std::fs::remove_dir_all(&dir);
        unsafe { pc_setup_free(s) };
    }

    /// The venue rejects an over-long agent name with "Extra agent name must be between 1
    /// and 16 characters long." — found the hard way, against live mainnet, after the user
    /// had already entered their private key. The cap is on the base name; the
    /// ` valid_until <ms>` suffix is parsed off by the venue and does not count.
    #[test]
    fn agent_name_stays_within_the_venue_16_char_cap() {
        for _ in 0..32 {
            let s = begin(false);
            let base = unsafe { &*s }.agent_name.clone();
            assert!(
                !base.is_empty() && base.len() <= MAX_AGENT_NAME_LEN,
                "default agent name {base:?} is {} chars, venue allows 1..=16",
                base.len()
            );
            unsafe { pc_setup_free(s) };
        }

        // A caller-supplied name is truncated here rather than rejected by the venue after
        // the user has already typed a private key and waited on a network round trip.
        let long = CString::new("a-very-long-agent-name-indeed").unwrap();
        let s = unsafe { pc_setup_begin(false, long.as_ptr(), 0) };
        let base = unsafe { &*s }.agent_name.clone();
        assert_eq!(base.len(), MAX_AGENT_NAME_LEN);
        assert_eq!(base, "a-very-long-agen");
        unsafe { pc_setup_free(s) };
    }

    #[test]
    fn agent_name_carries_the_expiry_the_venue_enforces() {
        let s = begin(false);
        let mut buf = [0i8; 128];
        assert!(unsafe { pc_setup_agent_name(s, buf.as_mut_ptr(), 128) } > 0);
        let name = unsafe { CStr::from_ptr(buf.as_ptr()) }.to_str().unwrap();
        assert!(name.starts_with("parsec-"));
        assert!(name.contains(" valid_until "));
        let valid_until = unsafe { pc_setup_valid_until_ms(s) };
        assert!(name.ends_with(&valid_until.to_string()));
        // Default lifetime is the venue's 180-day maximum, not longer.
        assert!(valid_until <= crate::ffi::actions::now_ms() + MAX_AGENT_LIFETIME_MS);
        unsafe { pc_setup_free(s) };
    }

    #[test]
    fn lifetime_is_clamped_to_the_venue_maximum() {
        let s = unsafe { pc_setup_begin(false, ptr::null(), u64::MAX) };
        let valid_until = unsafe { pc_setup_valid_until_ms(s) };
        assert!(valid_until <= crate::ffi::actions::now_ms() + MAX_AGENT_LIFETIME_MS);
        unsafe { pc_setup_free(s) };
    }
}
