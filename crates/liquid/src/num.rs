//! Ruby numeric semantics: string-to-number parsing, integer arithmetic with
//! overflow into big integers, and a small BigDecimal emulation (Liquid's
//! math filters convert floats to BigDecimal before operating on them).

use crate::bigint::{BigInt, TooLarge};
use crate::error::{Error, ErrorKind, Result};
use crate::value::{format_float, Value};
use std::cmp::Ordering;

pub fn float_to_i64_saturating(f: f64) -> i64 {
    if f.is_nan() {
        0
    } else {
        f.trunc() as i64
    }
}

pub fn clamp_bigint(b: &BigInt) -> i64 {
    b.to_i64().unwrap_or(if b.is_negative() { i64::MIN } else { i64::MAX })
}

fn is_ruby_space(b: u8) -> bool {
    matches!(b, b' ' | b'\t' | b'\n' | 0x0b | 0x0c | b'\r')
}

/// Parses digits with single underscores between them, starting at `i`.
/// Returns (digits, end index).
fn scan_digits(b: &[u8], mut i: usize, radix: u32) -> (String, usize) {
    let mut out = String::new();
    while i < b.len() {
        let c = b[i];
        if (c as char).is_digit(radix) {
            out.push(c as char);
            i += 1;
        } else if c == b'_' && !out.is_empty() && i + 1 < b.len() && (b[i + 1] as char).is_digit(radix) {
            i += 1;
        } else {
            break;
        }
    }
    (out, i)
}

/// Ruby `String#to_i`.
pub fn ruby_str_to_i(s: &str) -> BigInt {
    let b = s.as_bytes();
    let mut i = 0;
    while i < b.len() && is_ruby_space(b[i]) {
        i += 1;
    }
    let mut neg = false;
    if i < b.len() && (b[i] == b'-' || b[i] == b'+') {
        neg = b[i] == b'-';
        i += 1;
    }
    let (digits, _) = scan_digits(b, i, 10);
    if digits.is_empty() {
        return BigInt::zero();
    }
    let v = BigInt::parse(&digits).unwrap_or_else(BigInt::zero);
    if neg { v.neg() } else { v }
}

/// Ruby `String#to_i` clamped to i64.
pub fn ruby_str_to_i64(s: &str) -> i64 {
    clamp_bigint(&ruby_str_to_i(s))
}

/// Ruby `String#to_f`.
pub fn ruby_str_to_f(s: &str) -> f64 {
    let b = s.as_bytes();
    let mut i = 0;
    while i < b.len() && is_ruby_space(b[i]) {
        i += 1;
    }
    let mut text = String::new();
    if i < b.len() && (b[i] == b'-' || b[i] == b'+') {
        if b[i] == b'-' {
            text.push('-');
        }
        i += 1;
    }
    let (int, j) = scan_digits(b, i, 10);
    i = j;
    if int.is_empty() {
        // Ruby accepts ".5" as 0.5.
        if i + 1 < b.len() && b[i] == b'.' && b[i + 1].is_ascii_digit() {
            text.push('0');
        } else {
            return if text.starts_with('-') { -0.0 } else { 0.0 };
        }
    }
    text.push_str(&int);
    if i + 1 < b.len() && b[i] == b'.' && b[i + 1].is_ascii_digit() {
        let (frac, j) = scan_digits(b, i + 1, 10);
        text.push('.');
        text.push_str(&frac);
        i = j;
    }
    if i + 1 < b.len() && (b[i] == b'e' || b[i] == b'E') {
        let mut k = i + 1;
        let mut exp = String::from("e");
        if k < b.len() && (b[k] == b'-' || b[k] == b'+') {
            exp.push(b[k] as char);
            k += 1;
        }
        let (ed, _) = scan_digits(b, k, 10);
        if !ed.is_empty() {
            exp.push_str(&ed);
            text.push_str(&exp);
        }
    }
    text.parse::<f64>().unwrap_or(0.0)
}

