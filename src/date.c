/*
 * date.c - Utils.to_date (Time.parse subset) and Ruby-compatible strftime.
 *
 * The local time zone is UTC (the reference suite freezes TZ=UTC); parsed
 * strings carrying an explicit offset keep it.
 */
#include "lq.h"
#include "internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Civil calendar                                                      */
/* ------------------------------------------------------------------ */

static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, int *m, int *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yy + (*m <= 2);
}

static bool is_leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

typedef struct tm_parts {
    int64_t year;
    int mon, mday, hour, min, sec;
    int wday, yday; /* yday 1..366 */
    int64_t nsec;
    int32_t off;
    bool utc;
    int64_t epoch;
} tm_parts;

static void breakdown(const lq_time *t, tm_parts *p) {
    int64_t local = t->sec + t->off;
    int64_t days = floor_div(local, 86400);
    int64_t secs = local - days * 86400;
    civil_from_days(days, &p->year, &p->mon, &p->mday);
    p->hour = (int)(secs / 3600);
    p->min = (int)(secs / 60 % 60);
    p->sec = (int)(secs % 60);
    int64_t w = (days + 4) % 7;
    if (w < 0) w += 7;
    p->wday = (int)w;
    p->yday = (int)(days - days_from_civil(p->year, 1, 1) + 1);
    p->nsec = t->nsec;
    p->off = t->off;
    p->utc = t->utc;
    p->epoch = t->sec;
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

static const char *MONTHS[] = {"january", "february", "march", "april", "may", "june", "july",
                               "august", "september", "october", "november", "december"};
static const char *WDAYS[] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};

static int month_from_word(const char *w, size_t n) {
    if (n < 3) return 0;
    for (int i = 0; i < 12; i++) {
        size_t ml = strlen(MONTHS[i]);
        if (n == 3 || (n <= ml && n >= 3)) {
            if (strncmp(w, MONTHS[i], n) == 0 && (n == 3 || n == ml)) return i + 1;
        }
    }
    /* "sept" */
    if (n == 4 && strncmp(w, "sept", 4) == 0) return 9;
    return 0;
}

static bool is_wday_word(const char *w, size_t n) {
    if (n < 3) return false;
    for (int i = 0; i < 7; i++) {
        size_t wl = strlen(WDAYS[i]);
        if ((n == 3 || n == wl) && strncmp(w, WDAYS[i], n) == 0) return true;
    }
    return false;
}

static bool zone_word(const char *w, size_t n, int32_t *off, bool *utc) {
    static const struct { const char *name; int hours; } Z[] = {
        {"utc", 0}, {"gmt", 0}, {"z", 0}, {"ut", 0}, {"est", -5}, {"edt", -4}, {"cst", -6}, {"cdt", -5},
        {"mst", -7}, {"mdt", -6}, {"pst", -8}, {"pdt", -7}, {"bst", 1}, {"cet", 1}, {"cest", 2},
        {"eet", 2}, {"eest", 3}, {"jst", 9}, {"ist", 5}, {"hst", -10}, {"akst", -9}, {"akdt", -8},
    };
    for (size_t i = 0; i < sizeof Z / sizeof Z[0]; i++) {
        if (strlen(Z[i].name) == n && strncmp(w, Z[i].name, n) == 0) {
            *off = Z[i].hours * 3600;
            *utc = Z[i].hours == 0 && (n == 3 || n == 1 || n == 2) && strncmp(w, "bst", 3) != 0;
            if (strncmp(w, "ist", 3) == 0 && n == 3) *off = 5 * 3600 + 1800;
            return true;
        }
    }
    return false;
}

typedef struct pstate {
    char *s; /* lowercased copy */
    size_t n;
    bool *used;
} pstate;

static bool digits_at(const char *s, size_t n, size_t i, size_t cnt) {
    if (i + cnt > n) return false;
    for (size_t k = 0; k < cnt; k++)
        if (!isdigit((unsigned char)s[i + k])) return false;
    return true;
}

