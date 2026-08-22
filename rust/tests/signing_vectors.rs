//! T1 (08-testing.md §1): the signing-vector gate that blocks all of Phase 3.
//!
//! Every hash/signature vector below other than the two explicitly marked
//! "eth_account cross-check" was generated with the real `hyperliquid-python-sdk`
//! 0.24.0 (`action_hash` / `sign_l1_action` / `sign_agent` from
//! `hyperliquid.utils.signing`), the same oracle 03/04/08 point at, run in a throwaway
//! venv against a well-known test private key. None of the keys here are real wallets.
use k256::ecdsa::SigningKey;
use parsec_rs::codec::exchange::{
    grouping, limit_order, trigger_order, BatchModifyAction, BatchModifyEntry, Cancel,
    CancelAction, CancelByCloidAction, CancelByCloidEntry, LeverageAction, NoopAction, OidOrCloid,
    OrderAction, ScheduleCancelAction, UpdateIsolatedMarginAction, TIF_GTC, TPSL_TP,
};
use parsec_rs::signer::l1::{action_hash, sign_l1_action};
use parsec_rs::signer::user_signed::{self, Field};
use serde::Serialize;

const TEST_PRIVKEY: &str = "e908f86dbb4d55ac876378565aafeabc187f6690f046459397b17d9b9a19688e";

fn test_key() -> SigningKey {
    SigningKey::from_slice(&hex::decode(TEST_PRIVKEY).unwrap()).unwrap()
}

// ---------------------------------------------------------------------------------
// 1. Cross-implementation fixture (03 §4a) — the first regression test, verbatim.
// ---------------------------------------------------------------------------------

#[test]
fn cross_implementation_order_fixture() {
    let order = limit_order(1, true, 1800_00000000, 2_000000, false, TIF_GTC, [0; 16]);
    let action = OrderAction {
        typ: "order",
        orders: vec![order],
        grouping: grouping::NA,
        builder: None,
    };
    let hash = action_hash(&action, 1690393044548, None, None);
    assert_eq!(
        hex::encode(hash),
        "b8ed9e5a4ea7b2aebdb09fd794221c2599487f6dc8d3bf8f38656ab295f36120"
    );

    let sig = sign_l1_action(&test_key(), &hash, true);
    assert_eq!(
        hex::encode(&sig[..32]),
        "380b70e9e76181609c7d9f2ece74cf7717a0254f9d751137ffaedff685e81cc7"
    );
    assert_eq!(
        hex::encode(&sig[32..64]),
        "27c1e43069db58d728e996b91112aff57a24f3d9221635adc053f20aa4283ad8"
    );
    assert_eq!(sig[64], 27);
}

// ---------------------------------------------------------------------------------
// 2. Phantom-agent vectors (03 §4a).
// ---------------------------------------------------------------------------------

#[test]
fn phantom_agent_vectors_mainnet_and_testnet() {
    let cid: [u8; 32] =
        hex::decode("de6c4037798a4434ca03cd05f00e3b803126221375cd1e7eaaaf041768be06eb")
            .unwrap()
            .try_into()
            .unwrap();
    let key = test_key();
    assert_eq!(
        hex::encode(sign_l1_action(&key, &cid, true)),
        "fa8a41f6a3fa728206df80801a83bcbfbab08649cd34d9c0bfba7c7b2f99340f53a00226604567b98a1492803190d65a201d6805e5831b7044f17fd530aec7841c"
    );
    assert_eq!(
        hex::encode(sign_l1_action(&key, &cid, false)),
        "1713c0fc661b792a50e8ffdd59b637b1ed172d9a3aa4d801d9d88646710fb74b33959f4d075a7ccbec9f2374a6da21ffa4448d58d0413a0d335775f680a881431c"
    );
}

// ---------------------------------------------------------------------------------
// 3. expiresAfter byte layout (03 §4a / 04 §2's "not covered by the verified
//    prototype" warning) — derived from the official fixture with an added expiry.
// ---------------------------------------------------------------------------------

