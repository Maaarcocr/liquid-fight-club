//! Minimal arbitrary-precision signed integers.
//!
//! Liquid inherits Ruby's unbounded integers, so arithmetic that overflows
//! `i64` must keep going. This type only implements what the engine needs.
//! Magnitudes are capped (see [`MAX_LIMBS`]) so hostile templates cannot
//! exhaust memory by repeatedly squaring a number.

use std::cmp::Ordering;

/// Upper bound on the number of 32-bit limbs (~9.8k decimal digits).
pub const MAX_LIMBS: usize = 1024;

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct BigInt {
    neg: bool,
    /// Little-endian base 2^32 limbs with no trailing zero limbs.
    mag: Vec<u32>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TooLarge;

impl BigInt {
    pub fn zero() -> BigInt {
        BigInt { neg: false, mag: Vec::new() }
    }

    pub fn from_i64(v: i64) -> BigInt {
        Self::from_i128(v as i128)
    }

    pub fn from_i128(v: i128) -> BigInt {
        let neg = v < 0;
        let mut m = v.unsigned_abs();
        let mut mag = Vec::new();
        while m > 0 {
            mag.push(m as u32);
            m >>= 32;
        }
        BigInt { neg, mag }
    }

    pub fn from_u128(mut m: u128) -> BigInt {
        let mut mag = Vec::new();
        while m > 0 {
            mag.push(m as u32);
            m >>= 32;
        }
        BigInt { neg: false, mag }
    }

    pub fn is_zero(&self) -> bool {
        self.mag.is_empty()
    }

    pub fn is_negative(&self) -> bool {
        self.neg
    }

    fn trim(mut self) -> BigInt {
        while self.mag.last() == Some(&0) {
            self.mag.pop();
        }
        if self.mag.is_empty() {
            self.neg = false;
        }
        self
    }

    fn checked(self) -> Result<BigInt, TooLarge> {
        if self.mag.len() > MAX_LIMBS { Err(TooLarge) } else { Ok(self) }
    }

    /// Parses an optional sign followed by ASCII digits.
    pub fn parse(s: &str) -> Option<BigInt> {
        let (neg, digits) = match s.as_bytes().first() {
            Some(b'-') => (true, &s[1..]),
            Some(b'+') => (false, &s[1..]),
            _ => (false, s),
        };
        if digits.is_empty() || !digits.bytes().all(|b| b.is_ascii_digit()) {
            return None;
        }
        if digits.len() > MAX_LIMBS * 9 {
            return None;
        }
        let mut mag: Vec<u32> = Vec::new();
        for chunk in digits.as_bytes().chunks(9) {
            let mut v: u64 = 0;
            let mut mul: u64 = 1;
            for &b in chunk {
                v = v * 10 + (b - b'0') as u64;
                mul *= 10;
            }
            // mag = mag * mul + v
            let mut carry = v;
            for limb in mag.iter_mut() {
                let t = (*limb as u64) * mul + carry;
                *limb = t as u32;
                carry = t >> 32;
            }
            while carry > 0 {
                mag.push(carry as u32);
                carry >>= 32;
            }
        }
        Some(BigInt { neg, mag }.trim())
    }

    pub fn to_i64(&self) -> Option<i64> {
        if self.mag.len() > 2 {
            return None;
        }
        let mut m: u128 = 0;
        for (i, &l) in self.mag.iter().enumerate() {
            m |= (l as u128) << (32 * i);
        }
        let v: i128 = if self.neg { -(m as i128) } else { m as i128 };
        i64::try_from(v).ok()
    }

    pub fn to_f64(&self) -> f64 {
        // Go through the decimal string so rounding is correct.
        self.to_string().parse::<f64>().unwrap_or(if self.neg { f64::NEG_INFINITY } else { f64::INFINITY })
    }

    pub fn neg(&self) -> BigInt {
        BigInt { neg: !self.neg, mag: self.mag.clone() }.trim()
    }

    pub fn abs(&self) -> BigInt {
        BigInt { neg: false, mag: self.mag.clone() }
    }

    fn cmp_mag(a: &[u32], b: &[u32]) -> Ordering {
        if a.len() != b.len() {
            return a.len().cmp(&b.len());
        }
        for i in (0..a.len()).rev() {
            if a[i] != b[i] {
                return a[i].cmp(&b[i]);
            }
        }
        Ordering::Equal
    }

    fn add_mag(a: &[u32], b: &[u32]) -> Vec<u32> {
        let (a, b) = if a.len() >= b.len() { (a, b) } else { (b, a) };
        let mut out = Vec::with_capacity(a.len() + 1);
        let mut carry = 0u64;
        for i in 0..a.len() {
            let t = a[i] as u64 + *b.get(i).unwrap_or(&0) as u64 + carry;
            out.push(t as u32);
            carry = t >> 32;
        }
        if carry > 0 {
            out.push(carry as u32);
        }
        out
    }

    /// a - b where |a| >= |b|.
    fn sub_mag(a: &[u32], b: &[u32]) -> Vec<u32> {
        let mut out = Vec::with_capacity(a.len());
        let mut borrow = 0i64;
        for i in 0..a.len() {
            let mut t = a[i] as i64 - *b.get(i).unwrap_or(&0) as i64 - borrow;
            if t < 0 {
                t += 1 << 32;
                borrow = 1;
            } else {
                borrow = 0;
            }
            out.push(t as u32);
        }
        out
    }

    pub fn add(&self, other: &BigInt) -> Result<BigInt, TooLarge> {
        let r = if self.neg == other.neg {
            BigInt { neg: self.neg, mag: Self::add_mag(&self.mag, &other.mag) }
        } else {
            match Self::cmp_mag(&self.mag, &other.mag) {
                Ordering::Equal => BigInt::zero(),
                Ordering::Greater => BigInt { neg: self.neg, mag: Self::sub_mag(&self.mag, &other.mag) },
                Ordering::Less => BigInt { neg: other.neg, mag: Self::sub_mag(&other.mag, &self.mag) },
            }
        };
        r.trim().checked()
    }

    pub fn sub(&self, other: &BigInt) -> Result<BigInt, TooLarge> {
        self.add(&other.neg())
    }

    pub fn mul(&self, other: &BigInt) -> Result<BigInt, TooLarge> {
        if self.is_zero() || other.is_zero() {
            return Ok(BigInt::zero());
        }
        if self.mag.len() + other.mag.len() > MAX_LIMBS + 1 {
            return Err(TooLarge);
        }
        crate::budget::charge((self.mag.len() * other.mag.len()) as u64 / 16);
        let mut out = vec![0u32; self.mag.len() + other.mag.len()];
        for (i, &a) in self.mag.iter().enumerate() {
            let mut carry = 0u64;
            for (j, &b) in other.mag.iter().enumerate() {
                let t = out[i + j] as u64 + (a as u64) * (b as u64) + carry;
                out[i + j] = t as u32;
                carry = t >> 32;
            }
            let mut k = i + other.mag.len();
            while carry > 0 {
                let t = out[k] as u64 + carry;
                out[k] = t as u32;
                carry = t >> 32;
                k += 1;
            }
        }
        BigInt { neg: self.neg != other.neg, mag: out }.trim().checked()
    }

    /// Divides the magnitude by a small number, returning (quotient, remainder).
    fn divmod_small(mag: &[u32], d: u32) -> (Vec<u32>, u32) {
        let mut out = vec![0u32; mag.len()];
        let mut rem = 0u64;
        for i in (0..mag.len()).rev() {
            let cur = (rem << 32) | mag[i] as u64;
            out[i] = (cur / d as u64) as u32;
            rem = cur % d as u64;
        }
        (out, rem as u32)
    }

    /// Truncating division of magnitudes (schoolbook, bit by bit for simplicity on
    /// multi-limb divisors). Returns (quotient, remainder).
    fn divmod_mag(a: &[u32], b: &[u32]) -> (Vec<u32>, Vec<u32>) {
        if Self::cmp_mag(a, b) == Ordering::Less {
            return (Vec::new(), a.to_vec());
        }
        if b.len() == 1 {
            let (q, r) = Self::divmod_small(a, b[0]);
            return (q, if r == 0 { Vec::new() } else { vec![r] });
        }
        // Long division, one bit at a time. Inputs are bounded, so this is fine.
        let total_bits = a.len() * 32;
        crate::budget::charge((total_bits * b.len()) as u64 / 16);
        let mut q = vec![0u32; a.len()];
        let mut r: Vec<u32> = Vec::new();
        for bit in (0..total_bits).rev() {
            // r = r << 1 | bit
            let mut carry = (a[bit / 32] >> (bit % 32)) & 1;
            for limb in r.iter_mut() {
                let nc = *limb >> 31;
                *limb = (*limb << 1) | carry;
                carry = nc;
            }
            if carry > 0 {
                r.push(carry);
            }
            if Self::cmp_mag(&r, b) != Ordering::Less {
                r = Self::sub_mag(&r, b);
                while r.last() == Some(&0) {
                    r.pop();
                }
                q[bit / 32] |= 1 << (bit % 32);
            }
        }
        (q, r)
    }

    /// Floored division and modulo (Ruby semantics). Returns None on division by zero.
    pub fn div_mod_floor(&self, other: &BigInt) -> Option<(BigInt, BigInt)> {
        if other.is_zero() {
            return None;
        }
        let (q, r) = Self::divmod_mag(&self.mag, &other.mag);
        let mut q = BigInt { neg: self.neg != other.neg, mag: q }.trim();
        let mut r = BigInt { neg: self.neg, mag: r }.trim();
        if !r.is_zero() && (r.neg != other.neg) {
            q = q.sub(&BigInt::from_i64(1)).ok()?;
            r = r.add(other).ok()?;
        }
        Some((q, r))
    }

    /// Truncating division (rounds toward zero).
    pub fn div_trunc(&self, other: &BigInt) -> Option<(BigInt, BigInt)> {
        if other.is_zero() {
            return None;
        }
        let (q, r) = Self::divmod_mag(&self.mag, &other.mag);
        Some((BigInt { neg: self.neg != other.neg, mag: q }.trim(), BigInt { neg: self.neg, mag: r }.trim()))
    }

    pub fn pow10(n: u32) -> Result<BigInt, TooLarge> {
        let mut r = BigInt::from_i64(1);
        let ten9 = BigInt::from_i64(1_000_000_000);
        let mut left = n;
        while left >= 9 {
            r = r.mul(&ten9)?;
            left -= 9;
        }
        if left > 0 {
            r = r.mul(&BigInt::from_i64(10i64.pow(left)))?;
        }
        Ok(r)
    }

    pub fn is_even(&self) -> bool {
        self.mag.first().is_none_or(|l| l & 1 == 0)
    }

    /// Number of decimal digits of the magnitude (0 has 1 digit).
    pub fn decimal_digits(&self) -> usize {
        let s = self.abs().to_string();
        s.len()
    }

    /// Bit length of the magnitude (like Ruby's Integer#bit_length for positives).
    pub fn bit_length(&self) -> u64 {
        match self.mag.last() {
            None => 0,
            Some(&top) => (self.mag.len() as u64 - 1) * 32 + (32 - top.leading_zeros() as u64),
        }
    }
}

impl std::fmt::Display for BigInt {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.mag.is_empty() {
            return f.write_str("0");
        }
        let mut chunks = Vec::new();
        let mut cur = self.mag.clone();
        crate::budget::charge((self.mag.len() * self.mag.len()) as u64 / 16);
        while !cur.is_empty() {
            let (q, r) = Self::divmod_small(&cur, 1_000_000_000);
            chunks.push(r);
            cur = q;
            while cur.last() == Some(&0) {
                cur.pop();
            }
        }
        let mut s = String::with_capacity(chunks.len() * 9 + 1);
        if self.neg {
            s.push('-');
        }
        s.push_str(&chunks.last().unwrap().to_string());
        for c in chunks.iter().rev().skip(1) {
            s.push_str(&format!("{:09}", c));
        }
        f.write_str(&s)
    }
}

