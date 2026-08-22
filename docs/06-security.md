# Parsec — key custody and security

Implements decision **D2** from `01-overview.md`: the MetaMask master key is used once and
never stored; parsec holds only a Hyperliquid **agent wallet** key, encrypted at rest.

---

## 1. Why an agent wallet changes the whole threat model

Hyperliquid has two signing schemes (03 §4). L1 actions — `order`, `cancel`, `modify`,
`updateLeverage`, `updateIsolatedMargin`, `scheduleCancel` — are signed through a *phantom
agent* indirection, so an approved agent key can sign them on the master's behalf. Fund
movement — `withdraw3`, `usdSend`, `spotSend`, `usdClassTransfer` — and account-security
actions — `approveAgent`, `approveBuilderFee`, `convertToMultiSigUser` — are **user-signed**
actions whose EIP-712 message contains the action fields directly. The recovered signer *is*
the acting account, so an agent key signing a withdrawal recovers to the agent's own address,
which holds nothing.

The consequence is the entire justification for this design:

> **Worst case with an agent key: an attacker trades your account into the ground.
> Worst case with the master key: an attacker takes the money and leaves.**

Both are bad. Only one is survivable, and only one is reversible by revoking the agent.

**Verify this on testnet before trusting it** (`08-testing.md`). The docs describe the two
schemes but publish no explicit "agents cannot withdraw" statement, so the allow/deny list is
marked UNVERIFIED in 03 §5. A ten-minute test — approve an agent, attempt `withdraw3` with the
agent key, confirm the rejection — converts an inference into a fact, and it is the single
highest-value test in the project.

### Agent wallet facts that constrain the implementation

| Fact | Consequence for parsec |
|---|---|
| Default expiry 180 days; custom via `agentName = "<name> valid_until <ms>"`, max 180 days | Store `valid_until_ms`; warn in the UI at 14 days, refuse to start at 0 |
| 1 unnamed + up to 3 named agents per account | parsec always registers a **named** agent (`parsec-<host>-<short-id>`) so it never silently deregisters another tool's unnamed agent |
| A matching-name `approveAgent` deregisters that named agent | Rotation is just re-approving the same name — no orphan agents accumulate |
| Nonces are tracked **per signer** (the agent address), 100 most recent | One agent per running process. Never share a keystore between two concurrently running parsec instances |
| **Never reuse an agent address** — after deregistration nonce state may be pruned, making old signed actions replayable | Rotation always generates a **fresh keypair**. Never re-import an old agent key |
| Query info with the **master** address, sign with the **agent** key | The keystore stores both addresses; `Session` uses the right one per call. Using the agent address for `clearinghouseState` silently returns an empty account |

---

## 2. Onboarding — where the master key is, and for how long

```
  parsec setup --network testnet
        │
        ├─ 1. generate a fresh secp256k1 keypair          → agent_sk (32 B), agent_addr
        │
        ├─ 2. prompt for the MASTER private key            → master_sk, into mlock'd memory
        │        (stdin, no echo; never a CLI argument, never a file)
        │
        ├─ 3. build + sign `approveAgent` with master_sk   → user-signed EIP-712
        │        agentName = "parsec-<host>-<id> valid_until <now + 180d>"
        │
        ├─ 4. POST /exchange, confirm {"status":"ok"}
        │
        ├─ 5. ZERO master_sk immediately                   ← the only copy is now gone
        │
        ├─ 6. prompt for a keystore passphrase (twice)
        │
        └─ 7. write ~/.parsec/keystore-<network>.json, mode 0600
```

The master key exists in the process for the duration of one signature — seconds — and is
never written to disk, never logged, never included in an error message, and never passed as
an argument (where it would land in shell history and `ps` output).

**Alternative for the paranoid, supported from day one.** `parsec setup --print-approval`
emits the exact `approveAgent` EIP-712 payload and stops. Sign it wherever you like — MetaMask,
a hardware wallet, an air-gapped machine — and hand the signature back with
`parsec setup --signature 0x...`. The master key then never touches this process at all. This
path costs about thirty lines of code and removes the single largest residual risk, so it is
in Phase 2, not "later".

---

## 3. Keystore format

One file, `~/.parsec/keystore-<network>.json`, mode `0600`, JSON with a binary blob:

```json
{
  "version": 1,
  "network": "testnet",
  "master_address": "0x1234...",
  "agent_address": "0xabcd...",
  "agent_name": "parsec-mba-7f3c",
  "valid_until_ms": 1789000000000,
  "created_ms": 1755600000000,
  "kdf": {
    "name": "argon2id",
    "m_cost_kib": 262144,
    "t_cost": 3,
    "p_cost": 1,
    "salt": "<base64, 16 bytes>"
  },
  "cipher": {
    "name": "xchacha20poly1305",
    "nonce": "<base64, 24 bytes>"
  },
  "ciphertext": "<base64, 32-byte key + 16-byte tag>"
}
```