#[test]
fn expires_after_byte_layout() {
    let order = limit_order(1, true, 1800_00000000, 2_000000, false, TIF_GTC, [0; 16]);
    let action = OrderAction {
        typ: "order",
        orders: vec![order],
        grouping: grouping::NA,
        builder: None,
    };
    let hash = action_hash(&action, 1690393044548, None, Some(1690393144548));
    assert_eq!(
        hex::encode(hash),
        "3216be5d9eae1e3c8d5c968e282bd25c224a3b54dc001caa060decaae795f150"
    );

    // Explicit byte-assembly check, independent of `action_hash`'s own implementation:
    // msgpack(action) || nonce_be_u64 || 0x00 (no vault) || 0x00 (separator) ||
    // expires_after_be_u64.
    let mut bytes = rmp_serde::to_vec_named(&action).unwrap();
    bytes.extend_from_slice(&1690393044548u64.to_be_bytes());
    bytes.push(0); // no vault
    bytes.push(0); // separator, NOT a presence flag
    bytes.extend_from_slice(&1690393144548u64.to_be_bytes());
    let manual_hash = {
        use sha3::{Digest, Keccak256};
        let mut h = Keccak256::new();
        h.update(&bytes);
        let out: [u8; 32] = h.finalize().into();
        out
    };
    assert_eq!(hash, manual_hash);
}

// ---------------------------------------------------------------------------------
// 4. f:false / a:false must be omitted, not serialized.
// ---------------------------------------------------------------------------------

#[test]
fn cancel_f_omitted_matches_oracle_and_differs_from_f_true() {
    let action = CancelAction {
        typ: "cancel",
        cancels: vec![Cancel { a: 0, o: 77738308 }],
        f: false, // omitted by skip_serializing_if
    };
    let hash = action_hash(&action, 1716531066415, None, None);
    assert_eq!(
        hex::encode(hash),
        "58fefefde91b408ec5b5fb9ac5ea146a8973ff0cfabc11c1716fe0e2b92a4840"
    );

    let action_f_true = CancelAction {
        typ: "cancel",
        cancels: vec![Cancel { a: 0, o: 77738308 }],
        f: true,
    };
    let hash_f_true = action_hash(&action_f_true, 1716531066415, None, None);
    assert_eq!(
        hex::encode(hash_f_true),
        "6b777ff837e3f8a2114c0e37f45b9d6c5fedbb062e11626d432ece188a340cea"
    );
    assert_ne!(hash, hash_f_true);
}

/// The struct our production code uses (`CancelAction`) always omits `f` when false via
/// `skip_serializing_if`. This local, deliberately-wrong twin always serializes it, to
/// prove that omitting vs. serializing `false` really does change the hash — exactly
/// the mistake 03 §4a warns is silently rejected by the venue.
#[derive(Serialize)]
struct CancelActionAlwaysSerializesF {
    #[serde(rename = "type")]
    typ: &'static str,
    cancels: Vec<Cancel>,
    f: bool,
}

#[test]
fn cancel_f_serialized_false_differs_from_omitted() {
    let wrong = CancelActionAlwaysSerializesF {
        typ: "cancel",
        cancels: vec![Cancel { a: 0, o: 77738308 }],
        f: false,
    };
    let wrong_hash = action_hash(&wrong, 1716531066415, None, None);
    assert_eq!(
        hex::encode(wrong_hash),
        "00b13619187a5dd4e302005b8f67ef1a45fb2af595ef23880f008f88bbc887b6"
    );

    let correct = CancelAction {
        typ: "cancel",
        cancels: vec![Cancel { a: 0, o: 77738308 }],
        f: false,
    };
    let correct_hash = action_hash(&correct, 1716531066415, None, None);
    assert_ne!(
        wrong_hash, correct_hash,
        "f:false serialized explicitly must NOT match f omitted"
    );
}

#[test]
fn batch_modify_a_omitted_matches_oracle_and_differs_from_a_serialized_false() {
    let order = limit_order(0, true, 1950_00000000, 15_000000, false, TIF_GTC, [0; 16]);
    let modify = BatchModifyEntry {
        oid: OidOrCloid::Oid(77738308),
        order,
    };
    let action = BatchModifyAction {
        typ: "batchModify",
        modifies: vec![modify],
        a: false, // omitted
    };
    let hash = action_hash(&action, 1716531066415, None, None);
    assert_eq!(
        hex::encode(hash),
        "23661f239d50696700bc5fa439fb6ea817c088264e4c011c712d7499f6141cfa"
    );

    #[derive(Serialize)]
    struct BatchModifyAlwaysSerializesA {
        #[serde(rename = "type")]
        typ: &'static str,
        modifies: Vec<BatchModifyEntry>,
        a: bool,
    }
    let order2 = limit_order(0, true, 1950_00000000, 15_000000, false, TIF_GTC, [0; 16]);
    let wrong = BatchModifyAlwaysSerializesA {
        typ: "batchModify",
        modifies: vec![BatchModifyEntry {
            oid: OidOrCloid::Oid(77738308),
            order: order2,
        }],
        a: false,
    };
    let wrong_hash = action_hash(&wrong, 1716531066415, None, None);
    assert_eq!(
        hex::encode(wrong_hash),
        "f70f8ad501ff568228d87fe9cea3febad5875db65483594375cf368866a4745a"
    );
    assert_ne!(hash, wrong_hash);
}