/// Ruby `Kernel#Integer(string)`; None when the string is not an integer.
pub fn ruby_integer(s: &str) -> Option<BigInt> {
    let b = s.as_bytes();
    let mut i = 0;
    while i < b.len() && is_ruby_space(b[i]) {
        i += 1;
    }
    let mut end = b.len();
    while end > i && (is_ruby_space(b[end - 1]) || b[end - 1] == 0) {
        end -= 1;
    }
    let b = &b[..end];
    let mut neg = false;
    if i < b.len() && (b[i] == b'-' || b[i] == b'+') {
        neg = b[i] == b'-';
        i += 1;
    }
    let mut radix = 10;
    if i + 1 < b.len() && b[i] == b'0' {
        match b[i + 1] {
            b'x' | b'X' => {
                radix = 16;
                i += 2;
            }
            b'b' | b'B' => {
                radix = 2;
                i += 2;
            }
            b'o' | b'O' => {
                radix = 8;
                i += 2;
            }
            b'd' | b'D' => {
                radix = 10;
                i += 2;
            }
            b'0'..=b'9' | b'_' => {
                radix = 8;
                i += 1;
            }
            _ => {}
        }
    }
    let (digits, j) = scan_digits(b, i, radix);
    if digits.is_empty() || j != b.len() {
        // A lone "0" (or "0_0") is fine in octal mode.
        if radix == 8 && digits.is_empty() && j == b.len() && i > 0 && b[i - 1] == b'0' {
            return Some(BigInt::zero());
        }
        return None;
    }
    let mut v = BigInt::zero();
    let r = BigInt::from_i64(radix as i64);
    for c in digits.chars() {
        let d = c.to_digit(radix)? as i64;
        v = v.mul(&r).ok()?.add(&BigInt::from_i64(d)).ok()?;
    }
    Some(if neg { v.neg() } else { v })
}

// ---------------------------------------------------------------------------
// Decimal (BigDecimal emulation)
// ---------------------------------------------------------------------------

#[derive(Clone, Debug)]
pub enum Dec {
    /// mant * 10^exp
    Fin(BigInt, i64),
    Inf(bool),
    NaN,
}

const DIV_DIGITS: usize = 40;

fn too_large(_: TooLarge) -> Error {
    Error::memory()
}

impl Dec {
    pub fn from_int(i: &BigInt) -> Dec {
        Dec::Fin(i.clone(), 0)
    }

    /// Parses `[-]digits[.digits][e[+-]digits]`, "Infinity", "-Infinity", "NaN".
    pub fn parse(s: &str) -> Option<Dec> {
        let s = s.trim();
        match s {
            "Infinity" | "+Infinity" => return Some(Dec::Inf(false)),
            "-Infinity" => return Some(Dec::Inf(true)),
            "NaN" => return Some(Dec::NaN),
            _ => {}
        }
        let (mant_part, exp_part) = match s.find(['e', 'E']) {
            Some(p) => (&s[..p], Some(&s[p + 1..])),
            None => (s, None),
        };
        let (neg, mant_part) = match mant_part.as_bytes().first() {
            Some(b'-') => (true, &mant_part[1..]),
            Some(b'+') => (false, &mant_part[1..]),
            _ => (false, mant_part),
        };
        let (ip, fp) = match mant_part.find('.') {
            Some(p) => (&mant_part[..p], &mant_part[p + 1..]),
            None => (mant_part, ""),
        };
        let digits: String = format!("{}{}", ip, fp).chars().filter(|c| *c != '_').collect();
        if digits.is_empty() || !digits.bytes().all(|c| c.is_ascii_digit()) {
            return None;
        }
        let mut exp: i64 = -(fp.chars().filter(|c| *c != '_').count() as i64);
        if let Some(e) = exp_part {
            exp += e.parse::<i64>().ok()?;
        }
        let m = BigInt::parse(&digits)?;
        Some(Dec::Fin(if neg { m.neg() } else { m }, exp))
    }

