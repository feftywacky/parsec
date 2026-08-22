//! EIP-712 typed-data signing for Hyperliquid "user-signed" actions (03 §4b).
//!
//! Unlike L1 actions (`signer::l1`), these are signed *directly*: the EIP-712 message
//! is the action's own fields, with no msgpack step and no phantom-agent indirection.
//! The recovered signer **is** the account that authorizes the action — this is the
//! entire reason an agent key cannot move funds (06 §1): an agent signing a `Withdraw`
//! recovers to the agent's own, fundless address, not the master's.
//!
//! `HyperliquidTransaction:ApproveAgent` is implemented for real use (agent onboarding
//! and rotation). `Withdraw` and `UsdClassTransfer` are implemented only so their
//! agent-key-recovers-to-agent-address property can be exercised in tests — parsec
//! itself never constructs a withdrawal or a transfer.
use super::{keccak, word};
use k256::ecdsa::signature::hazmat::PrehashSigner;
use k256::ecdsa::{RecoveryId, Signature, SigningKey};

/// One EIP-712 field value, tagged by its Solidity ABI type name. The type name feeds
/// the `primaryType(type1 name1,type2 name2,...)` type-string that is keccak-hashed to
/// produce the struct's type hash; the encoding rules below follow the EIP-712 spec's
/// "atomic type" and "dynamic type" cases (dynamic `string`/`bytes` are hashed, not
/// embedded).
pub enum FieldValue {
    Str(String),
    Address([u8; 20]),
    Bool(bool),
    Uint64(u64),
    Bytes32([u8; 32]),
}

impl FieldValue {
    fn type_name(&self) -> &'static str {
        match self {
            FieldValue::Str(_) => "string",
            FieldValue::Address(_) => "address",
            FieldValue::Bool(_) => "bool",
            FieldValue::Uint64(_) => "uint64",
            FieldValue::Bytes32(_) => "bytes32",
        }
    }

    fn encode(&self) -> [u8; 32] {
        match self {
            FieldValue::Str(s) => keccak(s.as_bytes()),
            FieldValue::Address(a) => word(a),
            FieldValue::Bool(b) => word(&[*b as u8]),
            FieldValue::Uint64(v) => word(&v.to_be_bytes()),
            FieldValue::Bytes32(b) => *b,
        }
    }
}

/// A named, typed EIP-712 field. Order matters: it is part of the type-string and
/// therefore part of the struct hash (03 §4b lists every primary type's field order
/// verbatim from `signing.py`; the builders below reproduce those orders exactly).
pub struct Field {
    pub name: &'static str,
    pub value: FieldValue,
}

fn encode_type(primary_type: &str, fields: &[Field]) -> Vec<u8> {
    let mut s = String::with_capacity(primary_type.len() + 16 * fields.len());
    s.push_str(primary_type);
    s.push('(');
    for (i, f) in fields.iter().enumerate() {
        if i > 0 {
            s.push(',');
        }
        s.push_str(f.value.type_name());
        s.push(' ');
        s.push_str(f.name);
    }
    s.push(')');
    s.into_bytes()
}

/// `keccak256(typeHash || encode(field_0) || encode(field_1) || ...)`.
pub fn struct_hash(primary_type: &str, fields: &[Field]) -> [u8; 32] {
    let type_hash = keccak(&encode_type(primary_type, fields));
    let mut enc = Vec::with_capacity(32 * (1 + fields.len()));
    enc.extend_from_slice(&type_hash);
    for f in fields {
        enc.extend_from_slice(&f.value.encode());
    }
    keccak(&enc)
}

/// Domain separator for `EIP712Domain(string name,string version,uint256 chainId,
/// address verifyingContract)` with `name="HyperliquidSignTransaction"`, `version="1"`,
/// `verifyingContract=0x0`. `chainId` comes from the action's own `signatureChainId`
/// hex field (03 §4b) — it only sets the domain the wallet signs on; the actual
/// environment (mainnet vs testnet) is carried by `hyperliquidChain` inside the message.
pub fn domain_separator(chain_id: u64) -> [u8; 32] {
    let type_hash = keccak(
        b"EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)",
    );
    let mut enc = Vec::with_capacity(32 * 5);
    enc.extend_from_slice(&type_hash);
    enc.extend_from_slice(&keccak(b"HyperliquidSignTransaction"));
    enc.extend_from_slice(&keccak(b"1"));
    enc.extend_from_slice(&word(&chain_id.to_be_bytes()));
    enc.extend_from_slice(&[0u8; 32]);
    keccak(&enc)
}

/// The final EIP-712 digest: `keccak256(0x1901 || domainSeparator || structHash)`.
pub fn digest(chain_id: u64, primary_type: &str, fields: &[Field]) -> [u8; 32] {
    let dom = domain_separator(chain_id);
    let sh = struct_hash(primary_type, fields);
    let mut buf = [0u8; 66];
    buf[0] = 0x19;
    buf[1] = 0x01;
    buf[2..34].copy_from_slice(&dom);
    buf[34..66].copy_from_slice(&sh);
    keccak(&buf)
}

