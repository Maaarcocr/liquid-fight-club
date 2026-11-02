//! Dates for the `date` filter: a subset of Ruby's `Time.parse` and a full
//! `strftime`. Local time is UTC (the reference runs with `TZ=UTC`).

use std::fmt::Write as _;

/// A point in time with a fixed UTC offset.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Timestamp {
    /// Seconds since the Unix epoch.
    pub secs: i64,
    pub nanos: u32,
    /// Offset east of UTC in seconds.
    pub offset: i32,
    /// True for local (UTC) times, whose `%Z` is "UTC"; false for fixed offsets.
    pub utc: bool,
}

impl Timestamp {
    pub fn from_unix(secs: i64) -> Timestamp {
        Timestamp { secs, nanos: 0, offset: 0, utc: true }
    }

    pub fn now() -> Timestamp {
        let d = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default();
        Timestamp { secs: d.as_secs() as i64, nanos: d.subsec_nanos(), offset: 0, utc: true }
    }

    fn local_secs(&self) -> i64 {
        self.secs + self.offset as i64
    }

    fn civil(&self) -> Civil {
        let ls = self.local_secs();
        let days = ls.div_euclid(86400);
        let sod = ls.rem_euclid(86400);
        let (y, m, d) = civil_from_days(days);
        Civil {
            year: y,
            month: m,
            day: d,
            hour: (sod / 3600) as u32,
            min: ((sod % 3600) / 60) as u32,
            sec: (sod % 60) as u32,
            wday: (days + 4).rem_euclid(7) as u32,
            yday: (days - days_from_civil(y, 1, 1)) as u32 + 1,
            days,
        }
    }
}

struct Civil {
    year: i64,
    month: u32,
    day: u32,
    hour: u32,
    min: u32,
    sec: u32,
    wday: u32,
    yday: u32,
    days: i64,
}

/// Days since 1970-01-01 of a proleptic Gregorian date.
pub fn days_from_civil(y: i64, m: u32, d: u32) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = y.div_euclid(400);
    let yoe = y - era * 400;
    let mp = (m as i64 + 9) % 12;
    let doy = (153 * mp + 2) / 5 + d as i64 - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146097 + doe - 719468
}

pub fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719468;
    let era = z.div_euclid(146097);
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (if m <= 2 { y + 1 } else { y }, m, d)
}

const MONTHS: [&str; 12] =
    ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"];
const DAYS: [&str; 7] = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"];

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

#[derive(Default, Debug)]
struct Parts {
    year: Option<i64>,
    mon: Option<i64>,
    mday: Option<i64>,
    hour: Option<i64>,
    min: Option<i64>,
    sec: Option<i64>,
    nanos: u32,
    offset: Option<i32>,
    yday: Option<i64>,
}

fn zone_offset(name: &str) -> Option<i32> {
    let h = |x: i32| Some(x * 3600);
    match name.to_ascii_lowercase().as_str() {
        "utc" | "gmt" | "z" | "ut" => h(0),
        "est" => h(-5),
        "edt" => h(-4),
        "cst" => h(-6),
        "cdt" => h(-5),
        "mst" => h(-7),
        "mdt" => h(-6),
        "pst" => h(-8),
        "pdt" => h(-7),
        "akst" => h(-9),
        "akdt" => h(-8),
        "hst" => h(-10),
        "bst" => h(1),
        "cet" => h(1),
        "cest" => h(2),
        "eet" => h(2),
        "eest" => h(3),
        "wet" => h(0),
        "west" => h(1),
        "jst" => h(9),
        "ist" => Some(19800),
        "aest" => h(10),
        "aedt" => h(11),
        _ => None,
    }
}

fn month_from_name(s: &str) -> Option<i64> {
    let l = s.to_ascii_lowercase();
    if l.len() < 3 {
        return None;
    }
    for (i, m) in MONTHS.iter().enumerate() {
        let ml = m.to_ascii_lowercase();
        if ml.starts_with(&l[..3]) && (l.len() == 3 || ml == l || (l == "sept" && i == 8)) {
            return Some(i as i64 + 1);
        }
    }
    None
}