    /// `BigDecimal(float.to_s)`.
    pub fn from_f64(f: f64) -> Dec {
        Dec::parse(&format_float(f)).unwrap_or(Dec::NaN)
    }

    pub fn is_zero(&self) -> bool {
        matches!(self, Dec::Fin(m, _) if m.is_zero())
    }

    fn sign_neg(&self) -> bool {
        match self {
            Dec::Fin(m, _) => m.is_negative(),
            Dec::Inf(n) => *n,
            Dec::NaN => false,
        }
    }

    fn align(a: &BigInt, ae: i64, b: &BigInt, be: i64) -> Result<(BigInt, BigInt, i64)> {
        if ae == be {
            return Ok((a.clone(), b.clone(), ae));
        }
        if ae > be {
            let d = ae - be;
            if d > 100_000 {
                return Err(Error::memory());
            }
            Ok((a.mul(&BigInt::pow10(d as u32).map_err(too_large)?).map_err(too_large)?, b.clone(), be))
        } else {
            let d = be - ae;
            if d > 100_000 {
                return Err(Error::memory());
            }
            Ok((a.clone(), b.mul(&BigInt::pow10(d as u32).map_err(too_large)?).map_err(too_large)?, ae))
        }
    }

    pub fn add(&self, o: &Dec) -> Result<Dec> {
        Ok(match (self, o) {
            (Dec::NaN, _) | (_, Dec::NaN) => Dec::NaN,
            (Dec::Inf(a), Dec::Inf(b)) => {
                if a == b {
                    Dec::Inf(*a)
                } else {
                    Dec::NaN
                }
            }
            (Dec::Inf(a), _) | (_, Dec::Inf(a)) => Dec::Inf(*a),
            (Dec::Fin(a, ae), Dec::Fin(b, be)) => {
                let (x, y, e) = Self::align(a, *ae, b, *be)?;
                Dec::Fin(x.add(&y).map_err(too_large)?, e)
            }
        })
    }

    pub fn neg(&self) -> Dec {
        match self {
            Dec::Fin(m, e) => Dec::Fin(m.neg(), *e),
            Dec::Inf(n) => Dec::Inf(!n),
            Dec::NaN => Dec::NaN,
        }
    }

    pub fn sub(&self, o: &Dec) -> Result<Dec> {
        self.add(&o.neg())
    }

    pub fn mul(&self, o: &Dec) -> Result<Dec> {
        Ok(match (self, o) {
            (Dec::NaN, _) | (_, Dec::NaN) => Dec::NaN,
            (Dec::Inf(a), x) | (x, Dec::Inf(a)) => {
                if x.is_zero() {
                    Dec::NaN
                } else {
                    Dec::Inf(*a != x.sign_neg())
                }
            }
            (Dec::Fin(a, ae), Dec::Fin(b, be)) => Dec::Fin(a.mul(b).map_err(too_large)?, ae + be),
        })
    }

    pub fn div(&self, o: &Dec) -> Result<Dec> {
        Ok(match (self, o) {
            (Dec::NaN, _) | (_, Dec::NaN) => Dec::NaN,
            (Dec::Inf(_), Dec::Inf(_)) => Dec::NaN,
            (Dec::Inf(a), b) => Dec::Inf(*a != b.sign_neg()),
            (Dec::Fin(..), Dec::Inf(_)) => Dec::Fin(BigInt::zero(), 0),
            (Dec::Fin(a, ae), Dec::Fin(b, be)) => {
                if b.is_zero() {
                    if a.is_zero() {
                        return Ok(Dec::NaN);
                    }
                    return Ok(Dec::Inf(a.is_negative()));
                }
                if a.is_zero() {
                    return Ok(Dec::Fin(BigInt::zero(), 0));
                }
                let da = a.decimal_digits() as i64;
                let db = b.decimal_digits() as i64;
                let k = (DIV_DIGITS as i64 + db - da).max(0);
                let scaled = a.mul(&BigInt::pow10(k as u32).map_err(too_large)?).map_err(too_large)?;
                let (q, r) = scaled.div_trunc(b).ok_or_else(Error::memory)?;
                // Round half away from zero.
                let twice = r.abs().mul(&BigInt::from_i64(2)).map_err(too_large)?;
                let q = if twice >= b.abs() {
                    let one = BigInt::from_i64(if q.is_negative() || (q.is_zero() && (a.is_negative() != b.is_negative())) {
                        -1
                    } else {
                        1
                    });
                    q.add(&one).map_err(too_large)?
                } else {
                    q
                };
                Dec::Fin(q, ae - be - k)
            }
        })
    }