**Argon2id at m=1 GiB, t=4, p=1** — libsodium's `SENSITIVE` tier, roughly 3.5 s. That sounds
extreme until you notice this is unlocked *once per session* to protect a key that controls
money. `INTERACTIVE` (64 MiB, t=2) is the wrong tier here; `MODERATE` (256 MiB, t=3) is the
floor. The memory cost is the parameter that matters — it is what makes GPU and ASIC attacks
uneconomic.

Reference constants, read from libsodium source:
```
crypto_pwhash_argon2id_ALG_ARGON2ID13         2
crypto_pwhash_argon2id_SALTBYTES             16
OPSLIMIT_INTERACTIVE 2   MEMLIMIT_INTERACTIVE   67108864  (64 MiB)
OPSLIMIT_MODERATE    3   MEMLIMIT_MODERATE     268435456  (256 MiB)
OPSLIMIT_SENSITIVE   4   MEMLIMIT_SENSITIVE   1073741824  (1 GiB)
crypto_aead_xchacha20poly1305_ietf_KEYBYTES  32
crypto_aead_xchacha20poly1305_ietf_NPUBBYTES 24
crypto_aead_xchacha20poly1305_ietf_ABYTES    16
```
In RustCrypto terms (`argon2` crate) that is `m_cost = 1048576` KiB, `t_cost = 4`,
`p_cost = 1`. **Store the parameters in the file** so they can be raised later without
breaking existing keystores.

**XChaCha20-Poly1305** over AES-GCM for two reasons: its 192-bit nonce can be randomly
generated without any reuse concern, and it is constant-time in software everywhere (no
AES-NI dependency, which matters if this ever runs somewhere unusual).

**Every cleartext header field is bound as AAD.** The AAD is the canonical serialization of
`version | network | master_address | agent_address | valid_until_ms | kdf | cipher`. Without
this, an attacker who cannot decrypt the key could still flip `"network": "testnet"` to
`"mainnet"` and watch you fire testnet-sized orders at a live book. Authenticating the header
makes that a decryption failure instead of a very expensive afternoon.

**Wrong passphrase is indistinguishable from a corrupted file** — both are just AEAD tag
failures. The error message says so plainly rather than guessing, and never hints at how close
the passphrase was.

---

## 4. Secrets in memory

The agent key is 32 bytes that must live in RAM for the whole session, since every order
signs with it. Handling, in the Rust layer only — **the key never crosses the FFI boundary**,
so no C++ code ever holds key material:

```rust
pub struct SecretKey {
    bytes: Box<[u8; 32]>,   // heap, mlock'd
}

impl SecretKey {
    fn new(bytes: [u8; 32]) -> io::Result<Self> {
        let mut b = Box::new(bytes);
        // SAFETY: pointer is valid for 32 bytes and stays pinned for the box's lifetime
        unsafe { libc::mlock(b.as_ptr() as *const _, 32) };   // keep out of swap
        Ok(Self { bytes: b })
    }
}

impl Drop for SecretKey {
    fn drop(&mut self) {
        // volatile write: the compiler may not elide it as a dead store
        for b in self.bytes.iter_mut() {
            unsafe { std::ptr::write_volatile(b, 0) };
        }
        std::sync::atomic::compiler_fence(Ordering::SeqCst);
        unsafe { libc::munlock(self.bytes.as_ptr() as *const _, 32) };
    }
}
```

Three details that are easy to get wrong:

- **A plain `memset`/`= 0` on a dying buffer is a dead store and compilers delete it.** This
  was reproduced, not assumed. With `clang -O2` on arm64:
  ```c
  void bad(void){ char key[32]; use(key); memset(key,0,sizeof key); }
  ```
  the generated assembly contains **zero store instructions** between `bl _use` and `ret` —
  the compiler proved `key` was dead and deleted the `memset` under the as-if rule. The key
  stays in the stack frame. Writing through a `volatile` pointer instead emits 32 explicit
  `strb wzr, [sp, #N]` stores, because volatile accesses are observable behaviour.

  Portable zeroing options, in order of preference:

  | Function | Availability | Note |
  |---|---|---|
  | `zeroize::Zeroizing` | Rust | **what parsec uses** — `write_volatile` + `compiler_fence`, immune by construction |
  | `sodium_memzero` | any, libsodium | one call, works everywhere |
  | `explicit_bzero` | glibc >= 2.25, macOS 10.12+, BSD | portable enough behind an `#ifdef` |
  | `memset_s` | C11 Annex K | macOS yes (needs `-D__STDC_WANT_LIB_EXT1__=1`), **glibc no** |
- **`mlock` keeps the key out of the swap file**, which is the difference between a secret
  that dies with the process and one that persists on disk after a crash. It needs no
  privileges for a page or two. Note `RLIMIT_MEMLOCK` is often 64 KiB unprivileged on Linux —
  fine for a 32-byte key, and it will fail for the 1 GiB argon2id working buffer. That failure
  is expected and harmless; do not treat it as an error.
