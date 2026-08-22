use serde_json::Value;
pub fn string<'a>(v: &'a Value, key: &str) -> Option<&'a str> {
    v.get(key)?.as_str()
}
pub fn integer(v: &Value, key: &str) -> Option<u64> {
    v.get(key)?
        .as_u64()
        .or_else(|| v.get(key)?.as_str()?.parse().ok())
}
