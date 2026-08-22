use super::{keccak, word};
use k256::ecdsa::signature::hazmat::PrehashSigner;
use k256::ecdsa::{RecoveryId, Signature, SigningKey};
use serde::Serialize;

/// EIP-712 domain separator for Hyperliquid L1 actions: `name="Exchange"`,
/// `version="1"`, `chainId=1337` (hardcoded, independent of the wallet's actual
/// network), `verifyingContract=0x0` (03 §4a).
fn l1_domain_separator() -> [u8; 32] {
    let mut bytes = Vec::with_capacity(160);
    bytes.extend_from_slice(&keccak(
        b"EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)",
    ));
    bytes.extend_from_slice(&keccak(b"Exchange"));
    bytes.extend_from_slice(&keccak(b"1"));
    bytes.extend_from_slice(&word(&1337u64.to_be_bytes()));
    bytes.extend_from_slice(&[0; 32]);
    keccak(&bytes)
}
fn agent_struct_hash(source: &str, connection_id: &[u8; 32]) -> [u8; 32] {
    let mut bytes = Vec::with_capacity(96);
    bytes.extend_from_slice(&keccak(b"Agent(string source,bytes32 connectionId)"));
    bytes.extend_from_slice(&keccak(source.as_bytes()));
    bytes.extend_from_slice(connection_id);
    keccak(&bytes)
}
pub fn action_hash<T: Serialize>(
    action: &T,
    nonce: u64,
    vault: Option<[u8; 20]>,
    expires_after: Option<u64>,
) -> [u8; 32] {
    let mut bytes = rmp_serde::to_vec_named(action).expect("serializable action");
    bytes.extend_from_slice(&nonce.to_be_bytes());
    match vault {
        Some(address) => {
            bytes.push(1);
            bytes.extend_from_slice(&address)
        }
        None => bytes.push(0),
    }
    if let Some(expires) = expires_after {
        bytes.push(0);
        bytes.extend_from_slice(&expires.to_be_bytes())
    }
    keccak(&bytes)
}
pub fn sign_l1_action(key: &SigningKey, connection_id: &[u8; 32], mainnet: bool) -> [u8; 65] {
    let source = if mainnet { "a" } else { "b" };
    let mut digest_bytes = [0u8; 66];
    digest_bytes[..2].copy_from_slice(&[0x19, 0x01]);
    digest_bytes[2..34].copy_from_slice(&l1_domain_separator());
    digest_bytes[34..].copy_from_slice(&agent_struct_hash(source, connection_id));
    let digest = keccak(&digest_bytes);
    let signature: Signature = key.sign_prehash(&digest).expect("valid digest");
    let recid = RecoveryId::trial_recovery_from_prehash(key.verifying_key(), &digest, &signature)
        .expect("recoverable signature");
    let mut result = [0; 65];
    result[..64].copy_from_slice(&signature.to_bytes());
    result[64] = 27 + recid.to_byte();
    result
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn vectors() {
        let key = SigningKey::from_slice(
            &hex::decode("e908f86dbb4d55ac876378565aafeabc187f6690f046459397b17d9b9a19688e")
                .unwrap(),
        )
        .unwrap();
        let cid: [u8; 32] =
            hex::decode("de6c4037798a4434ca03cd05f00e3b803126221375cd1e7eaaaf041768be06eb")
                .unwrap()
                .try_into()
                .unwrap();
        assert_eq!(hex::encode(sign_l1_action(&key,&cid,true)),"fa8a41f6a3fa728206df80801a83bcbfbab08649cd34d9c0bfba7c7b2f99340f53a00226604567b98a1492803190d65a201d6805e5831b7044f17fd530aec7841c");
        assert_eq!(hex::encode(sign_l1_action(&key,&cid,false)),"1713c0fc661b792a50e8ffdd59b637b1ed172d9a3aa4d801d9d88646710fb74b33959f4d075a7ccbec9f2374a6da21ffa4448d58d0413a0d335775f680a881431c");
    }
}