- The passphrase buffer and the decrypted key get identical treatment, as does the master key
  during setup. Prefer the `zeroize` crate (`Zeroizing<Vec<u8>>`) over hand-rolling this in
  the places where the type is not a fixed array.

**Core dumps.** A crash dump would contain the key. Set `RLIMIT_CORE` to 0 at startup unless
`PARSEC_ALLOW_CORE=1` — debuggability is worth trading for a secret that does not outlive the
process.

---

## 5. Rules the code must hold to

Enforceable, checkable, and worth a test each:

1. **The key never crosses the FFI boundary.** `parsec.h` has no field that can hold key
   material. C++ cannot leak what it cannot see.
2. **No secret is ever formatted.** `SecretKey` implements `Debug` and `Display` as
   `SecretKey(<redacted>)`. A `{:?}` in a log line is then harmless by construction rather
   than by vigilance.
3. **Structured logging with an allowlist**, not a denylist. Log lines are built from typed
   fields; there is no code path that formats an arbitrary struct into a log.
4. **`~/.parsec/` is `0700`, keystore `0600`**, verified at open; parsec refuses to start on
   looser permissions rather than warning and continuing.
5. **`.gitignore` covers `*.json` under `.parsec/` and any `keystore*`** — and the repo
   contains no key material even in tests, which use freshly generated throwaway keys.
6. **No telemetry, no crash reporting, no auto-update.** parsec talks to exactly one host: the
   configured Hyperliquid API. Anything else is a bug.
7. **Mainnet requires two independent signals** — a config flag *and* a typed confirmation at
   startup. The window title and a persistent header badge show the network at all times, in a
   different accent colour.

---

## 6. Threat model — what this does and does not stop

| Threat | Outcome |
|---|---|
| Laptop stolen, powered off | Keystore is argon2id-hardened. A strong passphrase holds. |
| Backup or cloud sync leaks the keystore | Same — the file is useless without the passphrase. |
| Malware reads `~/.parsec/` while parsec is not running | Same. |
| Shoulder-surfed or keylogged passphrase | Keystore falls. Attacker gets **trading rights only**; revoke by approving a new agent with the same name. |
| Malware with local code execution **while parsec is unlocked and running** | The key is in that process's memory. Game over for the agent key. `mlock` and zeroing do not help here — nothing at this layer does. Still bounded: trading rights, not withdrawal rights. |
| Compromised dependency in the build | Game over. Mitigation is `Cargo.lock` + pinned CMake FetchContent tags + `cargo audit` in CI, which reduces exposure without eliminating it. |
| Hostile network / MITM | rustls with certificate verification, no custom roots, no "skip verify" flag anywhere in the codebase — not even behind a debug build. |
| parsec crashes with orders resting | `scheduleCancel` deadline expires within 30 s and the venue cancels everything (02 §8). |
| Agent key silently expires | `valid_until_ms` checked at startup and daily; UI warns from 14 days out. |

**The honest limit:** an attacker who can run code as your user while parsec is unlocked wins,
and no amount of client-side hardening changes that. What this design buys is that the loss is
capped at trading rights on one account, is revocable in one action, and does not extend to the
funds themselves. That is the achievable goal; pretending to more would be theatre.

---

## 7. Rotation and revocation

- **Rotate** — `parsec rotate-agent` generates a fresh keypair, re-runs `approveAgent` with
  the *same* `agentName` (which deregisters the old one), and rewrites the keystore. Requires
  the master key again, or the offline-signing path from §2.
- **Revoke in an emergency** — approving *any* new agent under the same name immediately
  invalidates the old key. From a phone with MetaMask this is faster than getting to a laptop,
  so it belongs in the README as an incident-response note, not just in code.
- **Expiry is not revocation.** An expired agent stops working but the key still exists; treat
  a leaked key as live until it has been explicitly superseded.

---

## 8. Footnote: why not the macOS Keychain

Decision D3 (macOS + Linux from day one) already rules out a Keychain-only design, but the
measurement is worth recording in case a macOS-only convenience layer is ever added on top of
the portable keystore:

- The **modern data-protection keychain** (`kSecUseDataProtectionKeychain = true`) returns
  **`errSecMissingEntitlement` (-34018)** for an unsigned binary. Ad-hoc signing with a
  `keychain-access-groups` entitlement gets the process **SIGKILLed** (exit 137) — it requires
  a real Developer ID team identity.
- The **legacy file-based keychain** (omit `kSecUseDataProtectionKeychain`) works unsigned;
  full `SecItemAdd` / `SecItemCopyMatching` / `SecItemUpdate` / `SecItemDelete` CRUD was
  verified round-tripping with `OSStatus 0`.

So a Keychain path is viable only via the legacy API, or by taking on Apple code-signing as a
build requirement. Neither is worth it when the portable keystore already exists and must be
maintained for Linux regardless.