fn is_weekday_name(s: &str) -> bool {
    let l = s.to_ascii_lowercase();
    l.len() >= 3 && DAYS.iter().any(|d| {
        let dl = d.to_ascii_lowercase();
        dl.starts_with(&l[..3]) && (l.len() == 3 || dl == l)
    })
}

#[derive(Debug, Clone, PartialEq)]
enum Tk {
    Num(String),
    Word(String),
    Sym(char),
}

fn lex(s: &str) -> Vec<Tk> {
    let mut out = Vec::new();
    let cs: Vec<char> = s.chars().collect();
    let mut i = 0;
    while i < cs.len() {
        let c = cs[i];
        if c.is_ascii_digit() {
            let st = i;
            while i < cs.len() && cs[i].is_ascii_digit() {
                i += 1;
            }
            out.push(Tk::Num(cs[st..i].iter().collect()));
        } else if c.is_ascii_alphabetic() {
            let st = i;
            while i < cs.len() && cs[i].is_ascii_alphabetic() {
                i += 1;
            }
            out.push(Tk::Word(cs[st..i].iter().collect()));
        } else if c.is_whitespace() {
            i += 1;
        } else {
            out.push(Tk::Sym(c));
            i += 1;
        }
    }
    out
}

fn expand_year(s: &str) -> i64 {
    let v: i64 = s.parse().unwrap_or(0);
    if s.len() <= 2 {
        if v >= 69 { 1900 + v } else { 2000 + v }
    } else {
        v
    }
}