static size_t digit_run(const char *s, size_t n, size_t i) {
    size_t j = i;
    while (j < n && isdigit((unsigned char)s[j])) j++;
    return j - i;
}

static int64_t num_at(const char *s, size_t i, size_t len) {
    int64_t v = 0;
    for (size_t k = 0; k < len && k < 18; k++) v = v * 10 + (s[i + k] - '0');
    return v;
}

static void mark(pstate *ps, size_t a, size_t b) {
    for (size_t i = a; i < b && i < ps->n; i++) ps->used[i] = true;
}

/* returns true if parsed something */
static bool parse_time_string(const char *src, size_t n, const lq_time *now, lq_time *out) {
    pstate ps;
    ps.n = n;
    ps.s = lq_xmalloc(n + 1);
    ps.used = lq_xcalloc(n + 1, 1);
    for (size_t i = 0; i < n; i++) ps.s[i] = (char)tolower((unsigned char)src[i]);
    ps.s[n] = 0;
    const char *s = ps.s;

    bool have_date = false, have_time = false, have_zone = false, have_year = false, have_mon = false, have_day = false;
    int64_t year = 0;
    int mon = 0, day = 0, hour = 0, min = 0, sec = 0;
    int64_t nsec = 0;
    int32_t off = 0;
    bool zone_utc = false;
    bool ok = true;

    /* time: hh:mm[:ss[.frac]] [am|pm] */
    for (size_t i = 0; i < n; i++) {
        size_t h = digit_run(s, n, i);
        if (h >= 1 && h <= 2 && (i == 0 || !isdigit((unsigned char)s[i - 1])) && i + h < n && s[i + h] == ':' &&
            digits_at(s, n, i + h + 1, 2)) {
            size_t j = i + h + 1;
            hour = (int)num_at(s, i, h);
            min = (int)num_at(s, j, 2);
            j += 2;
            if (j < n && s[j] == ':' && digits_at(s, n, j + 1, 2)) {
                sec = (int)num_at(s, j + 1, 2);
                j += 3;
                if (j < n && (s[j] == '.' || s[j] == ',') && j + 1 < n && isdigit((unsigned char)s[j + 1])) {
                    size_t fl = digit_run(s, n, j + 1);
                    int64_t f = 0;
                    for (size_t k = 0; k < 9; k++) f = f * 10 + (k < fl ? s[j + 1 + k] - '0' : 0);
                    nsec = f;
                    j += 1 + fl;
                }
            }
            size_t k = j;
            while (k < n && s[k] == ' ') k++;
            if (k + 1 < n && (s[k] == 'a' || s[k] == 'p') && (s[k + 1] == 'm' || (s[k + 1] == '.' && k + 2 < n && s[k + 2] == 'm'))) {
                bool pm = s[k] == 'p';
                if (hour == 12) hour = 0;
                if (pm) hour += 12;
                j = k + (s[k + 1] == 'm' ? 2 : 3);
                if (j < n && s[j] == '.') j++;
            }
            mark(&ps, i, j);
            have_time = true;
            /* zone right after the time */
            size_t z = j;
            while (z < n && s[z] == ' ') z++;
            if (z < n && (s[z] == '+' || s[z] == '-') && isdigit((unsigned char)s[z + 1])) {
                size_t d = digit_run(s, n, z + 1);
                int sign = s[z] == '-' ? -1 : 1;
                int hh = 0, mm = 0;
                size_t end = z + 1 + d;
                if (d == 4) { hh = (int)num_at(s, z + 1, 2); mm = (int)num_at(s, z + 3, 2); }
                else if (d <= 2) {
                    hh = (int)num_at(s, z + 1, d);
                    if (end < n && s[end] == ':' && digits_at(s, n, end + 1, 2)) {
                        mm = (int)num_at(s, end + 1, 2);
                        end += 3;
                    }
                } else { hh = (int)num_at(s, z + 1, 2); mm = (int)num_at(s, z + 3, 2); }
                off = sign * (hh * 3600 + mm * 60);
                have_zone = true;
                mark(&ps, z, end);
            } else if (z < n && isalpha((unsigned char)s[z])) {
                size_t e = z;
                while (e < n && isalpha((unsigned char)s[e])) e++;
                if (zone_word(s + z, e - z, &off, &zone_utc)) {
                    have_zone = true;
                    mark(&ps, z, e);
                }
            }
            break;
        }
    }
    /* "3pm" style */
    if (!have_time) {
        for (size_t i = 0; i < n; i++) {
            size_t h = digit_run(s, n, i);
            if (h >= 1 && h <= 2 && (i == 0 || !isdigit((unsigned char)s[i - 1]))) {
                size_t k = i + h;
                while (k < n && s[k] == ' ') k++;
                if (k + 1 < n && (s[k] == 'a' || s[k] == 'p') && s[k + 1] == 'm' &&
                    (k + 2 >= n || !isalpha((unsigned char)s[k + 2]))) {
                    hour = (int)num_at(s, i, h);
                    if (hour == 12) hour = 0;
                    if (s[k] == 'p') hour += 12;
                    have_time = true;
                    mark(&ps, i, k + 2);
                    break;
                }
            }
            i += h;
        }
    }
    /* date: [+-]y+-mm-dd (any year length), yyyy/mm/dd */
    for (size_t i = 0; i < n && !have_date; i++) {
        if (ps.used[i]) continue;
        size_t y = digit_run(s, n, i);
        if (y >= 1 && (i == 0 || !isdigit((unsigned char)s[i - 1])) && i + y < n && s[i + y] == '-' &&
            digit_run(s, n, i + y + 1) >= 1) {
            size_t j = i + y + 1;
            size_t m = digit_run(s, n, j);
            if (m >= 1 && m <= 2 && j + m < n && s[j + m] == '-') {
                size_t k = j + m + 1;
                size_t d = digit_run(s, n, k);
                if (d >= 1 && d <= 2) {
                    bool sign = i > 0 && (s[i - 1] == '-' || s[i - 1] == '+') && (i < 2 || !isdigit((unsigned char)s[i - 2]));
                    if (y > 12) { ok = false; break; }
                    year = num_at(s, i, y);
                    if (!sign && y <= 2) year += year >= 69 ? 1900 : 2000;
                    if (sign && s[i - 1] == '-') year = -year;
                    mon = (int)num_at(s, j, m);
                    day = (int)num_at(s, k, d);
                    have_date = have_year = have_mon = have_day = true;
                    mark(&ps, sign ? i - 1 : i, k + d);
                    break;
                }
            }
        }
        if (y == 4 && (i == 0 || !isdigit((unsigned char)s[i - 1])) && i + y < n && (s[i + y] == '-' || s[i + y] == '/')) {
            char sep = s[i + y];
            size_t j = i + y + 1;
            size_t m = digit_run(s, n, j);
            if (m >= 1 && m <= 2 && j + m < n && s[j + m] == sep) {
                size_t k = j + m + 1;
                size_t d = digit_run(s, n, k);
                if (d >= 1 && d <= 2) {
                    year = num_at(s, i, 4);
                    mon = (int)num_at(s, j, m);
                    day = (int)num_at(s, k, d);
                    have_date = have_year = have_mon = have_day = true;
                    mark(&ps, i, k + d);
                }
            } else if (m >= 1 && m <= 2) {
                year = num_at(s, i, 4);
                mon = (int)num_at(s, j, m);
                have_date = have_year = have_mon = true;
                mark(&ps, i, j + m);
            }
        }
        if (y) i += y - 1;
    }
    /* dd/mm/yy[yy] or dd-mm-yyyy */
    for (size_t i = 0; i < n && !have_date; i++) {
        if (ps.used[i]) continue;
        size_t a = digit_run(s, n, i);
        if (a >= 1 && a <= 2 && (i == 0 || !isdigit((unsigned char)s[i - 1])) && i + a < n && (s[i + a] == '/' || s[i + a] == '-' || s[i + a] == '.')) {
            char sep = s[i + a];
            size_t j = i + a + 1;
            size_t b = digit_run(s, n, j);
            if (b >= 1 && b <= 2) {
                size_t k = j + b;
                if (k < n && s[k] == sep && digit_run(s, n, k + 1) >= 1) {
                    size_t c = digit_run(s, n, k + 1);
                    if (sep == '/' ) {
                        /* Date._parse treats a/b/c as mon/day/year? reference: day first fails for 06/15 */
                        day = (int)num_at(s, i, a);
                        mon = (int)num_at(s, j, b);
                    } else {
                        day = (int)num_at(s, i, a);
                        mon = (int)num_at(s, j, b);
                    }
                    year = num_at(s, k + 1, c);
                    if (c <= 2) year += year >= 69 ? 1900 : 2000;
                    have_date = have_year = have_mon = have_day = true;
                    mark(&ps, i, k + 1 + c);
                } else if (sep == '/') {
                    mon = (int)num_at(s, i, a);
                    day = (int)num_at(s, j, b);
                    have_date = have_mon = have_day = true;
                    mark(&ps, i, k);
                }
            }
        }
        if (a) i += a - 1;
    }
    /* month names */
    if (!have_date) {
        for (size_t i = 0; i < n; i++) {
            if (ps.used[i] || !isalpha((unsigned char)s[i]) || (i > 0 && isalpha((unsigned char)s[i - 1]))) continue;
            size_t e = i;
            while (e < n && isalpha((unsigned char)s[e])) e++;
            int m = month_from_word(s + i, e - i);
            if (m) {
                mon = m;
                have_mon = true;
                have_date = true;
                mark(&ps, i, e);
                size_t k = e;
                if (k < n && s[k] == '.') k++;
                while (k < n && (s[k] == ' ' || s[k] == '-' || s[k] == ',')) k++;
                size_t d = digit_run(s, n, k);
                if (d >= 1 && d <= 2 && !ps.used[k]) {
                    day = (int)num_at(s, k, d);
                    have_day = true;
                    size_t ke = k + d;
                    if (ke + 1 < n && (strncmp(s + ke, "st", 2) == 0 || strncmp(s + ke, "nd", 2) == 0 ||
                                       strncmp(s + ke, "rd", 2) == 0 || strncmp(s + ke, "th", 2) == 0))
                        ke += 2;
                    mark(&ps, k, ke);
                } else {
                    /* day before the month */
                    size_t b = i;
                    while (b > 0 && (s[b - 1] == ' ' || s[b - 1] == '-' || s[b - 1] == '.')) b--;
                    size_t be = b;
                    while (b > 0 && isalpha((unsigned char)s[b - 1])) b--;
                    if (be - b == 2 && b > 0) be = b; /* "16th" */
                    size_t st = be;
                    while (st > 0 && isdigit((unsigned char)s[st - 1])) st--;
                    if (be - st >= 1 && be - st <= 2 && !ps.used[st]) {
                        day = (int)num_at(s, st, be - st);
                        have_day = true;
                        mark(&ps, st, be);
                    }
                }
                break;
            }
            i = e - 1;
        }
        if (have_mon) {
            /* year: a remaining 4-digit (or other) number */
            for (size_t i = 0; i < n; i++) {
                if (ps.used[i] || !isdigit((unsigned char)s[i])) continue;
                size_t d = digit_run(s, n, i);
                if (d >= 3 || (d >= 1 && !have_year)) {
                    bool usedr = false;
                    for (size_t k = i; k < i + d; k++) usedr |= ps.used[k];
                    if (!usedr) {
                        year = num_at(s, i, d);
                        if (d <= 2) year += year >= 69 ? 1900 : 2000;
                        have_year = true;
                        mark(&ps, i, i + d);
                        break;
                    }
                }
                i += d - 1;
            }
        }
    }
    /* bare digit runs (Date._parse "ddd" forms): dd, ddd (yday), mmdd,
       yyddd, yymmdd, yyyyddd, yyyymmdd[hh[mm[ss]]] */
    int yday = 0;
    if (!have_date) {
        for (size_t i = 0; i < n; i++) {
            if (ps.used[i] || !isdigit((unsigned char)s[i])) continue;
            size_t d = digit_run(s, n, i);
            bool hit = true;
            switch (d) {
            case 1: if (have_time) { day = (int)num_at(s, i, 1); have_day = true; } else hit = false; break;
            case 2: day = (int)num_at(s, i, 2); have_day = true; break;
            case 3: yday = (int)num_at(s, i, 3); break;
            case 4: mon = (int)num_at(s, i, 2); day = (int)num_at(s, i + 2, 2); have_mon = have_day = true; break;
            case 5:
                year = num_at(s, i, 2);
                year += year >= 69 ? 1900 : 2000;
                yday = (int)num_at(s, i + 2, 3);
                have_year = true;
                break;
            case 6:
                year = num_at(s, i, 2);
                year += year >= 69 ? 1900 : 2000;
                mon = (int)num_at(s, i + 2, 2);
                day = (int)num_at(s, i + 4, 2);
                have_year = have_mon = have_day = true;
                break;
            case 7:
                year = num_at(s, i, 4);
                yday = (int)num_at(s, i + 4, 3);
                have_year = true;
                break;
            case 8: case 10: case 12: case 14:
                year = num_at(s, i, 4);
                mon = (int)num_at(s, i + 4, 2);
                day = (int)num_at(s, i + 6, 2);
                have_year = have_mon = have_day = true;
                if (d >= 10) { hour = (int)num_at(s, i + 8, 2); have_time = true; }
                if (d >= 12) min = (int)num_at(s, i + 10, 2);
                if (d >= 14) sec = (int)num_at(s, i + 12, 2);
                break;
            default: hit = false;
            }
            if (hit) {
                have_date = true;
                mark(&ps, i, i + d);
                break;
            }
            i += d - 1;
        }
    }
    /* standalone zone (e.g. "... 2004 UTC") */
    if (!have_zone && (have_date || have_time)) {
        for (size_t i = 0; i < n; i++) {
            if (ps.used[i]) continue;
            if (isalpha((unsigned char)s[i]) && (i == 0 || !isalpha((unsigned char)s[i - 1]))) {
                size_t e = i;
                while (e < n && isalpha((unsigned char)s[e])) e++;
                if (!is_wday_word(s + i, e - i) && zone_word(s + i, e - i, &off, &zone_utc)) {
                    have_zone = true;
                    break;
                }
                i = e - 1;
            }
        }
    }
    free(ps.s);
    free(ps.used);
    if (!ok) return false;
    if (!have_date && !have_time) return false;

    tm_parts np;
    breakdown(now, &np);
    if (!have_year) year = np.year;
    if (yday) {
        if (yday < 1 || yday > (is_leap(year) ? 366 : 365)) return false;
        int64_t dd = days_from_civil(year, 1, 1) + (yday - 1);
        int64_t yy;
        civil_from_days(dd, &yy, &mon, &day);
        have_mon = have_day = true;
    }
    if (!have_mon) mon = have_day ? np.mon : (have_date ? 1 : np.mon);
    if (!have_day) day = have_date ? 1 : np.mday;
    if (!have_date && !have_day) {
        year = np.year;
        mon = np.mon;
        day = np.mday;
    }
    if (mon < 1 || mon > 12) ok = false;
    if (day < 1 || day > 31) ok = false;
    if (hour > 24 || min > 59 || sec > 60) ok = false;
    /* keep epoch arithmetic within int64 */
    if (year > 100000000000LL || year < -100000000000LL) ok = false;
    if (!ok) return false;
    /* Time.local normalizes e.g. Feb 30 -> Mar 1 */
    int64_t days = days_from_civil(year, mon, 1) + (day - 1);
    int32_t zone_off = have_zone ? off : 0;
    out->sec = days * 86400 + hour * 3600 + min * 60 + sec - zone_off;
    out->nsec = nsec;
    out->off = zone_off;
    out->utc = !have_zone || zone_utc;
    return true;
}

