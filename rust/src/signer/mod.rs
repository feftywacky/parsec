//! Signing and key custody for the two Hyperliquid signature schemes (03 §4) plus the
//! agent-wallet lifecycle and encrypted keystore that sit on top of them.
//!
//! - `l1` — phantom-agent signing for trading actions (order, cancel, ...).
//! - `user_signed` — EIP-712 typed-data signing for account-security / fund-moving actions.
//! - `agent` — agent keypair generation, address derivation, `approveAgent` construction.
//! - `keystore` — encrypted-at-rest storage for the agent private key (06 §3, §4).
pub mod agent;
pub mod keystore;
pub mod l1;
pub mod user_signed;

use k256::ecdsa::signature::hazmat::PrehashVerifier as _;
use k256::ecdsa::{RecoveryId, Signature, VerifyingKey};
use sha3::{Digest, Keccak256};
use thiserror::Error;

pub(crate) fn keccak(data: &[u8]) -> [u8; 32] {
    let mut h = Keccak256::new();
    h.update(data);
    h.finalize().into()
}

/// abi.encode of a value into a left-padded 32-byte word.
pub(crate) fn word(value: &[u8]) -> [u8; 32] {
    let mut out = [0u8; 32];
    let start = 32usize.saturating_sub(value.len());
    out[start..].copy_from_slice(&value[value.len().saturating_sub(32)..]);
    out
}

/// Ethereum address (last 20 bytes of `keccak256(uncompressed_pubkey[1..])`) for a
/// secp256k1 verifying key. Shared by agent-address derivation (`agent::derive_address`)
/// and signature recovery (`recover_address`) so both paths agree by construction.
pub fn address_from_verifying_key(vk: &VerifyingKey) -> [u8; 20] {
    let point = vk.to_encoded_point(false);
    let hash = keccak(&point.as_bytes()[1..]);
    let mut out = [0u8; 20];
    out.copy_from_slice(&hash[12..]);
    out
}

#[derive(Debug, Error, PartialEq, Eq)]
pub enum AddressError {
    #[error("address must be '0x' followed by 40 hex characters")]
    Invalid,
}

/// Parse a `0x`-prefixed hex address into raw bytes. Accepts either case on input;
/// callers that need the wire form must go through `format_address`, which always
/// lowercases (03 §4a: "addresses must be lowercase before signing").
pub fn parse_address(s: &str) -> Result<[u8; 20], AddressError> {
    let hex_part = s.strip_prefix("0x").ok_or(AddressError::Invalid)?;
    if hex_part.len() != 40 {
        return Err(AddressError::Invalid);
    }
    let mut out = [0u8; 20];
    hex::decode_to_slice(hex_part, &mut out).map_err(|_| AddressError::Invalid)?;
    Ok(out)
}

/// Format raw address bytes as the lowercase `0x...` wire representation.
pub fn format_address(bytes: &[u8; 20]) -> String {
    format!("0x{}", hex::encode(bytes))
}

#[derive(Debug, Error, PartialEq, Eq)]
pub enum RecoverError {
    #[error("signature v byte must be 27 or 28")]
    BadRecoveryId,
    #[error("malformed r||s signature")]
    BadSignature,
    #[error("signature does not recover to a valid public key")]
    RecoveryFailed,
}

/// Recover the signing address from a prehashed digest and a 65-byte `r||s||v` signature.
/// Used only in tests, to prove structurally that an agent key signing a fund-moving
/// user-signed action recovers to the agent's own (fundless) address rather than the
/// master's — the property the whole custody model in 06 §1 rests on.
pub fn recover_address(digest: &[u8; 32], sig: &[u8; 65]) -> Result<[u8; 20], RecoverError> {
    let recid =
        RecoveryId::from_byte(sig[64].wrapping_sub(27)).ok_or(RecoverError::BadRecoveryId)?;
    let signature = Signature::from_slice(&sig[..64]).map_err(|_| RecoverError::BadSignature)?;
    let vk = VerifyingKey::recover_from_prehash(digest, &signature, recid)
        .map_err(|_| RecoverError::RecoveryFailed)?;
    // Defensive: confirm the recovered key actually verifies before trusting its address.
    vk.verify_prehash(digest, &signature)
        .map_err(|_| RecoverError::RecoveryFailed)?;
    Ok(address_from_verifying_key(&vk))
}
