# Parsec — testing

A trading client fails in ways ordinary software does not: silently, expensively, and while
nobody is watching. The test strategy is shaped around that, not around coverage percentage.

---

## 1. The three tests that matter most

Everything else in this document is ordinary engineering hygiene. These three are the ones
that decide whether parsec is safe to leave running, and each must be **run against the live
venue**, not mocked.

### T1 — Signing vectors (offline, blocks all of Phase 3)

Hyperliquid's response to a malformed signature is
`L1 error: User or API Wallet 0x... does not exist.` It names neither the cause nor the field,
and the recovered address changes with every different input (03 §8). Debugging this against
the network is a guessing game.

**Already solved, and cross-verified against a second implementation.** The Python SDK
(`hyperliquid-python-sdk` 0.24.0) was installed and run against the same inputs; it produces
**identical** signatures and an identical `action_hash`
(`0xb8ed9e5a4ea7b2aebdb09fd794221c2599487f6dc8d3bf8f38656ab295f36120`). Two independent
implementations agreeing is the strongest correctness evidence available here.

Keep the Python SDK installed as a **test oracle**. `prototypes/signing-vectors/cross_check_python.py`
regenerates the fixture; when a new action type is added, generate its expected hash there
first, then make the Rust match. That turns "does my msgpack ordering match the server's
expectation?" from a network guessing game into a local diff.

`prototypes/signing-vectors/` reproduces both of the official SDK's signature test vectors
byte-for-byte:
```
mainnet=true  got=0xfa8a41f6...aec7841c  MATCH: true
mainnet=false got=0x1713c0fc...a881431c  MATCH: true
```
Port this into `rust/tests/signing_vectors.rs` and **extend it** to cover every action type
parsec sends: `order` (limit, trigger, with and without cloid), `cancel`, `cancelByCloid`,
`batchModify`, `updateLeverage`, `updateIsolatedMargin`, `scheduleCancel`, and the
`expiresAfter` variant (which the prototype does not yet cover). Add the user-signed
`approveAgent` path against its own vector.

Also assert the negatives, since each corresponds to a real production failure:
- **Serializing through `serde_json::Value` (or any key-sorting map) produces a different
  hash** — measured: `0x356ed851…` instead of `0xb8ed9e5a…`. This one is already demonstrated
  in the prototype; keep it as a test so nobody "simplifies" the ordered structs away later.
- `f: false` serialized rather than omitted produces a *different* hash.
- Field reordering produces a different hash.
- A trailing zero (`"1900.50"` vs `"1900.5"`) produces a different hash.
- An uppercase address produces a different hash.

### T2 — The agent cannot withdraw (testnet, Phase 3)

The entire security model rests on this (06 §1), and the docs never state it explicitly — it
is inferred from the two signing schemes. Approve an agent, then attempt `withdraw3`,
`usdSend`, and `usdClassTransfer` **with the agent key** and confirm each is rejected.

Ten minutes of work that converts the project's central safety assumption from an inference
into a fact. If it ever passes when it should fail, the whole custody design needs rethinking
before mainnet.

### T3 — The dead man's switch actually fires (testnet, Phase 5)

Place resting orders, then `kill -9` parsec. The venue must cancel them within 30 s.

This is the only thing standing between a crash and unattended exposure, so it gets tested by
actually killing the process — not by unit-testing the timer. Run it at least twice, including
once after a long-running session, since `scheduleCancel` allows only **10 triggers per UTC
day** and a bug that burns them silently would leave you unprotected.

---

## 2. Rust unit tests

| Area | What to test |
|---|---|
| `codec::decimal` | Round-trip `"113377.0"` ↔ `i64@1e8` exactly. Edge cases: `"0"`, `"-0"`, `"0.00001"`, `"1e-8"`-scale values, >8 decimals **rejected not truncated**, values near `i64` limits. **Fuzz it** — it is the only place wire data becomes numbers. |
| `codec::exchange` | Msgpack byte output matches expected for each action; `skip_serializing_if` omits false flags; field order is declaration order. |
| `signer::l1` / `user_signed` | T1 above. |
| `signer::keystore` | Seal/open round-trip; wrong passphrase fails; **flipping any AAD-bound header byte fails** (especially `network`); params are read back from the file, not hardcoded. |
| `transport::ws` | Reconnect/resubscribe state machine driven by a fake socket; 30-trade backfill dedupe; post-request timeout fires when no `post` envelope arrives. |
| `ffi::types` | `size_of` / `align_of` / `offset_of` assertions matching `parsec.h` (02 §5.5). |

---

## 3. C++ unit tests (doctest)