    /// Floored modulo (`BigDecimal#%`). Errors on division by zero.
    pub fn modulo(&self, o: &Dec) -> Result<Dec> {
        match (self, o) {
            (_, Dec::Fin(b, _)) if b.is_zero() => Err(Error::new(ErrorKind::ZeroDivision, "divided by 0")),
            (Dec::NaN, _) | (_, Dec::NaN) | (Dec::Inf(_), _) => Ok(Dec::NaN),
            (Dec::Fin(..), Dec::Inf(n)) => {
                // x % Inf: x if same sign (or zero), else Inf with divisor sign.
                if self.is_zero() || self.sign_neg() == *n {
                    Ok(self.clone())
                } else {
                    Ok(Dec::Inf(*n))
                }
            }
            (Dec::Fin(a, ae), Dec::Fin(b, be)) => {
                let (x, y, e) = Self::align(a, *ae, b, *be)?;
                let (_, r) = x.div_mod_floor(&y).ok_or_else(Error::memory)?;
                Ok(Dec::Fin(r, e))
            }
        }
    }

    pub fn cmp(&self, o: &Dec) -> Option<Ordering> {
        match (self, o) {
            (Dec::NaN, _) | (_, Dec::NaN) => None,
            (Dec::Inf(a), Dec::Inf(b)) => Some(b.cmp(a)),
            (Dec::Inf(a), _) => Some(if *a { Ordering::Less } else { Ordering::Greater }),
            (_, Dec::Inf(b)) => Some(if *b { Ordering::Greater } else { Ordering::Less }),
            (Dec::Fin(a, ae), Dec::Fin(b, be)) => {
                let (x, y, _) = Self::align(a, *ae, b, *be).ok()?;
                Some(x.cmp(&y))
            }
        }
    }

    pub fn to_f64(&self) -> f64 {
        match self {
            Dec::NaN => f64::NAN,
            Dec::Inf(n) => {
                if *n {
                    f64::NEG_INFINITY
                } else {
                    f64::INFINITY
                }
            }
            Dec::Fin(m, e) => {
                if m.is_zero() {
                    return 0.0;
                }
                format!("{}e{}", m, e).parse::<f64>().unwrap_or(f64::NAN)
            }
        }
    }

    pub fn abs(&self) -> Dec {
        match self {
            Dec::Fin(m, e) => Dec::Fin(m.abs(), *e),
            Dec::Inf(_) => Dec::Inf(false),
            Dec::NaN => Dec::NaN,
        }
    }

    fn non_finite_error(&self) -> Error {
        let what = match self {
            Dec::NaN => "NaN",
            Dec::Inf(true) => "-Infinity",
            _ => "Infinity",
        };
        Error::new(ErrorKind::FloatDomain, format!("Computation results in '{}'", what))
    }