/// Sign a user-signed action's EIP-712 digest. `expiresAfter` is not supported on this
/// path at all (03 §4b) — there is no analogue of the L1 nonce-window byte here.
pub fn sign(sk: &SigningKey, chain_id: u64, primary_type: &str, fields: &[Field]) -> [u8; 65] {
    let d = digest(chain_id, primary_type, fields);
    let signature: Signature = sk
        .sign_prehash(&d)
        .expect("ecdsa signing does not fail for a valid scalar key and 32-byte digest");
    let recid = RecoveryId::trial_recovery_from_prehash(sk.verifying_key(), &d, &signature)
        .expect("recoverable signature");
    let mut out = [0u8; 65];
    out[..64].copy_from_slice(&signature.to_bytes());
    out[64] = 27 + recid.to_byte();
    out
}

pub const APPROVE_AGENT_TYPE: &str = "HyperliquidTransaction:ApproveAgent";
pub const WITHDRAW_TYPE: &str = "HyperliquidTransaction:Withdraw";
pub const USD_CLASS_TRANSFER_TYPE: &str = "HyperliquidTransaction:UsdClassTransfer";

/// Field order verbatim from `signing.py`'s `sign_agent`: `hyperliquidChain`,
/// `agentAddress:address`, `agentName:string`, `nonce:uint64`.
pub fn approve_agent_fields(
    hyperliquid_chain: &str,
    agent_address: [u8; 20],
    agent_name: &str,
    nonce: u64,
) -> Vec<Field> {
    vec![
        Field {
            name: "hyperliquidChain",
            value: FieldValue::Str(hyperliquid_chain.to_string()),
        },
        Field {
            name: "agentAddress",
            value: FieldValue::Address(agent_address),
        },
        Field {
            name: "agentName",
            value: FieldValue::Str(agent_name.to_string()),
        },
        Field {
            name: "nonce",
            value: FieldValue::Uint64(nonce),
        },
    ]
}

/// Field order verbatim from `signing.py`: `hyperliquidChain`, `destination:string`,
/// `amount:string`, `time:uint64`. **Never invoked by parsec** — implemented only so
/// `signer::tests` (and the integration suite) can prove an agent-key signature over a
/// withdrawal recovers to the agent's own address, not the master's.
pub fn withdraw_fields(
    hyperliquid_chain: &str,
    destination: &str,
    amount: &str,
    time: u64,
) -> Vec<Field> {
    vec![
        Field {
            name: "hyperliquidChain",
            value: FieldValue::Str(hyperliquid_chain.to_string()),
        },
        Field {
            name: "destination",
            value: FieldValue::Str(destination.to_string()),
        },
        Field {
            name: "amount",
            value: FieldValue::Str(amount.to_string()),
        },
        Field {
            name: "time",
            value: FieldValue::Uint64(time),
        },
    ]
}

/// Field order verbatim from `signing.py`: `hyperliquidChain`, `amount:string`,
/// `toPerp:bool`, `nonce:uint64`. **Never invoked by parsec** — see `withdraw_fields`.
pub fn usd_class_transfer_fields(
    hyperliquid_chain: &str,
    amount: &str,
    to_perp: bool,
    nonce: u64,
) -> Vec<Field> {
    vec![
        Field {
            name: "hyperliquidChain",
            value: FieldValue::Str(hyperliquid_chain.to_string()),
        },
        Field {
            name: "amount",
            value: FieldValue::Str(amount.to_string()),
        },
        Field {
            name: "toPerp",
            value: FieldValue::Bool(to_perp),
        },
        Field {
            name: "nonce",
            value: FieldValue::Uint64(nonce),
        },
    ]
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Cross-checked against `eth_account.messages.encode_typed_data` (a standard,
    /// independent EIP-712 implementation) for the exact same domain/types/message —
    /// see the task report for the derivation. This pins the generic struct-hash
    /// machinery; `tests/signing_vectors.rs` additionally pins the real Hyperliquid
    /// Python SDK's `sign_agent` output for the same action.
    #[test]
    fn approve_agent_digest_matches_oracle() {
        let fields = approve_agent_fields(
            "Mainnet",
            super::super::parse_address("0x1234567890123456789012345678901234567890").unwrap(),
            "parsec-test valid_until 1750000000000",
            1716531066415,
        );
        let dom = domain_separator(0x66eee);
        let sh = struct_hash(APPROVE_AGENT_TYPE, &fields);
        let d = digest(0x66eee, APPROVE_AGENT_TYPE, &fields);
        assert_eq!(
            hex::encode(dom),
            "feb1393ca4412a4ca577bd51d04f0a77033514c602f4d0a11490fb95f7428df6"
        );
        assert_eq!(
            hex::encode(sh),
            "094d7be7563801358b102d752bb5452c00b639910b4ea292e906245d3497dc89"
        );
        assert_eq!(
            hex::encode(d),
            "f793aafec4aad2293f25d10bd7d6abffc870191c4f1085fd27dcad252dddbf27"
        );
    }
}