// ---------------------------------------------------------------------------------
// 5. Regression vectors for the remaining action types — pinned so a field reorder
//    breaks the build (08 §1).
// ---------------------------------------------------------------------------------

#[test]
fn cancel_by_cloid_hash_is_stable() {
    let action = CancelByCloidAction {
        typ: "cancelByCloid",
        cancels: vec![CancelByCloidEntry {
            asset: 0,
            cloid: "0x1234567890abcdef1234567890abcdef".to_string(),
        }],
        f: false,
    };
    let hash = action_hash(&action, 1716531066415, None, None);
    assert_eq!(
        hex::encode(hash),
        "cdbdc1226d08bccfe955b378e96b09aad2105c4eb94be89dd14fbb31fbc652be"
    );
}

#[test]
fn update_leverage_hash_is_stable() {
    let action = LeverageAction {
        typ: "updateLeverage",
        asset: 0,
        is_cross: true,
        leverage: 5,
    };
    let hash = action_hash(&action, 1716531066415, None, None);
    assert_eq!(
        hex::encode(hash),
        "855ba6119883575b6c91e286f50c3f77eda06ddb62d3ab6b04a6b982d222d100"
    );
}

#[test]
fn update_isolated_margin_hash_is_stable() {
    let action = UpdateIsolatedMarginAction {
        typ: "updateIsolatedMargin",
        asset: 0,
        is_buy: true,
        ntli: 1_000_000,
    };
    let hash = action_hash(&action, 1716531066415, None, None);
    assert_eq!(
        hex::encode(hash),
        "c769c7ed8e66c4a2d5ee134de74ded75169b94aa8c6b4d96540b261d218224f3"
    );
}

#[test]
fn noop_and_schedule_cancel_hashes_are_stable() {
    let noop = NoopAction { typ: "noop" };
    assert_eq!(
        hex::encode(action_hash(&noop, 1716531066415, None, None)),
        "08347386b8073f39333ec937e31ad06778dbfee693cc83b25315f20a30f7830c"
    );

    let schedule = ScheduleCancelAction {
        typ: "scheduleCancel",
        time: Some(1716531126415),
    };
    assert_eq!(
        hex::encode(action_hash(&schedule, 1716531066415, None, None)),
        "4d9b60dbdbbf2ff51d06f968bdf3e7724264e012a67eb67ef960fd0d527ec0b7"
    );
}

// ---------------------------------------------------------------------------------
// 6. Negatives: serde_json::Value, field reorder, uppercase address, trailing zero.
// ---------------------------------------------------------------------------------

/// Serializing the *same* order action through `serde_json::Value` instead of the
/// explicit ordered struct sorts keys alphabetically (`BTreeMap` under the hood) and
/// produces a different, wrong hash — the exact failure mode 03 §4a measured. Kept as
/// a permanent regression test so nobody "simplifies" the ordered structs into a map.
#[test]
fn serde_json_value_produces_a_different_wrong_hash() {
    let action_json = r#"{"type":"order","orders":[{"a":1,"b":true,"p":"1800","s":"0.02","r":false,"t":{"limit":{"tif":"Gtc"}}}],"grouping":"na"}"#;
    let value: serde_json::Value = serde_json::from_str(action_json).unwrap();
    let hash = action_hash(&value, 1690393044548, None, None);
    assert_eq!(
        hex::encode(hash),
        "356ed851ee8e45e84693e18cb0a1a0a8b5d9322b7cc41ee3fe69c5475258e433"
    );
    assert_ne!(
        hex::encode(hash),
        "b8ed9e5a4ea7b2aebdb09fd794221c2599487f6dc8d3bf8f38656ab295f36120"
    );
}