    /// Integer part rounded with the given mode: -1 floor, 1 ceil, 0 half-up.
    fn to_integer_mode(&self, mode: i8) -> Result<BigInt> {
        match self {
            Dec::Fin(m, e) => {
                if *e >= 0 {
                    if *e > 100_000 {
                        return Err(Error::memory());
                    }
                    return m.mul(&BigInt::pow10(*e as u32).map_err(too_large)?).map_err(too_large);
                }
                let d = -*e;
                if d > 100_000 {
                    return Ok(match mode {
                        -1 if m.is_negative() => BigInt::from_i64(-1),
                        1 if !m.is_negative() && !m.is_zero() => BigInt::from_i64(1),
                        _ => BigInt::zero(),
                    });
                }
                let p = BigInt::pow10(d as u32).map_err(too_large)?;
                let (q, r) = m.div_trunc(&p).ok_or_else(Error::memory)?;
                if r.is_zero() {
                    return Ok(q);
                }
                let one = BigInt::from_i64(1);
                Ok(match mode {
                    -1 => {
                        if m.is_negative() {
                            q.sub(&one).map_err(too_large)?
                        } else {
                            q
                        }
                    }
                    1 => {
                        if m.is_negative() {
                            q
                        } else {
                            q.add(&one).map_err(too_large)?
                        }
                    }
                    _ => {
                        let twice = r.abs().mul(&BigInt::from_i64(2)).map_err(too_large)?;
                        if twice >= p {
                            if m.is_negative() {
                                q.sub(&one).map_err(too_large)?
                            } else {
                                q.add(&one).map_err(too_large)?
                            }
                        } else {
                            q
                        }
                    }
                })
            }
            _ => Err(self.non_finite_error()),
        }
    }

    pub fn floor(&self) -> Result<BigInt> {
        self.to_integer_mode(-1)
    }

    pub fn ceil(&self) -> Result<BigInt> {
        self.to_integer_mode(1)
    }

    /// `round(n)` half away from zero. Returns an integer for n <= 0 (like
    /// BigDecimal#round(0)), else a decimal.
    pub fn round(&self, n: i64) -> Result<Num> {
        match self {
            Dec::Fin(..) => {}
            _ => {
                if n <= 0 {
                    return Err(self.non_finite_error());
                }
                return Ok(Num::Dec(self.clone()));
            }
        }
        if n == 0 {
            return Ok(Num::from_bigint(self.to_integer_mode(0)?));
        }
        let n = n.clamp(-100_000, 100_000);
        // Shift so that rounding to an integer rounds at the requested digit.
        let shifted = match self {
            Dec::Fin(m, e) => Dec::Fin(m.clone(), e + n),
            _ => unreachable!(),
        };
        let r = shifted.to_integer_mode(0)?;
        let d = Dec::Fin(r, -n);
        if n < 0 {
            return Ok(Num::from_bigint(d.to_integer_mode(0)?));
        }
        Ok(Num::Dec(d))
    }
}

// ---------------------------------------------------------------------------
// Num: the result of Utils.to_number
// ---------------------------------------------------------------------------

#[derive(Clone, Debug)]
pub enum Num {
    Int(i64),
    Big(BigInt),
    Dec(Dec),
}

impl Num {
    pub fn from_bigint(b: BigInt) -> Num {
        match b.to_i64() {
            Some(i) => Num::Int(i),
            None => Num::Big(b),
        }
    }

    fn to_big(&self) -> Option<BigInt> {
        match self {
            Num::Int(i) => Some(BigInt::from_i64(*i)),
            Num::Big(b) => Some(b.clone()),
            Num::Dec(_) => None,
        }
    }

    pub fn to_dec(&self) -> Dec {
        match self {
            Num::Int(i) => Dec::Fin(BigInt::from_i64(*i), 0),
            Num::Big(b) => Dec::Fin(b.clone(), 0),
            Num::Dec(d) => d.clone(),
        }
    }

    pub fn is_dec(&self) -> bool {
        matches!(self, Num::Dec(_))
    }

    /// Converts to a Liquid value (BigDecimal results become floats, as the
    /// filters do with `to_f`).
    pub fn into_value(self) -> Value {
        match self {
            Num::Int(i) => Value::Int(i),
            Num::Big(b) => Value::from_bigint(b),
            Num::Dec(d) => Value::Float(d.to_f64()),
        }
    }

