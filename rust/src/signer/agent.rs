//! Agent-wallet lifecycle (06 §2): generate a fresh keypair, derive its address, build
//! and sign the `approveAgent` action.
//!
//! **Never reuse an agent address** (03 §5) — after deregistration its nonce state may
//! be pruned, which would make previously signed actions replayable. Rotation always
//! calls `generate` again; there is no "re-import an existing agent key" path.
use super::user_signed::{self, Field};
use super::{address_from_verifying_key, format_address};
use k256::ecdsa::SigningKey;
use rand_core::OsRng;

/// A freshly generated agent (API wallet) keypair, plus its derived address.
///
/// The private key itself is handed to the caller as a `SigningKey`; it is the
/// caller's job (`Session`, `keystore::Keystore::seal`) to get it into `mlock`'d,
/// zeroize-on-drop storage promptly — this type does not hold it any longer than it
/// takes to generate and derive the address.
pub struct AgentKeypair {
    pub signing_key: SigningKey,
    pub address: [u8; 20],
}

/// Generate a brand-new secp256k1 keypair for use as a Hyperliquid agent wallet.
/// Uses the OS CSPRNG (`getrandom`) — never a deterministic or user-supplied seed.
pub fn generate() -> AgentKeypair {
    let signing_key = SigningKey::random(&mut OsRng);
    let address = derive_address(&signing_key);
    AgentKeypair {
        signing_key,
        address,
    }
}

/// Ethereum-style address for a secp256k1 signing key: `keccak256(uncompressed_pubkey
/// without the 0x04 prefix)[12..32]`.
pub fn derive_address(sk: &SigningKey) -> [u8; 20] {
    address_from_verifying_key(sk.verifying_key())
}

/// The `approveAgent` action, in the form needed both to sign it and to POST it.
/// `agent_name` is always `Some` in normal parsec operation — 06 §2 requires a named
/// agent (`parsec-<host>-<id>`) so parsec never silently deregisters another tool's
/// unnamed agent. The unnamed case (`agent_name: None`) is kept for completeness: per
/// the Python SDK, an unnamed approval is *signed* with `agentName: ""` but the field
/// is then omitted entirely from the posted action (03 §3).
pub struct ApproveAgentRequest {
    pub hyperliquid_chain: &'static str, // "Mainnet" | "Testnet"
    pub signature_chain_id: u64,
    pub agent_address: [u8; 20],
    pub agent_name: Option<String>,
    pub nonce: u64,
}

impl ApproveAgentRequest {
    /// The string signed into the EIP-712 message — `""` when `agent_name` is `None`,
    /// matching the Python SDK's `agentName: name or ""` convention exactly.
    fn signed_agent_name(&self) -> &str {
        self.agent_name.as_deref().unwrap_or("")
    }

    fn fields(&self) -> Vec<Field> {
        user_signed::approve_agent_fields(
            self.hyperliquid_chain,
            self.agent_address,
            self.signed_agent_name(),
            self.nonce,
        )
    }

    /// The digest the master key must sign. Exposed standalone for the offline
    /// `--print-approval` flow (06 §2): print this (or the full EIP-712 payload built
    /// from `fields()`) for signing on an air-gapped machine or hardware wallet, then
    /// feed the resulting signature back in via `--signature` — the master key never
    /// touches this process.
    pub fn digest(&self) -> [u8; 32] {
        user_signed::digest(
            self.signature_chain_id,
            user_signed::APPROVE_AGENT_TYPE,
            &self.fields(),
        )
    }

    /// Sign locally with a master key that *is* resident in this process (the
    /// non-offline onboarding path in 06 §2, steps 2-5). The key must be zeroized by
    /// the caller immediately after this call returns.
    pub fn sign(&self, master_sk: &SigningKey) -> [u8; 65] {
        user_signed::sign(
            master_sk,
            self.signature_chain_id,
            user_signed::APPROVE_AGENT_TYPE,
            &self.fields(),
        )
    }

    /// The `agentAddress` field as it must appear on the wire: lowercase hex.
    pub fn agent_address_hex(&self) -> String {
        format_address(&self.agent_address)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn derived_address_is_deterministic_and_stable_across_regeneration() {
        let a = generate();
        let b = generate();
        // Freshly generated keys must never collide in a way that could look like a
        // reused address; this is a sanity check that generation is actually random.
        assert_ne!(a.address, b.address);
        // Re-deriving from the same key must be idempotent.
        assert_eq!(derive_address(&a.signing_key), a.address);
    }

    #[test]
    fn print_approval_then_signature_flow_matches_direct_signing() {
        let master = SigningKey::random(&mut OsRng);
        let req = ApproveAgentRequest {
            hyperliquid_chain: "Testnet",
            signature_chain_id: 0x66eee,
            agent_address: generate().address,
            agent_name: Some("parsec-test".to_string()),
            nonce: 1_716_531_066_415,
        };
        // Offline path: only the digest leaves this process.
        let d = req.digest();
        let sig_offline = user_signed::sign(
            &master,
            req.signature_chain_id,
            user_signed::APPROVE_AGENT_TYPE,
            &req.fields(),
        );
        // Direct path: sign() does the same thing in one call.
        let sig_direct = req.sign(&master);
        assert_eq!(sig_offline, sig_direct);
        // And the digest signed is exactly what `sign` used internally.
        assert_eq!(
            super::super::recover_address(&d, &sig_direct).unwrap(),
            super::super::address_from_verifying_key(master.verifying_key())
        );
    }
}