| Area | What to test |
|---|---|
| `exec::Rounder` | **Exhaustive.** `szDecimals` 0–6 × prices spanning 0.0001 → 500 000. All documented examples from 03 §6. Exactly-5-significant-figure boundaries. The integer escape hatch (`123456` valid, `12345.6` not). Min notional. `RoundMode::Passive` never rounds a limit more aggressive than requested. |
| `md::L2Book` | Snapshot replace; `best_bid`/`best_ask`/`mid`/`spread`; `sweep()` against hand-computed expected fills including partial-depth exhaustion. |
| `md::CandleSeries` | In-progress bucket overwritten while `t` matches, appended when it advances; REST backfill then WS handover leaves no gap or duplicate. |
| `core::Seqlock` | Concurrent writer/reader stress under TSan; reader never observes a torn snapshot. |
| `core::SpscRing` | Full/empty boundaries; overflow returns false rather than overwriting. |
| `exec::OrderState` | Every transition in the state machine, including ack-timeout → `Unknown`, and the invariant that **`Unknown` never auto-resends**. |
| `portfolio::pnl` | Unrealized P&L, ROE, margin used, and liquidation price against hand-computed values for **both cross and isolated**, long and short. |
| `risk::PreTradeChecks` | Each check rejects at its boundary and passes just inside it. |
| ABI | `static_assert`s mirroring the Rust side. |

Run the C++ suite under **ASan + UBSan** in CI, and the concurrency tests under **TSan**.

---

## 4. Replay harness

The recorder from Phase 6 dumps raw WS frames with arrival timestamps. The replay driver feeds
them into the engine in place of the Rust layer, at either wall-clock or maximum speed.

This gives what live testing cannot: **determinism**. A session that produced a bug becomes a
regression test. It also makes the engine testable without network, keys, or a venue — and it
is the foundation if the backtester idea from this repo's history ever returns (D1).

Worth capturing as fixtures: a normal trading hour, a reconnect, a fast market, a session with
fills and cancels, and any session that ever produced a reconciliation divergence.

---

## 5. Testnet integration checklist

Run before each phase gate. Every item is a real user-visible behaviour, not an internal
assertion.

**Market data**
- [ ] Book, BBO, tape, and candles stream for 30 min with no panic and no allocation growth
- [ ] Network drop for 60 s → clean reconnect, full resubscribe, no duplicate trades
- [ ] `l2Book` touch agrees with `bbo` within the coalescing window
- [ ] Staleness indicator trips when the feed is artificially delayed

**Account**
- [ ] Positions and balances match the web UI exactly, cross and isolated
- [ ] Unrealized P&L tracks mark price, not last
- [ ] Funding accrual matches `userFundings`

**Orders**
- [ ] Limit order rests, appears in Open Orders, cancels cleanly
- [ ] Market order fills; realized slippage is within the ticket's estimate
- [ ] TP/SL attaches; a filled child cancels its sibling (`siblingFilledCanceled`)
- [ ] Leverage change and cross↔isolated switch both take effect
- [ ] Sub-$10 order is rejected with a clear, visible message
- [ ] Post-only order that would cross is rejected with `badAloPxRejected`, surfaced as a toast
- [ ] Order placed from the web UI appears in parsec; order placed in parsec appears in the web UI

**Failure modes**
- [ ] T3 (dead man's switch)
- [ ] Reconciliation: cancel an order from the web UI → parsec detects, logs, and corrects
- [ ] Rate-limit budget meter decreases with activity and gates new orders below 10 %
- [ ] Unlock with a wrong passphrase fails cleanly and does not hint at closeness

---

## 6. CI

| Job | Contents |
|---|---|
| `build` | macOS **and Linux**, Debug + Release. Linux is unbuilt as of writing (05 §7) — wire it up in Phase 0, not later. |
| `test` | `cargo test` + `ctest`, with ASan/UBSan; TSan on the concurrency subset |
| `abi` | Layout assertions on both sides — fails loudly if a struct drifts |
| `lint` | `clippy -D warnings`, `clang-tidy`, `cargo fmt --check` |
| `audit` | `cargo audit`; dependency tags pinned to SHAs |
| `replay` | Recorded fixtures replayed through the engine, asserting final state |

CI must **not** hold keys or touch mainnet. Testnet integration runs are manual and gated on a
human reading the results — an automated job that places orders is a foot-gun with a cron
schedule.

---

## 7. What is deliberately not tested

- The venue's own correctness. If `clearinghouseState` and `userFills` disagree, that is
  reconciliation's job to surface, not a test's job to assert.
- UI pixel output. Panels are thin readers over snapshots; the snapshots are tested instead.
- Third-party library internals (ImGui, tokio). Pinned versions, and upgrades get a
  manual smoke pass.

## Slow security tests

Two tests run real SENSITIVE-tier Argon2id derivations (~3.5 s each) and are therefore
`#[ignore]`d so the default loop stays fast. Run them before changing anything in
`signer::keystore`, `ffi::setup`, or the unlock path:

```sh
cargo test --release -- --ignored
```

- `ffi::setup::tests::keystore_round_trips_from_seal_through_unlock` — seal, 0600/0700
  permissions, refusal to overwrite, header peek, and unlock back to the approved agent.
- `ffi::auth_tests::unlock_reports_failure_then_success_and_zeroes_the_passphrase` — the
  state machine the unlock dialog drives, including passphrase-buffer zeroing.

## Still unverified

The one thing no test here covers is a full authenticated round trip on a **funded** account:
`approveAgent` accepted, keystore written, order placed and filled. The onboarding path has
been exercised end to end against live testnet up to the venue's own reply — the venue
recovered the same signer address parsec did, which pins the EIP-712 digest — but it rejected
with "Must deposit before performing actions", so the accepted-approval branch is untested.
Do this on testnet with a funded account before trusting any of it, together with the
agent-cannot-withdraw check that 06 §1 calls the highest-value test in the project.