    pub fn add(&self, o: &Num) -> Result<Num> {
        if let (Num::Int(a), Num::Int(b)) = (self, o) {
            if let Some(r) = a.checked_add(*b) {
                return Ok(Num::Int(r));
            }
        }
        match (self.to_big(), o.to_big()) {
            (Some(a), Some(b)) => Ok(Num::from_bigint(a.add(&b).map_err(too_large)?)),
            _ => Ok(Num::Dec(self.to_dec().add(&o.to_dec())?)),
        }
    }

    pub fn sub(&self, o: &Num) -> Result<Num> {
        if let (Num::Int(a), Num::Int(b)) = (self, o) {
            if let Some(r) = a.checked_sub(*b) {
                return Ok(Num::Int(r));
            }
        }
        match (self.to_big(), o.to_big()) {
            (Some(a), Some(b)) => Ok(Num::from_bigint(a.sub(&b).map_err(too_large)?)),
            _ => Ok(Num::Dec(self.to_dec().sub(&o.to_dec())?)),
        }
    }

    pub fn mul(&self, o: &Num) -> Result<Num> {
        if let (Num::Int(a), Num::Int(b)) = (self, o) {
            if let Some(r) = a.checked_mul(*b) {
                return Ok(Num::Int(r));
            }
        }
        match (self.to_big(), o.to_big()) {
            (Some(a), Some(b)) => Ok(Num::from_bigint(a.mul(&b).map_err(too_large)?)),
            _ => Ok(Num::Dec(self.to_dec().mul(&o.to_dec())?)),
        }
    }

    pub fn div(&self, o: &Num) -> Result<Num> {
        match (self.to_big(), o.to_big()) {
            (Some(a), Some(b)) => {
                if b.is_zero() {
                    return Err(Error::new(ErrorKind::ZeroDivision, "divided by 0"));
                }
                if let (Num::Int(x), Num::Int(y)) = (self, o) {
                    if let Some(q) = x.checked_div(*y) {
                        // Floor division.
                        let q = if (x % y != 0) && ((*x < 0) != (*y < 0)) { q - 1 } else { q };
                        return Ok(Num::Int(q));
                    }
                }
                let (q, _) = a.div_mod_floor(&b).ok_or_else(Error::memory)?;
                Ok(Num::from_bigint(q))
            }
            _ => Ok(Num::Dec(self.to_dec().div(&o.to_dec())?)),
        }
    }

    pub fn modulo(&self, o: &Num) -> Result<Num> {
        match (self.to_big(), o.to_big()) {
            (Some(a), Some(b)) => {
                if b.is_zero() {
                    return Err(Error::new(ErrorKind::ZeroDivision, "divided by 0"));
                }
                if let (Num::Int(x), Num::Int(y)) = (self, o) {
                    if let Some(r) = x.checked_rem(*y) {
                        let r = if r != 0 && ((r < 0) != (*y < 0)) { r + y } else { r };
                        return Ok(Num::Int(r));
                    }
                }
                let (_, r) = a.div_mod_floor(&b).ok_or_else(Error::memory)?;
                Ok(Num::from_bigint(r))
            }
            _ => Ok(Num::Dec(self.to_dec().modulo(&o.to_dec())?)),
        }
    }

    pub fn cmp(&self, o: &Num) -> Option<Ordering> {
        match (self, o) {
            (Num::Int(a), Num::Int(b)) => Some(a.cmp(b)),
            _ => match (self.to_big(), o.to_big()) {
                (Some(a), Some(b)) => Some(a.cmp(&b)),
                _ => self.to_dec().cmp(&o.to_dec()),
            },
        }
    }

    pub fn abs(&self) -> Result<Num> {
        Ok(match self {
            Num::Int(i) => match i.checked_abs() {
                Some(a) => Num::Int(a),
                None => Num::Big(BigInt::from_i64(*i).abs()),
            },
            Num::Big(b) => Num::Big(b.abs()),
            Num::Dec(d) => Num::Dec(d.abs()),
        })
    }

    pub fn floor(&self) -> Result<Num> {
        match self {
            Num::Dec(d) => Ok(Num::from_bigint(d.floor()?)),
            _ => Ok(self.clone()),
        }
    }