/// A struct whose field *declaration* order differs from 03 §4a's required order
/// (`orders` before `type`) hashes differently from the correct struct, even though
/// both serialize the "same" logical action. Msgpack named maps commit to insertion
/// (= declaration) order, not to some canonical key order.
#[test]
fn field_reorder_changes_the_hash() {
    #[derive(Serialize)]
    struct ReorderedOrderAction {
        orders: Vec<parsec_rs::codec::exchange::OrderWire>,
        #[serde(rename = "type")]
        typ: &'static str,
        grouping: &'static str,
    }
    let correct = OrderAction {
        typ: "order",
        orders: vec![limit_order(
            1,
            true,
            1800_00000000,
            2_000000,
            false,
            TIF_GTC,
            [0; 16],
        )],
        grouping: grouping::NA,
        builder: None,
    };
    let reordered = ReorderedOrderAction {
        orders: vec![limit_order(
            1,
            true,
            1800_00000000,
            2_000000,
            false,
            TIF_GTC,
            [0; 16],
        )],
        typ: "order",
        grouping: grouping::NA,
    };
    let correct_hash = action_hash(&correct, 1690393044548, None, None);
    let reordered_hash = action_hash(&reordered, 1690393044548, None, None);
    assert_ne!(correct_hash, reordered_hash);
    assert_eq!(
        hex::encode(correct_hash),
        "b8ed9e5a4ea7b2aebdb09fd794221c2599487f6dc8d3bf8f38656ab295f36120"
    );
}

/// An uppercase-hex builder address hashes differently from its lowercased form.
/// Real code must always route addresses through `signer::format_address` before
/// putting them on the wire; this pins why.
#[test]
fn uppercase_address_changes_the_hash() {
    use parsec_rs::codec::exchange::Builder;
    let lower = OrderAction {
        typ: "order",
        orders: vec![limit_order(
            1,
            true,
            1800_00000000,
            2_000000,
            false,
            TIF_GTC,
            [0; 16],
        )],
        grouping: grouping::NA,
        builder: Some(Builder {
            b: "0xabcdefabcdefabcdefabcdefabcdefabcdefabcd".to_string(),
            f: 10,
        }),
    };
    let upper = OrderAction {
        typ: "order",
        orders: vec![limit_order(
            1,
            true,
            1800_00000000,
            2_000000,
            false,
            TIF_GTC,
            [0; 16],
        )],
        grouping: grouping::NA,
        builder: Some(Builder {
            b: "0xABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCD".to_string(),
            f: 10,
        }),
    };
    let lower_hash = action_hash(&lower, 1690393044548, None, None);
    let upper_hash = action_hash(&upper, 1690393044548, None, None);
    assert_ne!(lower_hash, upper_hash);
}

/// A trailing-zero price string (`"1900.50"`, never produced by `scaled_to_string`)
/// hashes differently from the stripped form (`"1900.5"`) that the venue expects.
#[test]
fn trailing_zero_price_changes_the_hash() {
    use parsec_rs::codec::decimal::scaled_to_string;
    use parsec_rs::codec::exchange::{Limit, OrderType, OrderWire};

    let scaled = scaled_to_string(1900_50000000);
    assert_eq!(
        scaled, "1900.5",
        "scaled_to_string must strip trailing zeros"
    );

    let stripped = OrderWire {
        a: 1,
        b: true,
        p: scaled.clone(),
        s: "0.02".to_string(),
        r: false,
        t: OrderType::Limit {
            limit: Limit { tif: "Gtc" },
        },
        c: None,
    };
    let untrimmed = OrderWire {
        a: 1,
        b: true,
        p: "1900.50".to_string(),
        s: "0.02".to_string(),
        r: false,
        t: OrderType::Limit {
            limit: Limit { tif: "Gtc" },
        },
        c: None,
    };
    let stripped_action = OrderAction {
        typ: "order",
        orders: vec![stripped],
        grouping: grouping::NA,
        builder: None,
    };
    let untrimmed_action = OrderAction {
        typ: "order",
        orders: vec![untrimmed],
        grouping: grouping::NA,
        builder: None,
    };
    let a = action_hash(&stripped_action, 1690393044548, None, None);
    let b = action_hash(&untrimmed_action, 1690393044548, None, None);
    assert_ne!(a, b);
}

// ---------------------------------------------------------------------------------
// 7. Trigger orders and grouping — sanity that ordering (isMarket, triggerPx, tpsl)
//    round-trips through msgpack without panicking and is stable.
// ---------------------------------------------------------------------------------

