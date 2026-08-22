use thiserror::Error;
pub const SCALE: i64 = 100_000_000;
#[derive(Debug, Error, PartialEq, Eq)]
pub enum DecimalError {
    #[error("empty decimal")]
    Empty,
    #[error("invalid decimal")]
    Invalid,
    #[error("more than 8 fractional places")]
    Precision,
    #[error("out of range")]
    Range,
}
pub fn parse_scaled(input: &str) -> Result<i64, DecimalError> {
    let s = input.trim();
    if s.is_empty() {
        return Err(DecimalError::Empty);
    }
    let (negative, rest) = match s.strip_prefix('-') {
        Some(v) => (true, v),
        None => match s.strip_prefix('+') {
            Some(v) => (false, v),
            None => (false, s),
        },
    };
    let (whole, fraction) = match rest.split_once('.') {
        Some((a, b)) if !b.contains('.') => (a, b),
        Some(_) => return Err(DecimalError::Invalid),
        None => (rest, ""),
    };
    if whole.is_empty() && fraction.is_empty()
        || !whole.bytes().all(|c| c.is_ascii_digit())
        || !fraction.bytes().all(|c| c.is_ascii_digit())
    {
        return Err(DecimalError::Invalid);
    }
    if fraction.len() > 8 {
        return Err(DecimalError::Precision);
    }
    let integer = if whole.is_empty() {
        0
    } else {
        whole.parse::<i64>().map_err(|_| DecimalError::Range)?
    };
    let frac = if fraction.is_empty() {
        0
    } else {
        fraction.parse::<i64>().map_err(|_| DecimalError::Range)?
            * 10_i64.pow((8 - fraction.len()) as u32)
    };
    let total = integer
        .checked_mul(SCALE)
        .and_then(|x| x.checked_add(frac))
        .ok_or(DecimalError::Range)?;
    Ok(if negative && total != 0 {
        -total
    } else {
        total
    })
}
/// Parse a decimal string into an `i64` at 1e8 scale **for display-only statistics
/// fields only** — never for anything that can reach an order (`px`, `sz`). Those must
/// go through [`parse_scaled`], which rejects extra precision rather than losing it.
///
/// The venue sends `funding`, `openInterest`, `dayNtlVlm`, `premium` and `dayBaseVlm`
/// with up to 10 fractional digits (`docs/09-measurements.md` §2) — more precision than
/// parsec's 1e8 scale can hold. Losing the 9th/10th decimal place on a statistics field
/// is meaningless at display resolution, so this entry point rounds half-away-from-zero
/// at the 8th decimal instead of erroring. `parse_scaled`'s strict rejection stays
/// unchanged and is still the only parser used anywhere an order can be reached.
pub fn parse_scaled_stat_rounded(input: &str) -> Result<i64, DecimalError> {
    let s = input.trim();
    if s.is_empty() {
        return Err(DecimalError::Empty);
    }
    let (negative, rest) = match s.strip_prefix('-') {
        Some(v) => (true, v),
        None => match s.strip_prefix('+') {
            Some(v) => (false, v),
            None => (false, s),
        },
    };
    let (whole, fraction) = match rest.split_once('.') {
        Some((a, b)) if !b.contains('.') => (a, b),
        Some(_) => return Err(DecimalError::Invalid),
        None => (rest, ""),
    };
    if whole.is_empty() && fraction.is_empty()
        || !whole.bytes().all(|c| c.is_ascii_digit())
        || !fraction.bytes().all(|c| c.is_ascii_digit())
    {
        return Err(DecimalError::Invalid);
    }
    let mut integer: i64 = if whole.is_empty() {
        0
    } else {
        whole.parse::<i64>().map_err(|_| DecimalError::Range)?
    };
    let kept: &str = if fraction.len() > 8 {
        &fraction[..8]
    } else {
        fraction
    };
    let mut frac: i64 = if kept.is_empty() {
        0
    } else {
        kept.parse::<i64>().map_err(|_| DecimalError::Range)? * 10_i64.pow((8 - kept.len()) as u32)
    };
    // Round half-away-from-zero: only the first excess digit needs inspecting, since
    // any digit >= 5 there means the true value is at or past the halfway point
    // regardless of what follows it.
    if let Some(round_digit) = fraction.as_bytes().get(8) {
        if *round_digit >= b'5' {
            frac += 1;
            if frac == SCALE {
                frac = 0;
                integer = integer.checked_add(1).ok_or(DecimalError::Range)?;
            }
        }
    }
    let total = integer
        .checked_mul(SCALE)
        .and_then(|x| x.checked_add(frac))
        .ok_or(DecimalError::Range)?;
    Ok(if negative && total != 0 {
        -total
    } else {
        total
    })
}
pub fn scaled_to_string(value: i64) -> String {
    let negative = value < 0;
    let magnitude = value.unsigned_abs();
    let whole = magnitude / SCALE as u64;
    let frac = magnitude % SCALE as u64;
    if frac == 0 {
        return format!("{}{}", if negative { "-" } else { "" }, whole);
    }
    let mut f = format!("{:08}", frac);
    while f.ends_with('0') {
        f.pop();
    }
    format!("{}{}.{}", if negative { "-" } else { "" }, whole, f)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn round_trip() {
        for (s, v) in [
            ("113377.0", 11337700000000),
            ("0", 0),
            ("-0", 0),
            ("0.00001", 1000),
            ("-1.2", -120000000),
        ] {
            assert_eq!(parse_scaled(s), Ok(v));
            assert_eq!(parse_scaled(&scaled_to_string(v)), Ok(v));
        }
    }
    #[test]
    fn rejects_truncation() {
        assert_eq!(parse_scaled("0.000000001"), Err(DecimalError::Precision));
    }
    #[test]
    fn stat_rounded_matches_strict_parser_within_8_decimals() {
        for (s, v) in [
            ("113377.0", 11337700000000),
            ("0", 0),
            ("-0", 0),
            ("0.00001", 1000),
            ("-1.2", -120000000),
        ] {
            assert_eq!(parse_scaled_stat_rounded(s), Ok(v));
        }
    }
    #[test]
    fn stat_rounded_handles_the_real_10_decimal_wire_values() {
        // From tests/fixtures/info/ctxs.json's activeAssetCtx entry — real wire data,
        // 10 fractional digits, exactly the case that made `number()` silently
        // collapse funding/OI/volume to zero (docs/09 §2).
        assert_eq!(parse_scaled_stat_rounded("0.0000189066"), Ok(1891));
        assert_eq!(parse_scaled_stat_rounded("6559.18"), Ok(655918000000));
        assert_eq!(
            parse_scaled_stat_rounded("368211.7243199999"),
            Ok(36821172432000)
        );
        assert_eq!(parse_scaled_stat_rounded("0.0010074904"), Ok(100749));
        assert_eq!(parse_scaled_stat_rounded("4255.99"), Ok(425599000000));
    }
    #[test]
    fn stat_rounded_rounds_half_away_from_zero() {
        assert_eq!(parse_scaled_stat_rounded("0.000000005"), Ok(1)); // rounds up to 1e-8
        assert_eq!(parse_scaled_stat_rounded("0.000000004"), Ok(0)); // rounds down
        assert_eq!(parse_scaled_stat_rounded("-0.000000005"), Ok(-1));
        assert_eq!(parse_scaled_stat_rounded("0.999999995"), Ok(100000000)); // carries into whole part
    }
    #[test]
    fn stat_rounded_still_rejects_garbage() {
        assert_eq!(parse_scaled_stat_rounded(""), Err(DecimalError::Empty));
        assert_eq!(
            parse_scaled_stat_rounded("not-a-number"),
            Err(DecimalError::Invalid)
        );
    }
    /// Deterministic fuzz per docs/04 §4 ("Fuzz it") — a small xorshift PRNG generates
    /// varied decimal-ish strings and checks the invariants that must hold no matter
    /// what garbage arrives off the wire: never panics, and every `Ok` round-trips
    /// through `scaled_to_string` back to an equal value.
    #[test]
    fn fuzz_parse_scaled_never_panics_and_round_trips() {
        let mut state: u64 = 0x9E3779B97F4A7C15;
        let mut next = || {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            state
        };
        let alphabet: &[u8] = b"0123456789.-+ ";
        for _ in 0..20_000 {
            let len = (next() % 24) as usize;
            let s: String = (0..len)
                .map(|_| alphabet[(next() % alphabet.len() as u64) as usize] as char)
                .collect();
            if let Ok(v) = parse_scaled(&s) {
                assert_eq!(parse_scaled(&scaled_to_string(v)), Ok(v));
            } // rejection is fine; panicking is not
            let _ = parse_scaled_stat_rounded(&s); // must not panic either
        }
    }
}