impl PartialOrd for BigInt {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for BigInt {
    fn cmp(&self, other: &Self) -> Ordering {
        match (self.neg, other.neg) {
            (false, true) => Ordering::Greater,
            (true, false) => Ordering::Less,
            (false, false) => Self::cmp_mag(&self.mag, &other.mag),
            (true, true) => Self::cmp_mag(&other.mag, &self.mag),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_and_print() {
        let s = "123456789012345678901234567893";
        assert_eq!(BigInt::parse(s).unwrap().to_string(), s);
        assert_eq!(BigInt::parse("-18446744073709551616").unwrap().to_string(), "-18446744073709551616");
        assert_eq!(BigInt::parse("0").unwrap().to_string(), "0");
    }

    #[test]
    fn arithmetic() {
        let a = BigInt::parse("123456789012345678901234567893").unwrap();
        let one = BigInt::from_i64(1);
        assert_eq!(a.add(&one).unwrap().to_string(), "123456789012345678901234567894");
        let b = BigInt::parse("9223372036854775808").unwrap();
        assert_eq!(b.mul(&b).unwrap().to_string(), "85070591730234615865843651857942052864");
        let (q, r) = BigInt::from_i64(-7).div_mod_floor(&BigInt::from_i64(2)).unwrap();
        assert_eq!((q.to_string(), r.to_string()), ("-4".into(), "1".into()));
        let (q, r) = a.div_mod_floor(&b).unwrap();
        assert_eq!(q.mul(&b).unwrap().add(&r).unwrap(), a);
    }
}
