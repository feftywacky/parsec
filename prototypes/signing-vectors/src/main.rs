use k256::ecdsa::{RecoveryId, SigningKey, Signature as EcdsaSig};
use sha3::{Digest, Keccak256};

fn keccak(data: &[u8]) -> [u8; 32] {
    let mut h = Keccak256::new();
    h.update(data);
    h.finalize().into()
}

/// abi.encode of a 32-byte word (already left-padded)
fn word(b: &[u8]) -> [u8; 32] {
    let mut w = [0u8; 32];
    w[32 - b.len()..].copy_from_slice(b);
    w
}

/// EIP-712 domain separator for Hyperliquid L1 actions:
/// name="Exchange", version="1", chainId=1337, verifyingContract=0x0
fn l1_domain_separator() -> [u8; 32] {
    let type_hash = keccak(b"EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)");
    let mut enc = Vec::with_capacity(32 * 5);
    enc.extend_from_slice(&type_hash);
    enc.extend_from_slice(&keccak(b"Exchange"));
    enc.extend_from_slice(&keccak(b"1"));
    enc.extend_from_slice(&word(&1337u64.to_be_bytes()));
    enc.extend_from_slice(&[0u8; 32]); // address(0)
    keccak(&enc)
}

/// struct hash of Agent(string source, bytes32 connectionId)
fn agent_struct_hash(source: &str, connection_id: &[u8; 32]) -> [u8; 32] {
    let type_hash = keccak(b"Agent(string source,bytes32 connectionId)");
    let mut enc = Vec::with_capacity(32 * 3);
    enc.extend_from_slice(&type_hash);
    enc.extend_from_slice(&keccak(source.as_bytes()));
    enc.extend_from_slice(connection_id);
    keccak(&enc)
}

fn eip712_digest(domain_sep: &[u8; 32], struct_hash: &[u8; 32]) -> [u8; 32] {
    let mut buf = [0u8; 66];
    buf[0] = 0x19;
    buf[1] = 0x01;
    buf[2..34].copy_from_slice(domain_sep);
    buf[34..66].copy_from_slice(struct_hash);
    keccak(&buf)
}

/// Returns 65-byte r||s||v signature with v in {27,28}
fn sign_digest(sk: &SigningKey, digest: &[u8; 32]) -> [u8; 65] {
    let (sig, recid): (EcdsaSig, RecoveryId) =
        sk.sign_prehash_recoverable(digest).expect("sign");
    // low-s normalization is enforced by k256's sign_prehash_recoverable
    let mut out = [0u8; 65];
    out[..64].copy_from_slice(&sig.to_bytes());
    out[64] = 27 + recid.to_byte();
    out
}

/// action msgpack + nonce(be u64) + vault flag  -> keccak256 = connectionId
fn action_hash<T: serde::Serialize>(action: &T, nonce: u64, vault: Option<[u8; 20]>) -> [u8; 32] {
    let mut bytes = rmp_serde::to_vec_named(action).expect("msgpack");
    bytes.extend_from_slice(&nonce.to_be_bytes());
    match vault {
        Some(v) => { bytes.push(1); bytes.extend_from_slice(&v); }
        None => bytes.push(0),
    }
    keccak(&bytes)
}