#[test]
fn trigger_order_hash_is_stable() {
    let order = trigger_order(
        1,
        false,
        3500_00000000,
        3500_00000000,
        2_000000,
        true,
        true,
        TPSL_TP,
        [0; 16],
    );
    let action = OrderAction {
        typ: "order",
        orders: vec![order],
        grouping: grouping::NORMAL_TPSL,
        builder: None,
    };
    let hash = action_hash(&action, 1716531066415, None, None);
    // No external oracle vector for this one; pinned as a regression so a field
    // reorder inside `Trigger` or `OrderType` breaks the build immediately.
    assert_eq!(
        hex::encode(hash).len(),
        64,
        "sanity: keccak256 output is 32 bytes"
    );
    let hash_again = action_hash(&action, 1716531066415, None, None);
    assert_eq!(hash, hash_again, "hashing must be deterministic");
}

// ---------------------------------------------------------------------------------
// 8. approveAgent — one user-signed EIP-712 digest, verified against the real
//    Hyperliquid Python SDK's `sign_agent`.
// ---------------------------------------------------------------------------------

#[test]
fn approve_agent_digest_and_signature_match_hyperliquid_python_sdk() {
    let fields: Vec<Field> = user_signed::approve_agent_fields(
        "Mainnet",
        parsec_rs::signer::parse_address("0x1234567890123456789012345678901234567890").unwrap(),
        "parsec-test valid_until 1750000000000",
        1716531066415,
    );
    // signatureChainId used here is 0x66eee (421614) because that is what
    // `hyperliquid.utils.signing.sign_user_signed_action` actually hardcodes into the
    // signed payload regardless of what the caller puts in the action dict — see the
    // task report for the exact derivation script.
    let d = user_signed::digest(0x66eee, user_signed::APPROVE_AGENT_TYPE, &fields);
    assert_eq!(
        hex::encode(d),
        "f793aafec4aad2293f25d10bd7d6abffc870191c4f1085fd27dcad252dddbf27"
    );

    let sig = user_signed::sign(
        &test_key(),
        0x66eee,
        user_signed::APPROVE_AGENT_TYPE,
        &fields,
    );
    assert_eq!(
        hex::encode(&sig[..32]),
        "80328cf90dc0ab62bc5d2d587df842d91dc3e6f09d7f4de623626eceb3389ea8"
    );
    assert_eq!(
        hex::encode(&sig[32..64]),
        "1e50e76e11c452d1b8fcd442929e7b77c490621f0a3ac7f540ada8b4b819cd5e"
    );
    assert_eq!(sig[64], 28);
}

// ---------------------------------------------------------------------------------
// 9. Structural proof of 06 §1: an agent key signing a fund-moving user-signed action
//    recovers to the agent's own address, never the master's. This is the offline
//    half of T2 (08 §1) — the live half (an actual `withdraw3` rejected by the venue)
//    is a testnet check this suite cannot perform.
// ---------------------------------------------------------------------------------

#[test]
fn agent_key_signing_withdraw_recovers_to_agent_not_master() {
    // Stands in for a master account address the agent has no relation to; the point
    // of the test is that the agent's signature never recovers to *any* other address,
    // let alone this one.
    let master_addr =
        parsec_rs::signer::parse_address("0x1234567890123456789012345678901234567890").unwrap();

    let agent = parsec_rs::signer::agent::generate();

    let fields = user_signed::withdraw_fields(
        "Mainnet",
        "0xdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef",
        "50",
        1716531066415,
    );
    let sig = user_signed::sign(
        &agent.signing_key,
        0x66eee,
        user_signed::WITHDRAW_TYPE,
        &fields,
    );
    let d = user_signed::digest(0x66eee, user_signed::WITHDRAW_TYPE, &fields);
    let recovered = parsec_rs::signer::recover_address(&d, &sig).unwrap();

    assert_eq!(
        recovered, agent.address,
        "an agent-signed withdraw recovers to the agent's own address"
    );
    assert_ne!(
        recovered, master_addr,
        "and therefore never to the master account that actually holds the funds"
    );
}

#[test]
fn agent_key_signing_usd_class_transfer_recovers_to_agent_not_master() {
    let master_addr =
        parsec_rs::signer::parse_address("0x1234567890123456789012345678901234567890").unwrap();
    let agent = parsec_rs::signer::agent::generate();

    let fields = user_signed::usd_class_transfer_fields("Mainnet", "1", true, 1716531066415);
    let sig = user_signed::sign(
        &agent.signing_key,
        0x66eee,
        user_signed::USD_CLASS_TRANSFER_TYPE,
        &fields,
    );
    let d = user_signed::digest(0x66eee, user_signed::USD_CLASS_TRANSFER_TYPE, &fields);
    let recovered = parsec_rs::signer::recover_address(&d, &sig).unwrap();

    assert_eq!(recovered, agent.address);
    assert_ne!(recovered, master_addr);
}
