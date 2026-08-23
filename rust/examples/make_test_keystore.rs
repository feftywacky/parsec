//! Mint a keystore holding a throwaway agent key, so the unlock dialog and the locked/
//! unlocked UI states can be exercised without a funded account. Run with:
//!
//!     cargo run --release --example make_test_keystore -- <path> <passphrase>
//!
//! The agent inside is **not approved at any venue** and its master address is a
//! throwaway. parsec will unlock this file happily — the keystore knows nothing about
//! approval status — and then every authenticated action will be rejected by the venue.
//! That is exactly the state this is for: testing the sign-in path, not trading.
//!
//! Not part of the library's public surface and not wired into the C++ build. For a real
//! account, use `parsec setup` (docs/06 §2).
use k256::ecdsa::SigningKey;
use parsec_rs::signer::keystore::Keystore;
use parsec_rs::signer::{address_from_verifying_key, format_address};
use rand_core::OsRng;

fn main() {
    let mut args = std::env::args().skip(1);
    let path = args.next().unwrap_or_else(|| {
        eprintln!("usage: make_test_keystore <path> <passphrase> [--mainnet]");
        std::process::exit(2);
    });
    let passphrase = args.next().unwrap_or_else(|| {
        eprintln!("usage: make_test_keystore <path> <passphrase> [--mainnet]");
        std::process::exit(2);
    });
    let mainnet = args.any(|a| a == "--mainnet");

    let agent = SigningKey::random(&mut OsRng);
    let master = SigningKey::random(&mut OsRng);
    let agent_address = address_from_verifying_key(agent.verifying_key());
    let master_address = address_from_verifying_key(master.verifying_key());
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_millis() as u64;

    let mut key = [0u8; 32];
    key.copy_from_slice(&agent.to_bytes());

    eprintln!("sealing (Argon2id SENSITIVE tier, ~3.5s)...");
    let keystore = Keystore::seal(
        if mainnet { "mainnet" } else { "testnet" }.to_string(),
        format_address(&master_address),
        format_address(&agent_address),
        "parsec-test".to_string(),
        now + 180 * 24 * 60 * 60 * 1000,
        now,
        key,
        &passphrase,
    )
    .expect("seal");

    std::fs::write(&path, serde_json::to_string_pretty(&keystore).unwrap()).expect("write");
    // Match what `pc_setup_write_keystore` produces, so permission checks behave the same.
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600)).unwrap();
    }
    println!("wrote {path}");
    println!("  master (fake): {}", format_address(&master_address));
    println!("  agent  (fake): {}", format_address(&agent_address));
}