/// Parses the date/time fields from a string (a pragmatic `Date._parse`).
fn parse_parts(s: &str) -> Option<Parts> {
    let t = lex(s);
    let mut p = Parts::default();
    let mut used = vec![false; t.len()];
    let num = |i: usize| -> Option<&str> {
        match t.get(i) {
            Some(Tk::Num(n)) => Some(n.as_str()),
            _ => None,
        }
    };
    let sym = |i: usize, c: char| t.get(i) == Some(&Tk::Sym(c));
    let n = t.len();

    // Time: H:MM[:SS[.frac]] [am|pm]
    let mut time_end = 0;
    let mut i = 0;
    while i < n {
        if let (Some(h), true, Some(m)) = (num(i), sym(i + 1, ':'), num(i + 2)) {
            if h.len() <= 2 && m.len() == 2 {
                p.hour = Some(h.parse().ok()?);
                p.min = Some(m.parse().ok()?);
                used[i] = true;
                used[i + 1] = true;
                used[i + 2] = true;
                let mut j = i + 3;
                if sym(j, ':') {
                    if let Some(sec) = num(j + 1) {
                        p.sec = Some(sec.parse().ok()?);
                        used[j] = true;
                        used[j + 1] = true;
                        j += 2;
                        if (sym(j, '.') || sym(j, ',')) && num(j + 1).is_some() {
                            let f = num(j + 1).unwrap();
                            let mut digits: String = f.chars().take(9).collect();
                            while digits.len() < 9 {
                                digits.push('0');
                            }
                            p.nanos = digits.parse().unwrap_or(0);
                            used[j] = true;
                            used[j + 1] = true;
                            j += 2;
                        }
                    }
                }
                if let Some(Tk::Word(w)) = t.get(j) {
                    let wl = w.to_ascii_lowercase();
                    if wl == "am" || wl == "pm" {
                        let h = p.hour?;
                        if !(1..=12).contains(&h) {
                            return None;
                        }
                        p.hour = Some(h % 12 + if wl == "pm" { 12 } else { 0 });
                        used[j] = true;
                        j += 1;
                    }
                }
                time_end = j;
                break;
            }
        }
        i += 1;
    }
    if p.hour.is_none() {
        // "3pm"
        for i in 0..n {
            if let (Some(h), Some(Tk::Word(w))) = (num(i), t.get(i + 1)) {
                let wl = w.to_ascii_lowercase();
                if (wl == "am" || wl == "pm") && h.len() <= 2 {
                    let h: i64 = h.parse().ok()?;
                    if !(1..=12).contains(&h) {
                        return None;
                    }
                    p.hour = Some(h % 12 + if wl == "pm" { 12 } else { 0 });
                    used[i] = true;
                    used[i + 1] = true;
                    break;
                }
            }
        }
    }

    // Zone: Z / UTC / names / +hh:mm / +hhmm / +hh (only after something else).
    for i in 0..n {
        if used[i] {
            continue;
        }
        match &t[i] {
            Tk::Word(w) if zone_offset(w).is_some() && (w.len() > 1 || i > 0) => {
                if w.len() == 1 && !w.eq_ignore_ascii_case("z") {
                    continue;
                }
                p.offset = zone_offset(w);
                used[i] = true;
                break;
            }
            Tk::Sym(c @ ('+' | '-')) if i > 0 && p.hour.is_some() && i >= time_end => {
                if let Some(hh) = num(i + 1) {
                    let sign = if *c == '-' { -1 } else { 1 };
                    let (h, m, k) = if hh.len() == 4 {
                        (hh[..2].parse::<i32>().ok()?, hh[2..].parse::<i32>().ok()?, 2)
                    } else if hh.len() <= 2 && sym(i + 2, ':') && num(i + 3).is_some() {
                        (hh.parse().ok()?, num(i + 3).unwrap().parse().ok()?, 4)
                    } else if hh.len() <= 2 {
                        (hh.parse().ok()?, 0, 2)
                    } else {
                        continue;
                    };
                    p.offset = Some(sign * (h * 3600 + m * 60));
                    for u in used.iter_mut().skip(i).take(k) {
                        *u = true;
                    }
                    break;
                }
            }
            _ => {}
        }
    }

    // Dates.
    let mut found_date = false;
    for i in 0..n {
        if used[i] {
            continue;
        }
        // y-m-d / y/m/d / d.m.y / a/b/c
        if let Some(a) = num(i) {
            for sep in ['-', '/', '.'] {
                if sym(i + 1, sep) && num(i + 2).is_some() && !used[i + 2] {
                    let b = num(i + 2).unwrap();
                    let c = if sym(i + 3, sep) { num(i + 4) } else { None };
                    match (sep, c) {
                        ('-', Some(c)) => {
                            let signed = i > 0
                                && (sym(i - 1, '-') || sym(i - 1, '+'))
                                && !used[i - 1]
                                && (i == 1 || !matches!(t[i - 2], Tk::Num(_)));
                            let y = if signed {
                                used[i - 1] = true;
                                let v: i64 = a.parse().ok()?;
                                if sym(i - 1, '-') { -v } else { v }
                            } else {
                                expand_year(a)
                            };
                            p.year = Some(y);
                            p.mon = Some(b.parse().ok()?);
                            p.mday = Some(c.parse().ok()?);
                        }
                        ('/', Some(c)) | ('.', Some(c)) => {
                            if a.len() >= 3 || (sep == '/' && c.len() < 3) {
                                p.year = Some(expand_year(a));
                                p.mon = Some(b.parse().ok()?);
                                p.mday = Some(c.parse().ok()?);
                            } else {
                                p.year = Some(expand_year(c));
                                p.mon = Some(b.parse().ok()?);
                                p.mday = Some(a.parse().ok()?);
                            }
                        }
                        ('/', None) => {
                            p.mon = Some(a.parse().ok()?);
                            p.mday = Some(b.parse().ok()?);
                        }
                        _ => continue,
                    }
                    let k = if c.is_some() { 5 } else { 3 };
                    for u in used.iter_mut().skip(i).take(k) {
                        *u = true;
                    }
                    found_date = true;
                    break;
                }
            }
            if found_date {
                break;
            }
            if a.len() == 8 && !used[i] {
                p.year = Some(a[..4].parse().ok()?);
                p.mon = Some(a[4..6].parse().ok()?);
                p.mday = Some(a[6..].parse().ok()?);
                used[i] = true;
                found_date = true;
                break;
            }
        }
    }
    if !found_date {
        // Month names: "Jan 15 2024", "15 Jan 2024", "January 15, 2024", "June 2020".
        for i in 0..n {
            if used[i] {
                continue;
            }
            if let Tk::Word(w) = &t[i] {
                if let Some(m) = month_from_name(w) {
                    p.mon = Some(m);
                    used[i] = true;
                    // day before?
                    if i > 0 && !used[i - 1] {
                        if let Some(d) = num(i - 1) {
                            if d.len() <= 2 {
                                p.mday = Some(d.parse().ok()?);
                                used[i - 1] = true;
                            }
                        }
                    }
                    let mut j = i + 1;
                    if sym(j, '.') {
                        j += 1;
                    }
                    if p.mday.is_none() {
                        if let Some(d) = num(j) {
                            if d.len() <= 2 && !used[j] {
                                p.mday = Some(d.parse().ok()?);
                                used[j] = true;
                                j += 1;
                                if let Some(Tk::Word(suf)) = t.get(j) {
                                    if matches!(suf.to_ascii_lowercase().as_str(), "st" | "nd" | "rd" | "th") {
                                        used[j] = true;
                                        j += 1;
                                    }
                                }
                            }
                        }
                    }
                    if sym(j, ',') {
                        j += 1;
                    }
                    if let Some(y) = num(j) {
                        if !used[j] && (y.len() == 4 || p.mday.is_some()) {
                            p.year = Some(expand_year(y));
                            used[j] = true;
                        }
                    }
                    found_date = true;
                    break;
                }
            }
        }
        if found_date && p.year.is_none() {
            // A 4-digit number anywhere (e.g. after the time).
            for i in 0..n {
                if !used[i] {
                    if let Some(y) = num(i) {
                        if y.len() == 4 {
                            p.year = Some(y.parse().ok()?);
                            used[i] = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    if !found_date {
        // A lone 3-digit number is a day of the year; 1-2 digits a day of the month.
        for i in 0..n {
            if !used[i] {
                if let Some(d) = num(i) {
                    if d.len() == 3 {
                        p.yday = Some(d.parse().ok()?);
                        used[i] = true;
                        break;
                    } else if d.len() <= 2 && p.hour.is_some() {
                        p.mday = Some(d.parse().ok()?);
                        used[i] = true;
                        break;
                    }
                }
            }
        }
    }

    // Anything left must be ignorable: weekday names, punctuation, "T", "at".
    for i in 0..n {
        if used[i] {
            continue;
        }
        match &t[i] {
            Tk::Word(w) => {
                let wl = w.to_ascii_lowercase();
                if is_weekday_name(w) || wl == "t" || wl == "at" || wl == "of" || wl == "on" {
                    continue;
                }
                if p.year.is_none() && p.mon.is_none() && p.hour.is_none() {
                    return None;
                }
            }
            Tk::Sym(_) => {}
            Tk::Num(_) => {}
        }
    }
    if p.year.is_none()
        && p.mon.is_none()
        && p.mday.is_none()
        && p.hour.is_none()
        && p.min.is_none()
        && p.sec.is_none()
        && p.yday.is_none()
    {
        return None;
    }
    Some(p)
}

/// Ruby's `Time.parse(s)` with `now` filling in missing leading fields.
pub fn parse_time(s: &str, now: Timestamp) -> Option<Timestamp> {
    let p = parse_parts(s)?;
    let off = p.offset;
    let now_local = match off {
        Some(o) => Timestamp { offset: o, utc: o == 0, ..now },
        None => Timestamp { offset: 0, utc: true, ..now },
    };
    let nc = now_local.civil();
    let mut year = p.year;
    let mut mon = p.mon;
    let mut day = p.mday;
    let mut hour = p.hour;
    // Fill the leading missing components from `now`.
    'fill: {
        if year.is_some() {
            break 'fill;
        }
        year = Some(nc.year);
        if p.yday.is_some() {
            break 'fill;
        }
        if mon.is_some() {
            break 'fill;
        }
        mon = Some(nc.month as i64);
        if day.is_some() {
            break 'fill;
        }
        day = Some(nc.day as i64);
        if hour.is_some() {
            break 'fill;
        }
        hour = Some(nc.hour as i64);
    }
    let year = year?;
    let mon = mon.unwrap_or(1);
    let day = day.unwrap_or(1);
    let hour = hour.unwrap_or(0);
    let min = p.min.unwrap_or(0);
    let sec = p.sec.unwrap_or(0);
    if !(1..=12).contains(&mon) || !(1..=31).contains(&day) || !(0..=24).contains(&hour) || !(0..=59).contains(&min)
    {
        return None;
    }
    if !(0..=60).contains(&sec) {
        return None;
    }
    if year.abs() > 100_000_000_000 {
        return None;
    }
    let days = match p.yday {
        Some(yd) => {
            let len = days_from_civil(year + 1, 1, 1) - days_from_civil(year, 1, 1);
            if !(1..=len).contains(&yd) {
                return None;
            }
            days_from_civil(year, 1, 1) + yd - 1
        }
        None => days_from_civil(year, mon as u32, 1) + day - 1,
    };
    let local = days * 86400 + hour * 3600 + min * 60 + sec;
    let offset = off.unwrap_or(0);
    Some(Timestamp { secs: local - offset as i64, nanos: p.nanos, offset, utc: offset == 0 })
}

// ---------------------------------------------------------------------------
// strftime
// ---------------------------------------------------------------------------

fn iso_week(c: &Civil) -> (i64, u32) {
    // ISO 8601 week-numbering year and week.
    let wday = if c.wday == 0 { 7 } else { c.wday } as i64; // Mon=1..Sun=7
    let thursday = c.days - wday + 4;
    let (ty, _, _) = civil_from_days(thursday);
    let jan1 = days_from_civil(ty, 1, 1);
    let week = (thursday - jan1) / 7 + 1;
    (ty, week as u32)
}

enum Pad {
    Default,
    None,
    Space,
    Zero,
}

fn push_num(out: &mut String, v: i64, width: usize, pad: &Pad, default_pad: char) {
    let pad_char = match pad {
        Pad::None => {
            let _ = write!(out, "{}", v);
            return;
        }
        Pad::Space => ' ',
        Pad::Zero => '0',
        Pad::Default => default_pad,
    };
    let s = v.abs().to_string();
    let neg = v < 0;
    let len = s.len() + neg as usize;
    if pad_char == '0' {
        if neg {
            out.push('-');
        }
        for _ in len..width {
            out.push('0');
        }
        out.push_str(&s);
    } else {
        for _ in len..width {
            out.push(' ');
        }
        if neg {
            out.push('-');
        }
        out.push_str(&s);
    }
}

fn push_str(out: &mut String, s: &str, width: usize, pad: &Pad, upcase: bool, swapcase: bool) {
    let s: String = if upcase {
        s.to_uppercase()
    } else if swapcase {
        // Ruby's '#' flag: swap case of alphabetic strings.
        if s.chars().any(|c| c.is_lowercase()) { s.to_uppercase() } else { s.to_lowercase() }
    } else {
        s.to_string()
    };
    let n = s.chars().count();
    if !matches!(pad, Pad::None) {
        let pc = if matches!(pad, Pad::Zero) { '0' } else { ' ' };
        for _ in n..width {
            out.push(pc);
        }
    }
    out.push_str(&s);
}

/// Ruby `Time#strftime`.
pub fn strftime(t: &Timestamp, fmt: &str) -> String {
    let c = t.civil();
    let mut out = String::new();
    let chars: Vec<char> = fmt.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        let ch = chars[i];
        if ch != '%' {
            out.push(ch);
            i += 1;
            continue;
        }
        let start = i;
        i += 1;
        let mut pad = Pad::Default;
        let mut upcase = false;
        let mut swapcase = false;
        let mut colons = 0;
        while i < chars.len() {
            match chars[i] {
                '-' => pad = Pad::None,
                '_' => pad = Pad::Space,
                '0' => pad = Pad::Zero,
                '^' => upcase = true,
                '#' => swapcase = true,
                _ => break,
            }
            i += 1;
        }
        let mut width = 0usize;
        let mut has_width = false;
        while i < chars.len() && chars[i].is_ascii_digit() {
            width = width.saturating_mul(10).saturating_add(chars[i] as usize - '0' as usize).min(1024);
            has_width = true;
            i += 1;
        }
        while i < chars.len() && chars[i] == ':' {
            colons += 1;
            i += 1;
        }
        if i < chars.len() && (chars[i] == 'E' || chars[i] == 'O') && colons == 0 {
            // Locale modifiers: valid only before certain conversions.
            let next = chars.get(i + 1).copied();
            let ok = match chars[i] {
                'E' => matches!(next, Some('c' | 'C' | 'x' | 'X' | 'y' | 'Y')),
                _ => matches!(next, Some('d' | 'e' | 'H' | 'I' | 'm' | 'M' | 'S' | 'u' | 'U' | 'V' | 'w' | 'W' | 'y')),
            };
            if ok {
                i += 1;
            }
        }
        let Some(&conv) = chars.get(i) else {
            out.extend(&chars[start..]);
            break;
        };
        i += 1;
        if colons > 0 && conv != 'z' {
            out.extend(&chars[start..i]);
            continue;
        }
        let w = |d: usize| if has_width { width } else { d };
        let hour12 = if c.hour % 12 == 0 { 12 } else { c.hour % 12 } as i64;
        match conv {
            'Y' => {
                if c.year >= 0 && c.year < 10000 && !has_width {
                    push_num(&mut out, c.year, 4, &pad, '0');
                } else {
                    push_num(&mut out, c.year, w(if c.year < 0 { 5 } else { 4 }), &pad, '0');
                }
            }
            'C' => push_num(&mut out, c.year.div_euclid(100), w(2), &pad, '0'),
            'y' => push_num(&mut out, c.year.rem_euclid(100), w(2), &pad, '0'),
            'm' => push_num(&mut out, c.month as i64, w(2), &pad, '0'),
            'd' => push_num(&mut out, c.day as i64, w(2), &pad, '0'),
            'e' => push_num(&mut out, c.day as i64, w(2), &pad, ' '),
            'j' => push_num(&mut out, c.yday as i64, w(3), &pad, '0'),
            'H' => push_num(&mut out, c.hour as i64, w(2), &pad, '0'),
            'k' => push_num(&mut out, c.hour as i64, w(2), &pad, ' '),
            'I' => push_num(&mut out, hour12, w(2), &pad, '0'),
            'l' => push_num(&mut out, hour12, w(2), &pad, ' '),
            'M' => push_num(&mut out, c.min as i64, w(2), &pad, '0'),
            'S' => push_num(&mut out, c.sec as i64, w(2), &pad, '0'),
            'L' | 'N' => {
                let digits = if has_width { width } else if conv == 'L' { 3 } else { 9 };
                let mut s = format!("{:09}", t.nanos);
                if digits <= 9 {
                    s.truncate(digits);
                } else {
                    while s.len() < digits {
                        s.push('0');
                    }
                }
                out.push_str(&s);
            }
            's' => push_num(&mut out, t.secs, w(1), &pad, '0'),
            'u' => push_num(&mut out, if c.wday == 0 { 7 } else { c.wday } as i64, w(1), &pad, '0'),
            'w' => push_num(&mut out, c.wday as i64, w(1), &pad, '0'),
            'U' => {
                let wk = (c.yday as i64 + 6 - c.wday as i64) / 7;
                push_num(&mut out, wk, w(2), &pad, '0')
            }
            'W' => {
                let wd = (c.wday as i64 + 6) % 7;
                let wk = (c.yday as i64 + 6 - wd) / 7;
                push_num(&mut out, wk, w(2), &pad, '0')
            }
            'G' => {
                let (y, _) = iso_week(&c);
                push_num(&mut out, y, w(if y < 0 { 5 } else { 4 }), &pad, '0')
            }
            'g' => {
                let (y, _) = iso_week(&c);
                push_num(&mut out, y.rem_euclid(100), w(2), &pad, '0')
            }
            'V' => {
                let (_, wk) = iso_week(&c);
                push_num(&mut out, wk as i64, w(2), &pad, '0')
            }
            'A' => push_str(&mut out, DAYS[c.wday as usize], width, &pad, upcase, swapcase),
            'a' => push_str(&mut out, &DAYS[c.wday as usize][..3], width, &pad, upcase, swapcase),
            'B' => push_str(&mut out, MONTHS[c.month as usize - 1], width, &pad, upcase, swapcase),
            'b' | 'h' => push_str(&mut out, &MONTHS[c.month as usize - 1][..3], width, &pad, upcase, swapcase),
            'p' => {
                let s = if c.hour >= 12 { "PM" } else { "AM" };
                if swapcase {
                    push_str(&mut out, &s.to_lowercase(), width, &pad, false, false);
                } else {
                    push_str(&mut out, s, width, &pad, upcase, false);
                }
            }
            'P' => push_str(&mut out, if c.hour >= 12 { "pm" } else { "am" }, width, &pad, upcase, false),
            'Z' => {
                let z = if t.utc { "UTC" } else { "" };
                push_str(&mut out, z, width, &pad, upcase, swapcase);
            }
            'z' => {
                let off = t.offset;
                let sign = if off < 0 { '-' } else { '+' };
                let a = off.unsigned_abs();
                let (h, m, s) = (a / 3600, (a % 3600) / 60, a % 60);
                let rest = match colons {
                    0 => format!("{:02}", m),
                    1 => format!(":{:02}", m),
                    2 => format!(":{:02}:{:02}", m, s),
                    _ => String::new(),
                };
                if colons > 3 {
                    out.extend(&chars[start..i]);
                    continue;
                }
                match pad {
                    Pad::Space => {
                        let body = format!("{}{}{}", sign, h, rest);
                        let target = if has_width { width } else { 3 + rest.len() };
                        for _ in body.len()..target {
                            out.push(' ');
                        }
                        out.push_str(&body);
                    }
                    _ => {
                        out.push(sign);
                        let hs = format!("{:02}", h);
                        let total = 1 + hs.len() + rest.len();
                        if has_width && !matches!(pad, Pad::None) {
                            for _ in total..width {
                                out.push('0');
                            }
                        }
                        out.push_str(&hs);
                        out.push_str(&rest);
                    }
                }
            }
            'c' => {
                let s = strftime(t, "%a %b %e %H:%M:%S %Y");
                push_str(&mut out, &s, width, &pad, upcase, swapcase);
            }
            'x' | 'D' => push_str(&mut out, &strftime(t, "%m/%d/%y"), width, &pad, upcase, false),
            'X' | 'T' => push_str(&mut out, &strftime(t, "%H:%M:%S"), width, &pad, upcase, false),
            'F' => push_str(&mut out, &strftime(t, "%Y-%m-%d"), width, &pad, upcase, false),
            'R' => push_str(&mut out, &strftime(t, "%H:%M"), width, &pad, upcase, false),
            'r' => push_str(&mut out, &strftime(t, "%I:%M:%S %p"), width, &pad, upcase, false),
            'v' => push_str(&mut out, &strftime(t, "%e-%^b-%Y"), width, &pad, upcase, false),
            'n' => out.push('\n'),
            't' => out.push('\t'),
            '%' => out.push('%'),
            _ => out.extend(&chars[start..i]),
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn civil_roundtrip() {
        for d in [-800000i64, -1, 0, 1, 18000, 2932896] {
            let (y, m, dd) = civil_from_days(d);
            assert_eq!(days_from_civil(y, m, dd), d);
        }
    }

    #[test]
    fn formats() {
        let t = Timestamp::from_unix(1592222400);
        assert_eq!(strftime(&t, "%c"), "Mon Jun 15 12:00:00 2020");
        assert_eq!(strftime(&t, "%V %U %W %j"), "25 24 24 167");
        assert_eq!(strftime(&t, "%^b %#B %10A %-m %_m %Q"), "JUN JUNE     Monday 6  6 %Q");
        assert_eq!(strftime(&t, "%v"), "15-JUN-2020");
    }

    #[test]
    fn parses() {
        let now = Timestamp::from_unix(1704067318);
        let f = |s: &str| parse_time(s, now).map(|t| strftime(&t, "%Y-%m-%d %H:%M:%S %z"));
        assert_eq!(f("2024-01-15").as_deref(), Some("2024-01-15 00:00:00 +0000"));
        assert_eq!(f("Fri Jul 16 01:00:00 2004").as_deref(), Some("2004-07-16 01:00:00 +0000"));
        assert_eq!(f("2006-07-05 10:00:00 -04:00").as_deref(), Some("2006-07-05 10:00:00 -0400"));
        assert_eq!(f("10:30").as_deref(), Some("2024-01-01 10:30:00 +0000"));
        assert_eq!(f("January 15, 2024").as_deref(), Some("2024-01-15 00:00:00 +0000"));
        assert_eq!(f("15/01/2024").as_deref(), Some("2024-01-15 00:00:00 +0000"));
        assert_eq!(f("hello"), None);
        assert_eq!(f("2020-02-30").as_deref(), Some("2020-03-01 00:00:00 +0000"));
        assert_eq!(f("2020-13-45"), None);
    }
}