    pub fn ceil(&self) -> Result<Num> {
        match self {
            Num::Dec(d) => Ok(Num::from_bigint(d.ceil()?)),
            _ => Ok(self.clone()),
        }
    }

    /// `round(n)`.
    pub fn round(&self, n: i64) -> Result<Num> {
        match self {
            Num::Dec(d) => d.round(n),
            _ => {
                if n >= 0 {
                    return Ok(self.clone());
                }
                let d = self.to_dec();
                d.round(n)
            }
        }
    }
}

/// `Liquid::Utils.to_number`.
pub fn to_number(v: &Value) -> Num {
    match v {
        Value::Int(i) => Num::Int(*i),
        Value::BigInt(b) => Num::Big((**b).clone()),
        Value::Float(f) => Num::Dec(Dec::from_f64(*f)),
        Value::Str(s) => {
            let t = crate::expression::ruby_strip(s);
            if is_decimal(t) {
                match Dec::parse(t) {
                    Some(d) => Num::Dec(d),
                    None => Num::Int(0),
                }
            } else {
                Num::from_bigint(ruby_str_to_i(s))
            }
        }
        Value::Drop(d) => match d.to_number() {
            Some(n) => to_number(&n),
            None => Num::Int(0),
        },
        _ => Num::Int(0),
    }
}

/// `/\A-?\d+\.\d+\z/`
fn is_decimal(s: &str) -> bool {
    let b = s.as_bytes();
    let mut i = 0;
    if i < b.len() && b[i] == b'-' {
        i += 1;
    }
    let s1 = i;
    while i < b.len() && b[i].is_ascii_digit() {
        i += 1;
    }
    if i == s1 || i >= b.len() || b[i] != b'.' {
        return false;
    }
    i += 1;
    let s2 = i;
    while i < b.len() && b[i].is_ascii_digit() {
        i += 1;
    }
    i > s2 && i == b.len()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn to_i() {
        assert_eq!(ruby_str_to_i("1_000").to_string(), "1000");
        assert_eq!(ruby_str_to_i(" 12abc").to_string(), "12");
        assert_eq!(ruby_str_to_i("-0x1A").to_string(), "0");
        assert_eq!(ruby_str_to_i("1__0").to_string(), "1");
        assert_eq!(ruby_str_to_i("_1").to_string(), "0");
    }

    #[test]
    fn integer() {
        assert_eq!(ruby_integer(" 42 ").unwrap().to_string(), "42");
        assert_eq!(ruby_integer("0x1A").unwrap().to_string(), "26");
        assert_eq!(ruby_integer("012").unwrap().to_string(), "10");
        assert!(ruby_integer("1.5").is_none());
        assert!(ruby_integer("").is_none());
        assert!(ruby_integer("08").is_none());
        assert_eq!(ruby_integer("0").unwrap().to_string(), "0");
    }

    #[test]
    fn decimals() {
        let a = Num::Dec(Dec::from_f64(0.1));
        let b = Num::Dec(Dec::from_f64(0.2));
        assert_eq!(a.add(&b).unwrap().into_value().as_float(), Some(0.3));
        let ten = Num::Dec(Dec::from_f64(10.0));
        assert_eq!(ten.div(&Num::Int(3)).unwrap().into_value().as_float(), Some(10.0 / 3.0));
        assert_eq!(Num::Dec(Dec::from_f64(2.5)).round(0).unwrap().into_value().as_int(), Some(3));
        assert_eq!(Num::Dec(Dec::from_f64(-2.5)).round(0).unwrap().into_value().as_int(), Some(-3));
        assert_eq!(Num::Int(1250).round(-2).unwrap().into_value().as_int(), Some(1300));
        assert_eq!(Num::Dec(Dec::from_f64(2.675)).round(2).unwrap().into_value().as_float(), Some(2.68));
        assert_eq!(Num::Dec(Dec::from_f64(-7.5)).modulo(&Num::Int(2)).unwrap().into_value().as_float(), Some(0.5));
    }
}