fn main() {
    // ---- Test vector from hyperliquid-rust-sdk src/signature/create_signature.rs ----
    let pk = hex::decode("e908f86dbb4d55ac876378565aafeabc187f6690f046459397b17d9b9a19688e").unwrap();
    let sk = SigningKey::from_slice(&pk).unwrap();

    let mut cid = [0u8; 32];
    cid.copy_from_slice(&hex::decode("de6c4037798a4434ca03cd05f00e3b803126221375cd1e7eaaaf041768be06eb").unwrap());

    let dom = l1_domain_separator();

    for (is_mainnet, expected) in [
        (true,  "fa8a41f6a3fa728206df80801a83bcbfbab08649cd34d9c0bfba7c7b2f99340f53a00226604567b98a1492803190d65a201d6805e5831b7044f17fd530aec7841c"),
        (false, "1713c0fc661b792a50e8ffdd59b637b1ed172d9a3aa4d801d9d88646710fb74b33959f4d075a7ccbec9f2374a6da21ffa4448d58d0413a0d335775f680a881431c"),
    ] {
        let source = if is_mainnet { "a" } else { "b" };
        let sh = agent_struct_hash(source, &cid);
        let digest = eip712_digest(&dom, &sh);
        let sig = sign_digest(&sk, &digest);
        let got = hex::encode(sig);
        println!("mainnet={:<5} got=0x{}", is_mainnet, got);
        println!("              exp=0x{}", expected);
        println!("              MATCH: {}", got == expected);
    }

    // ---- cross-impl fixture: same order action the python SDK hashed ----
    let action_json = r#"{"type":"order","orders":[{"a":1,"b":true,"p":"1800","s":"0.02","r":false,"t":{"limit":{"tif":"Gtc"}}}],"grouping":"na"}"#;
    let val: serde_json::Value = serde_json::from_str(action_json).unwrap();
    let h_value = action_hash(&val, 1690393044548u64, None);
    println!("\nvia serde_json::Value = 0x{}", hex::encode(h_value));
    println!("  msgpack key order   = {:?}",
        rmp_serde::to_vec_named(&val).map(|b| b.iter().filter(|c| c.is_ascii_alphabetic())
            .map(|&c| c as char).collect::<String>()).unwrap());

    // Correct: explicit structs, declaration order == wire order
    #[derive(serde::Serialize)] struct LimitT { tif: &'static str }
    #[derive(serde::Serialize)] struct OrderT { limit: LimitT }
    #[derive(serde::Serialize)] struct OrderWire {
        a: u32, b: bool, p: &'static str, s: &'static str, r: bool, t: OrderT }
    #[derive(serde::Serialize)] struct OrderAction {
        #[serde(rename = "type")] typ: &'static str,
        orders: Vec<OrderWire>, grouping: &'static str }
    let typed = OrderAction {
        typ: "order",
        orders: vec![OrderWire { a: 1, b: true, p: "1800", s: "0.02", r: false,
                                 t: OrderT { limit: LimitT { tif: "Gtc" } } }],
        grouping: "na" };
    let h = action_hash(&typed, 1690393044548u64, None);
    println!("\naction_hash(order) = 0x{}", hex::encode(h));
    println!("python SDK gave     = 0xb8ed9e5a4ea7b2aebdb09fd794221c2599487f6dc8d3bf8f38656ab295f36120");
    println!("MATCH: {}", hex::encode(h) == "b8ed9e5a4ea7b2aebdb09fd794221c2599487f6dc8d3bf8f38656ab295f36120");

    // ---- signing latency ----
    let sh = agent_struct_hash("a", &cid);
    let digest = eip712_digest(&dom, &sh);
    for _ in 0..1000 { std::hint::black_box(sign_digest(&sk, &digest)); }
    let mut ts: Vec<u128> = Vec::with_capacity(2000);
    for _ in 0..2000 {
        let t0 = std::time::Instant::now();
        std::hint::black_box(action_hash(&typed, 1690393044548u64, None));
        std::hint::black_box(sign_digest(&sk, &digest));
        ts.push(t0.elapsed().as_nanos());
    }
    ts.sort();
    println!("\nrust action_hash+sign: median={}us  p99={}us  min={}us",
        ts[ts.len()/2] as f64/1000.0, ts[(ts.len() as f64*0.99) as usize] as f64/1000.0, ts[0] as f64/1000.0);

    // demonstrate action_hash over a real order action
    #[derive(serde::Serialize)]
    #[serde(rename_all = "camelCase")]
    struct OrderReq { a: u32, b: bool, p: String, s: String, r: bool, t: serde_json::Value }
    println!("\n(action_hash available; msgpack via rmp_serde::to_vec_named)");
    let _ = action_hash(&vec![1u8,2,3], 1690393044548u64, None);
}