static void now_time(lq_ctx *ctx, lq_time *t) {
    memset(t, 0, sizeof *t);
    if (ctx && ctx->sh->has_now) {
        t->sec = ctx->sh->now;
        t->off = ctx->sh->now_off;
    } else {
        t->sec = (int64_t)time(NULL);
        t->off = 0;
    }
    t->utc = t->off == 0;
}

bool lq_to_date(lq_ctx *ctx, lq_value v, lq_time *t) {
    lq_time now;
    now_time(ctx, &now);
    switch (v.type) {
    case LQ_INT:
        memset(t, 0, sizeof *t);
        t->sec = v.u.i;
        t->utc = true;
        return true;
    case LQ_STR: {
        const char *s = v.u.s->data;
        size_t n = v.u.s->len;
        if (n == 0) return false;
        if ((n == 3 && strncasecmp(s, "now", 3) == 0) || (n == 5 && strncasecmp(s, "today", 5) == 0)) {
            *t = now;
            return true;
        }
        bool all_digits = true;
        for (size_t i = 0; i < n; i++)
            if (!isdigit((unsigned char)s[i])) all_digits = false;
        if (all_digits) {
            lq_value iv = lq_str_to_i(s, n);
            memset(t, 0, sizeof *t);
            t->sec = iv.type == LQ_INT ? iv.u.i : INT64_MAX / 2;
            t->utc = true;
            lq_release(iv);
            return true;
        }
        return parse_time_string(s, n, &now, t);
    }
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* strftime                                                            */
/* ------------------------------------------------------------------ */

static const char *MON_ABBR[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *MON_FULL[] = {"January", "February", "March", "April", "May", "June", "July",
                                 "August", "September", "October", "November", "December"};
static const char *DAY_ABBR[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *DAY_FULL[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

typedef struct fmtopt {
    char pad;    /* 0 default, '-', '_', '0' */
    bool upper;
    bool swap;
    int width;   /* -1 default */
    int colons;
} fmtopt;

static void put_num(lq_buf *b, int64_t v, int defw, char defpad, const fmtopt *o) {
    char tmp[32];
    bool neg = v < 0;
    uint64_t u = neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    int n = snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)u);
    int w = o->width >= 0 ? o->width : defw;
    char pad = o->pad == '-' ? 0 : o->pad == '_' ? ' ' : o->pad == '0' ? '0' : defpad;
    int total = n + (neg ? 1 : 0);
    if (pad == ' ') {
        for (int i = total; i < w; i++) lq_buf_putc(b, ' ');
        if (neg) lq_buf_putc(b, '-');
    } else {
        if (neg) lq_buf_putc(b, '-');
        if (pad == '0')
            for (int i = total; i < w; i++) lq_buf_putc(b, '0');
    }
    lq_buf_put(b, tmp, (size_t)n);
}

static void put_text(lq_buf *b, const char *s, const fmtopt *o, bool swap_to_upper) {
    size_t n = strlen(s);
    int w = o->width;
    char pad = o->pad == '0' ? '0' : ' ';
    if (o->pad != '-' && w > 0)
        for (int i = (int)n; i < w; i++) lq_buf_putc(b, pad);
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (o->upper) c = (char)toupper((unsigned char)c);
        else if (o->swap) c = swap_to_upper ? (char)toupper((unsigned char)c) : (char)tolower((unsigned char)c);
        lq_buf_putc(b, c);
    }
}

static int iso_week(const tm_parts *p, int64_t *iso_year) {
    int wday = (p->wday + 6) % 7; /* Monday = 0 */
    int64_t y = p->year;
    int yday = p->yday - 1;
    int week = (yday - wday + 10) / 7;
    if (week < 1) {
        y -= 1;
        int prev_days = is_leap(y) ? 366 : 365;
        week = (yday + prev_days - wday + 10) / 7;
    } else {
        int days = is_leap(y) ? 366 : 365;
        if (week == 53 && (yday - wday + 3) >= days) {
            week = 1;
            y += 1;
        }
    }
    *iso_year = y;
    return week;
}

static void strftime_rec(lq_buf *b, const char *f, size_t fn, const tm_parts *p, int depth);

static void put_composite(lq_buf *b, const char *sub, const tm_parts *p, const fmtopt *o, int depth) {
    lq_buf t;
    lq_buf_init(&t);
    strftime_rec(&t, sub, strlen(sub), p, depth + 1);
    lq_buf_putc(&t, 0);
    t.len--;
    put_text(b, t.p, o, true);
    lq_buf_free(&t);
}

static void put_zone(lq_buf *b, const tm_parts *p, const fmtopt *o) {
    int32_t off = p->off;
    char sign = off < 0 ? '-' : '+';
    int32_t a = off < 0 ? -off : off;
    int hh = a / 3600, mm = a / 60 % 60, ss = a % 60;
    char tmp[32];
    int n;
    switch (o->colons) {
    case 1: n = snprintf(tmp, sizeof tmp, "%02d:%02d", hh, mm); break;
    case 2: n = snprintf(tmp, sizeof tmp, "%02d:%02d:%02d", hh, mm, ss); break;
    case 3:
        if (mm || ss) n = snprintf(tmp, sizeof tmp, "%02d:%02d", hh, mm);
        else n = snprintf(tmp, sizeof tmp, "%02d", hh);
        break;
    default: n = snprintf(tmp, sizeof tmp, "%02d%02d", hh, mm); break;
    }
    int w = o->width;
    int total = n + 1;
    if (w > total && o->pad != '-') {
        if (o->pad == '_') {
            for (int i = total; i < w; i++) lq_buf_putc(b, ' ');
            lq_buf_putc(b, sign);
        } else {
            lq_buf_putc(b, sign);
            for (int i = total; i < w; i++) lq_buf_putc(b, '0');
        }
    } else lq_buf_putc(b, sign);
    lq_buf_put(b, tmp, (size_t)n);
}

static void strftime_rec(lq_buf *b, const char *f, size_t fn, const tm_parts *p, int depth) {
    for (size_t i = 0; i < fn; i++) {
        if (f[i] != '%' || i + 1 >= fn) {
            lq_buf_putc(b, f[i]);
            continue;
        }
        size_t start = i;
        size_t j = i + 1;
        fmtopt o = {0, false, false, -1, 0};
        for (; j < fn; j++) {
            char c = f[j];
            if (c == '-' || c == '_' || c == '0') o.pad = c;
            else if (c == '^') o.upper = true;
            else if (c == '#') o.swap = true;
            else break;
        }
        if (j < fn && f[j] >= '1' && f[j] <= '9') {
            int w = 0;
            while (j < fn && isdigit((unsigned char)f[j])) {
                if (w < 10000) w = w * 10 + (f[j] - '0');
                j++;
            }
            o.width = w;
        }
        while (j < fn && f[j] == ':') {
            o.colons++;
            j++;
        }
        if (j < fn && (f[j] == 'E' || f[j] == 'O') && j + 1 < fn && strchr("cCxXyYdeHImMSuUVwWy", f[j + 1])) j++;
        if (j >= fn) {
            lq_buf_put(b, f + start, fn - start);
            return;
        }
        char c = f[j];
        int hour12 = p->hour % 12 == 0 ? 12 : p->hour % 12;
        if (o.colons && c != 'z') {
            lq_buf_put(b, f + start, j - start + 1);
            i = j;
            continue;
        }
        switch (c) {
        case 'Y': put_num(b, p->year, p->year < 0 ? 5 : 4, '0', &o); break;
        case 'C': put_num(b, floor_div(p->year, 100), 2, '0', &o); break;
        case 'y': put_num(b, ((p->year % 100) + 100) % 100, 2, '0', &o); break;
        case 'm': put_num(b, p->mon, 2, '0', &o); break;
        case 'd': put_num(b, p->mday, 2, '0', &o); break;
        case 'e': put_num(b, p->mday, 2, ' ', &o); break;
        case 'j': put_num(b, p->yday, 3, '0', &o); break;
        case 'H': put_num(b, p->hour, 2, '0', &o); break;
        case 'k': put_num(b, p->hour, 2, ' ', &o); break;
        case 'I': put_num(b, hour12, 2, '0', &o); break;
        case 'l': put_num(b, hour12, 2, ' ', &o); break;
        case 'M': put_num(b, p->min, 2, '0', &o); break;
        case 'S': put_num(b, p->sec, 2, '0', &o); break;
        case 'L':
        case 'N': {
            int w = o.width >= 0 ? o.width : (c == 'L' ? 3 : 9);
            char digits[10];
            snprintf(digits, sizeof digits, "%09lld", (long long)p->nsec);
            for (int k = 0; k < w; k++) lq_buf_putc(b, k < 9 ? digits[k] : '0');
            break;
        }
        case 'z': put_zone(b, p, &o); break;
        case 'Z': put_text(b, p->utc ? "UTC" : "", &o, false); break;
        case 'A': put_text(b, DAY_FULL[p->wday], &o, true); break;
        case 'a': put_text(b, DAY_ABBR[p->wday], &o, true); break;
        case 'B': put_text(b, MON_FULL[p->mon - 1], &o, true); break;
        case 'b':
        case 'h': put_text(b, MON_ABBR[p->mon - 1], &o, true); break;
        case 'p': {
            fmtopt q = o;
            if (o.swap) { q.swap = true; }
            put_text(b, p->hour < 12 ? "AM" : "PM", &q, false);
            break;
        }
        case 'P': put_text(b, p->hour < 12 ? "am" : "pm", &o, true); break;
        case 'u': put_num(b, p->wday == 0 ? 7 : p->wday, 1, '0', &o); break;
        case 'w': put_num(b, p->wday, 1, '0', &o); break;
        case 'U': put_num(b, (p->yday - 1 + 7 - p->wday) / 7, 2, '0', &o); break;
        case 'W': put_num(b, (p->yday - 1 + 7 - (p->wday + 6) % 7) / 7, 2, '0', &o); break;
        case 'V': {
            int64_t iy;
            put_num(b, iso_week(p, &iy), 2, '0', &o);
            break;
        }
        case 'G': {
            int64_t iy;
            iso_week(p, &iy);
            put_num(b, iy, iy < 0 ? 5 : 4, '0', &o);
            break;
        }
        case 'g': {
            int64_t iy;
            iso_week(p, &iy);
            put_num(b, ((iy % 100) + 100) % 100, 2, '0', &o);
            break;
        }
        case 's': put_num(b, p->epoch, 1, '0', &o); break;
        case 'n': put_text(b, "\n", &o, false); break;
        case 't': put_text(b, "\t", &o, false); break;
        case '%': put_text(b, "%", &o, false); break;
        case 'c': put_composite(b, "%a %b %e %H:%M:%S %Y", p, &o, depth); break;
        case 'x':
        case 'D': put_composite(b, "%m/%d/%y", p, &o, depth); break;
        case 'X':
        case 'T': put_composite(b, "%H:%M:%S", p, &o, depth); break;
        case 'F': put_composite(b, "%Y-%m-%d", p, &o, depth); break;
        case 'R': put_composite(b, "%H:%M", p, &o, depth); break;
        case 'r': put_composite(b, "%I:%M:%S %p", p, &o, depth); break;
        case 'v': put_composite(b, "%e-%^b-%Y", p, &o, depth); break;
        default:
            lq_buf_put(b, f + start, j - start + 1);
            break;
        }
        i = j;
    }
}

void lq_strftime(lq_buf *b, const char *fmt, size_t flen, const lq_time *t) {
    tm_parts p;
    breakdown(t, &p);
    strftime_rec(b, fmt, flen, &p, 0);
}

bool lq_parse_time_text(const char *s, size_t n, lq_time *t) {
    lq_time now;
    memset(&now, 0, sizeof now);
    now.sec = (int64_t)time(NULL);
    now.utc = true;
    return parse_time_string(s, n, &now, t);
}
