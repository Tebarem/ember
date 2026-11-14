/*
 * Date (ECMA-262 section 21.4): the Date constructor, Date.now,
 * Date.parse, Date.UTC, every prototype method (UTC and local getters and setters, the string
 * formats, toISOString, toJSON and @@toPrimitive), and the Annex B methods getYear, setYear and
 * toGMTString.
 *
 * A Date object keeps its [[DateValue]] in the object's internal word, as an encoded number. The
 * class is embi_js_class_date; Date.prototype is kept in a root record (the same pattern as the
 * collections), since the realm has no Date slot in rt->protos.
 *
 * Local time comes from SDL: SDL_TimeToDateTime(..., true) gives the UTC offset of an instant,
 * DST included. SDL_Time is an int64 count of nanoseconds, which only covers about 292 years, so the
 * offset query moves the instant by a whole number of 28-year cycles into 2000..2028. That keeps
 * the weekday pattern and the current DST rules, and it is an approximation for other years.
 *
 * embi_js_date_test_pin_offset fixes the local offset for every conversion, and
 * embi_js_date_test_pin_zone fixes one offset change (a DST transition) as well, so tests do not
 * depend on the machine's time zone. embi_js_date_test_unpin restores the SDL offset.
 */
#include "js/builtins.h"
#include "js/js.h"

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_time.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define EXC EMBI_JS_EXCEPTION
#define UNDEF EMBI_JS_UNDEFINED
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE

#define MS_PER_DAY 86400000.0
#define MS_PER_HOUR 3600000.0
#define MS_PER_MIN 60000.0
#define MAX_TIME_VALUE 8.64e15

/* Clock cycle used for the SDL offset query: 28 years is 10227 days. The window starts at
 * 2000-01-01 (946684800000 ms). */
#define OFFSET_CYCLE_MS 883612800000LL
#define OFFSET_BASE_MS 946684800000LL

/* The UTC bit of a native's magic. */
#define UTC_BIT 0x100

/* Date methods keep a number value (a time value) in the object's internal word. */
const embi_js_class embi_js_class_date = {
    .name = "Date",
};

/* ------------------------------------------------------------------ test hook */

void embi_js_date_test_pin_offset(int32_t minutes);
void embi_js_date_test_pin_zone(int32_t before_min, int32_t after_min, double transition_ms);
void embi_js_date_test_unpin(void);

/* The pinned zone: the offset (ms) before and from the UTC instant transition_ms on. A fixed
 * offset pins both to the same value with an infinite transition. */
static bool g_pinned;
static int64_t g_pin_before;
static int64_t g_pin_after;
static double g_pin_transition;

void embi_js_date_test_pin_offset(int32_t minutes)
{
    embi_js_date_test_pin_zone(minutes, minutes, INFINITY);
}

void embi_js_date_test_pin_zone(int32_t before_min, int32_t after_min, double transition_ms)
{
    g_pinned = true;
    g_pin_before = (int64_t)before_min * 60000;
    g_pin_after = (int64_t)after_min * 60000;
    g_pin_transition = transition_ms;
}

void embi_js_date_test_unpin(void)
{
    g_pinned = false;
}

/* ------------------------------------------------------------------ local time zone */

/* The local offset (ms, east of UTC) at the UTC instant t, which is finite. */
static int64_t offset_ms(double t)
{
    if (g_pinned) {
        return t < g_pin_transition ? g_pin_before : g_pin_after;
    }
    if (!isfinite(t) || fabs(t) > 1e16) {
        return 0;
    }
    int64_t ms = (int64_t)t;
    int64_t r = (ms - OFFSET_BASE_MS) % OFFSET_CYCLE_MS;
    if (r < 0) {
        r += OFFSET_CYCLE_MS;
    }
    r += OFFSET_BASE_MS;
    SDL_DateTime dt;
    if (!SDL_TimeToDateTime((SDL_Time)r * 1000000, &dt, true)) {
        return 0;
    }
    return (int64_t)dt.utc_offset * 1000;
}

/* LocalTime(t) for a finite UTC time value. */
static double local_from_utc(double t)
{
    if (!isfinite(t)) {
        return NAN;
    }
    return t + (double)offset_ms(t);
}

/* UTC(t) for a local time value, with the spec's disambiguation (ECMA-262 21.4.1.26): an instant
 * in a gap is read with the offset before the transition, and an instant in an overlap takes the
 * earlier of the two. */
static double utc_from_local(double t)
{
    if (!isfinite(t)) {
        return NAN;
    }
    double before = (double)offset_ms(t - MS_PER_DAY);
    double after = (double)offset_ms(t + MS_PER_DAY);
    double u1 = t - before;
    double u2 = t - after;
    bool v1 = (double)offset_ms(u1) == before;
    bool v2 = (double)offset_ms(u2) == after;
    if (v1 && v2) {
        return u1 < u2 ? u1 : u2;
    }
    if (v2) {
        return u2;
    }
    return u1;
}

/* ------------------------------------------------------------------ calendar arithmetic */

static int64_t floor_div64(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        q--;
    }
    return q;
}

/* Days since 1970-01-01 of a proleptic Gregorian date (month 1..12, day 1..31). */
static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2 ? 1 : 0;
    int64_t era = floor_div64(y, 400);
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* The inverse of days_from_civil: month is 1..12. */
static void civil_from_days(int64_t z, int64_t *y, int *m, int *d)
{
    z += 719468;
    int64_t era = floor_div64(z, 146097);
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yy + (*m <= 2 ? 1 : 0);
}

/* The fields of a time value (already local or UTC). month is 0..11, day is 1..31. */
typedef struct tfields {
    int64_t year;
    int month;
    int day;
    int hour;
    int min;
    int sec;
    int ms;
    int wday;
} tfields;

static void decompose(int64_t t, tfields *f)
{
    int64_t days = floor_div64(t, 86400000);
    int64_t rem = t - days * 86400000;
    int m = 0;
    int d = 0;
    civil_from_days(days, &f->year, &m, &d);
    f->month = m - 1;
    f->day = d;
    f->hour = (int)(rem / 3600000);
    rem %= 3600000;
    f->min = (int)(rem / 60000);
    rem %= 60000;
    f->sec = (int)(rem / 1000);
    f->ms = (int)(rem % 1000);
    int64_t wd = (days + 4) % 7;
    f->wday = (int)(wd < 0 ? wd + 7 : wd);
}

/* MakeDay (ECMA-262 21.4.1.28). month is 0-based and may be out of range. */
static double make_day(double year, double month, double date)
{
    if (!isfinite(year) || !isfinite(month) || !isfinite(date)) {
        return NAN;
    }
    double y = trunc(year);
    double m = trunc(month);
    double dt = trunc(date);
    double q = floor(m / 12.0);
    double ym = y + q;
    if (fabs(ym) > 1e12) {
        return NAN;
    }
    int mn = (int)(m - q * 12.0);
    int64_t days = days_from_civil((int64_t)ym, mn + 1, 1);
    return (double)days + dt - 1.0;
}

/* MakeTime (ECMA-262 21.4.1.27). */
static double make_time(double h, double m, double s, double ms)
{
    if (!isfinite(h) || !isfinite(m) || !isfinite(s) || !isfinite(ms)) {
        return NAN;
    }
    return trunc(h) * MS_PER_HOUR + trunc(m) * MS_PER_MIN + trunc(s) * 1000.0 + trunc(ms);
}

/* MakeDate (ECMA-262 21.4.1.29). */
static double make_date(double day, double time)
{
    if (!isfinite(day) || !isfinite(time)) {
        return NAN;
    }
    double tv = day * MS_PER_DAY + time;
    return isfinite(tv) ? tv : NAN;
}

/* TimeClip (ECMA-262 21.4.1.31). The +0 turns -0 into +0. */
static double time_clip(double t)
{
    if (!isfinite(t) || fabs(t) > MAX_TIME_VALUE) {
        return NAN;
    }
    return trunc(t) + 0.0;
}

/* ------------------------------------------------------------------ Date objects */

static bool is_date(embi_js_value v)
{
    return embi_js_is_object(v) && embi_js_as_object(v)->klass == &embi_js_class_date;
}

static double date_value(const embi_js_object *o)
{
    return embi_js_as_double((embi_js_value)(uintptr_t)o->internal);
}

static void set_date_value(embi_js_object *o, double t)
{
    o->internal = (void *)(uintptr_t)embi_js_from_double(t);
    EMBI_JS_WB_STRUCT(o);
}

/* The Date object of a receiver. Throws a TypeError and returns nullptr for other values. */
static embi_js_object *this_date(embi_js_runtime *rt, embi_js_value v)
{
    if (!is_date(v)) {
        embi_js_throw_type_error(rt, "this is not a Date object.");
        return nullptr;
    }
    return embi_js_as_object(v);
}

/* The root record: a one-slot value array holding Date.prototype. The root hook marks it. */
static void date_root_mark(embi_js_tracer *t, void *user)
{
    embi_js_mark_cell(t, user);
}

static embi_js_varray *date_record(embi_js_runtime *rt)
{
    for (uint32_t i = 0; i < rt->roots.len; i++) {
        if (rt->roots.data[i].fn == date_root_mark) {
            return rt->roots.data[i].user;
        }
    }
    EMBI_ASSERT(false);
    return nullptr;
}

static embi_js_value date_proto_value(embi_js_runtime *rt)
{
    return date_record(rt)->elems[0];
}

/* GetPrototypeFromConstructor for Date: the prototype of new_target when it is an object, else
 * the intrinsic fallback. The realm of new_target is not tracked (one realm per runtime). */
static embi_js_value proto_for_new(embi_js_runtime *rt, embi_js_value new_target,
                                   embi_js_value fallback)
{
    if (!embi_js_is_object(new_target)) {
        return fallback;
    }
    embi_js_value p = embi_js_get(rt, embi_js_as_object(new_target),
                                  embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE));
    if (p == EXC) {
        return EXC;
    }
    return embi_js_is_object(p) ? p : fallback;
}

static embi_js_value str_result(embi_js_runtime *rt, const char *s)
{
    embi_js_string *str = embi_js_str_new_utf8(rt, s, strlen(s));
    return str ? embi_js_from_string(str) : EXC;
}

/* ------------------------------------------------------------------ formatting */

static const char *const WEEKDAYS[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const MONTHS[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const WEEKDAYS_LONG[7] = {"sunday",   "monday", "tuesday", "wednesday",
                                             "thursday", "friday", "saturday"};
static const char *const MONTHS_LONG[12] = {"january", "february", "march",     "april",
                                            "may",     "june",     "july",      "august",
                                            "september", "october", "november", "december"};

/* DateString: "Tue Nov 09 2026", with a '-' before years below zero. */
static void put_date(char *b, size_t cap, const tfields *f)
{
    int y = (int)f->year;
    SDL_snprintf(b, cap, "%s %s %02d %s%04d", WEEKDAYS[f->wday], MONTHS[f->month], f->day,
                 y < 0 ? "-" : "", y < 0 ? -y : y);
}

/* TimeString: "10:00:00". */
static void put_time(char *b, size_t cap, const tfields *f)
{
    SDL_snprintf(b, cap, "%02d:%02d:%02d", f->hour, f->min, f->sec);
}

/* TimeZoneString: "GMT+0000 (Coordinated Universal Time)". No zone name is available from SDL, so
 * a name is made from the offset. */
static void put_tz(char *b, size_t cap, int64_t off)
{
    int64_t m = off / 60000;
    char sign = m < 0 ? '-' : '+';
    if (m < 0) {
        m = -m;
    }
    int hh = (int)(m / 60);
    int mm = (int)(m % 60);
    if (off == 0) {
        SDL_snprintf(b, cap, "GMT%c%02d%02d (Coordinated Universal Time)", sign, hh, mm);
    } else {
        SDL_snprintf(b, cap, "GMT%c%02d%02d (UTC%c%02d:%02d)", sign, hh, mm, sign, hh, mm);
    }
}

enum date_format {
    DF_FULL,        /* toString */
    DF_DATE,        /* toDateString */
    DF_TIME,        /* toTimeString */
    DF_UTC,         /* toUTCString, toGMTString */
    DF_LOCALE,      /* toLocaleString, en-US */
    DF_LOCALE_DATE, /* toLocaleDateString, en-US */
    DF_LOCALE_TIME, /* toLocaleTimeString, en-US */
};

static embi_js_value format_date(embi_js_runtime *rt, double t, enum date_format kind)
{
    if (isnan(t)) {
        return str_result(rt, "Invalid Date");
    }
    int64_t off = offset_ms(t);
    tfields lf;
    char date[64];
    char tm[32];
    char tz[96];
    char buf[256];
    decompose((int64_t)t + off, &lf);
    switch (kind) {
    case DF_FULL:
        put_date(date, sizeof date, &lf);
        put_time(tm, sizeof tm, &lf);
        put_tz(tz, sizeof tz, off);
        SDL_snprintf(buf, sizeof buf, "%s %s %s", date, tm, tz);
        break;
    case DF_DATE:
        put_date(date, sizeof date, &lf);
        SDL_snprintf(buf, sizeof buf, "%s", date);
        break;
    case DF_TIME:
        put_time(tm, sizeof tm, &lf);
        put_tz(tz, sizeof tz, off);
        SDL_snprintf(buf, sizeof buf, "%s %s", tm, tz);
        break;
    case DF_UTC: {
        tfields uf;
        decompose((int64_t)t, &uf);
        int y = (int)uf.year;
        SDL_snprintf(buf, sizeof buf, "%s, %02d %s %s%04d %02d:%02d:%02d GMT",
                     WEEKDAYS[uf.wday], uf.day, MONTHS[uf.month], y < 0 ? "-" : "",
                     y < 0 ? -y : y, uf.hour, uf.min, uf.sec);
        break;
    }
    case DF_LOCALE:
    case DF_LOCALE_DATE:
    case DF_LOCALE_TIME: {
        int h12 = lf.hour % 12 == 0 ? 12 : lf.hour % 12;
        const char *ampm = lf.hour < 12 ? "AM" : "PM";
        char d[48];
        char hm[48];
        SDL_snprintf(d, sizeof d, "%d/%d/%d", lf.month + 1, lf.day, (int)lf.year);
        SDL_snprintf(hm, sizeof hm, "%d:%02d:%02d %s", h12, lf.min, lf.sec, ampm);
        if (kind == DF_LOCALE) {
            SDL_snprintf(buf, sizeof buf, "%s, %s", d, hm);
        } else if (kind == DF_LOCALE_DATE) {
            SDL_snprintf(buf, sizeof buf, "%s", d);
        } else {
            SDL_snprintf(buf, sizeof buf, "%s", hm);
        }
        break;
    }
    }
    return str_result(rt, buf);
}

/* ------------------------------------------------------------------ parsing */

typedef struct src {
    const embi_js_string *s;
    uint32_t n;
} src;

/* The code unit at i, or -1 past the end. */
static int cu(const src *z, uint32_t i)
{
    return i < z->n ? (int)embi_js_str_at(z->s, i) : -1;
}

static bool is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static bool is_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int lower_ascii(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* Reads exactly n digits at *p and advances past them. */
static bool read_fixed(const src *z, uint32_t *p, int n, int *out)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        int c = cu(z, *p + (uint32_t)i);
        if (!is_digit(c)) {
            return false;
        }
        v = v * 10 + (c - '0');
    }
    *p += (uint32_t)n;
    *out = v;
    return true;
}

/* The Date Time String Format (ECMA-262 21.4.1.32). Returns false when the text is not in that
 * format; then *out is NaN for an out-of-range field, and a time value otherwise. */
static bool parse_iso(const src *z, double *out)
{
    uint32_t p = 0;
    int year = 0;
    int c = cu(z, 0);
    if (c == '+' || c == '-') {
        int sign = c == '-' ? -1 : 1;
        int y6 = 0;
        p = 1;
        if (!read_fixed(z, &p, 6, &y6)) {
            return false;
        }
        if (sign < 0 && y6 == 0) {
            /* -000000 is not a valid year: the text is in the format, but the value is NaN. */
            *out = NAN;
            return true;
        }
        year = sign * y6;
    } else if (!read_fixed(z, &p, 4, &year)) {
        return false;
    }
    int month = 1;
    int day = 1;
    int hour = 0;
    int min = 0;
    int sec = 0;
    int ms = 0;
    int off = 0;
    bool has_time = false;
    bool has_tz = false;
    if (cu(z, p) == '-') {
        p++;
        if (!read_fixed(z, &p, 2, &month)) {
            return false;
        }
        if (cu(z, p) == '-') {
            p++;
            if (!read_fixed(z, &p, 2, &day)) {
                return false;
            }
        }
    }
    if (cu(z, p) == 'T') {
        p++;
        if (!read_fixed(z, &p, 2, &hour) || cu(z, p) != ':') {
            return false;
        }
        p++;
        if (!read_fixed(z, &p, 2, &min)) {
            return false;
        }
        has_time = true;
        if (cu(z, p) == ':') {
            p++;
            if (!read_fixed(z, &p, 2, &sec)) {
                return false;
            }
            if (cu(z, p) == '.') {
                p++;
                if (!read_fixed(z, &p, 3, &ms)) {
                    return false;
                }
            }
        }
        c = cu(z, p);
        if (c == 'Z') {
            p++;
            has_tz = true;
        } else if (c == '+' || c == '-') {
            int s2 = c == '-' ? -1 : 1;
            int oh = 0;
            int om = 0;
            p++;
            if (!read_fixed(z, &p, 2, &oh) || cu(z, p) != ':') {
                return false;
            }
            p++;
            if (!read_fixed(z, &p, 2, &om)) {
                return false;
            }
            if (oh > 23 || om > 59) {
                return false;
            }
            off = s2 * (oh * 60 + om);
            has_tz = true;
        }
    }
    if (p != z->n) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 24 || min > 59 || sec > 59 ||
        (hour == 24 && (min != 0 || sec != 0 || ms != 0))) {
        *out = NAN;
        return true;
    }
    double t = make_date(make_day(year, month - 1, day), make_time(hour, min, sec, ms));
    if (has_tz) {
        t -= (double)off * MS_PER_MIN;
    } else if (has_time) {
        t = utc_from_local(t);
    }
    *out = time_clip(t);
    return true;
}

/* A number read from the text. */
typedef struct num_tok {
    int v; /* signed by a preceding '-' sign */
    int digits;
} num_tok;

static bool read_digits(const src *z, uint32_t *p, int *v, int *digits)
{
    int val = 0;
    int nd = 0;
    while (is_digit(cu(z, *p))) {
        if (nd >= 9) {
            return false;
        }
        val = val * 10 + (cu(z, *p) - '0');
        nd++;
        (*p)++;
    }
    *v = val;
    *digits = nd;
    return nd > 0;
}

/* The lenient legacy format (what browsers accept beyond the ISO form): month names, weekday
 * names (ignored), time with an optional AM or PM, numeric dates separated by / or -, a GMT or UTC
 * marker or a +hhmm offset, US zone abbreviations and parenthesized comments. Returns NaN for any
 * text it cannot read. */
static double parse_legacy(const src *z)
{
    uint32_t p = 0;
    uint32_t n = z->n;
    int month = -1;
    bool have_month = false;
    bool have_time = false;
    bool have_tz = false;
    bool ampm = false;
    bool pm = false;
    int hour = 0;
    int min = 0;
    int sec = 0;
    int ms = 0;
    int off = 0;
    bool pending_neg = false;
    num_tok dn[3];
    int ndn = 0;
    num_tok ln[4];
    int nln = 0;

    while (p < n) {
        int c = cu(z, p);
        if (c == '(') {
            int depth = 0;
            do {
                c = cu(z, p);
                if (c == '(') {
                    depth++;
                } else if (c == ')') {
                    depth--;
                }
                p++;
            } while (p < n && depth > 0);
            continue;
        }
        if (c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r') {
            p++;
            continue;
        }
        if (is_alpha(c)) {
            char w[16];
            int wl = 0;
            while (is_alpha(cu(z, p))) {
                if (wl >= 15) {
                    return NAN;
                }
                w[wl++] = (char)lower_ascii(cu(z, p));
                p++;
            }
            w[wl] = 0;
            bool matched = false;
            for (int i = 0; i < 12 && !matched; i++) {
                if (wl >= 3 && wl <= (int)strlen(MONTHS_LONG[i]) &&
                    strncmp(MONTHS_LONG[i], w, (size_t)wl) == 0) {
                    month = i;
                    have_month = true;
                    matched = true;
                }
            }
            for (int i = 0; i < 7 && !matched; i++) {
                if (wl >= 3 && wl <= (int)strlen(WEEKDAYS_LONG[i]) &&
                    strncmp(WEEKDAYS_LONG[i], w, (size_t)wl) == 0) {
                    matched = true;
                }
            }
            static const struct {
                const char *name;
                int minutes;
            } zones[] = {
                {"ut", 0},    {"utc", 0},   {"gmt", 0},    {"z", 0},     {"est", -300},
                {"edt", -240}, {"cst", -360}, {"cdt", -300}, {"mst", -420}, {"mdt", -360},
                {"pst", -480}, {"pdt", -420},
            };
            for (size_t i = 0; i < sizeof zones / sizeof zones[0] && !matched; i++) {
                if (strcmp(zones[i].name, w) == 0) {
                    off = zones[i].minutes;
                    have_tz = true;
                    matched = true;
                }
            }
            if (!matched) {
                if (strcmp(w, "am") == 0 || strcmp(w, "pm") == 0) {
                    ampm = true;
                    pm = w[0] == 'p';
                } else if (strcmp(w, "t") != 0) {
                    return NAN;
                }
            }
            continue;
        }
        if (c == '+' || c == '-') {
            int nx = cu(z, p + 1);
            if ((have_time || have_tz) && is_digit(nx)) {
                int sgn = c == '-' ? -1 : 1;
                p++;
                int v = 0;
                int nd = 0;
                if (!read_digits(z, &p, &v, &nd)) {
                    return NAN;
                }
                int oh = 0;
                int om = 0;
                if (nd == 4) {
                    oh = v / 100;
                    om = v % 100;
                } else if (nd <= 2) {
                    oh = v;
                    if (cu(z, p) == ':') {
                        p++;
                        if (!read_digits(z, &p, &om, &nd) || nd != 2) {
                            return NAN;
                        }
                    }
                } else {
                    return NAN;
                }
                if (oh > 23 || om > 59) {
                    return NAN;
                }
                off = sgn * (oh * 60 + om);
                have_tz = true;
                continue;
            }
            if (is_digit(nx)) {
                pending_neg = c == '-';
            }
            p++;
            continue;
        }
        if (is_digit(c)) {
            int v = 0;
            int nd = 0;
            if (!read_digits(z, &p, &v, &nd)) {
                return NAN;
            }
            num_tok tok;
            tok.v = pending_neg ? -v : v;
            tok.digits = nd;
            pending_neg = false;
            int cc = cu(z, p);
            if (cc == ':' && !have_time) {
                hour = tok.v;
                p++;
                int nd2 = 0;
                if (!read_digits(z, &p, &min, &nd2) || nd2 > 2) {
                    return NAN;
                }
                if (cu(z, p) == ':') {
                    p++;
                    if (!read_digits(z, &p, &sec, &nd2) || nd2 > 2) {
                        return NAN;
                    }
                }
                if (cu(z, p) == '.') {
                    p++;
                    int frac = 0;
                    int fd = 0;
                    while (is_digit(cu(z, p))) {
                        if (fd < 3) {
                            frac = frac * 10 + (cu(z, p) - '0');
                            fd++;
                        }
                        p++;
                    }
                    if (fd == 0) {
                        return NAN;
                    }
                    while (fd < 3) {
                        frac *= 10;
                        fd++;
                    }
                    ms = frac;
                }
                have_time = true;
                continue;
            }
            if (cc == '/' || (cc == '-' && is_digit(cu(z, p + 1)))) {
                if (ndn >= 3) {
                    return NAN;
                }
                dn[ndn++] = tok;
                while ((cu(z, p) == '/' || cu(z, p) == '-') && is_digit(cu(z, p + 1))) {
                    p++;
                    int v2 = 0;
                    int nd3 = 0;
                    if (ndn >= 3 || !read_digits(z, &p, &v2, &nd3)) {
                        return NAN;
                    }
                    dn[ndn].v = v2;
                    dn[ndn].digits = nd3;
                    ndn++;
                }
                continue;
            }
            if (nln >= 4) {
                return NAN;
            }
            ln[nln++] = tok;
            continue;
        }
        p++;
    }

    int y = 0;
    int mo = 0;
    int d = 1;
    bool year_digits_short = false;
    if (ndn > 0) {
        if (ndn != 3 || have_month) {
            return NAN;
        }
        if (dn[0].digits >= 3 || dn[0].v > 31 || dn[0].v < 0) {
            y = dn[0].v;
            mo = dn[1].v - 1;
            d = dn[2].v;
            year_digits_short = dn[0].digits <= 2 && dn[0].v >= 0;
        } else {
            mo = dn[0].v - 1;
            d = dn[1].v;
            y = dn[2].v;
            year_digits_short = dn[2].digits <= 2 && dn[2].v >= 0;
        }
    } else {
        if (!have_month || nln == 0) {
            return NAN;
        }
        mo = month;
        int yi = -1;
        for (int i = 0; i < nln && yi < 0; i++) {
            if (ln[i].digits >= 3 || ln[i].v > 31 || ln[i].v < 0) {
                yi = i;
            }
        }
        if (yi < 0) {
            if (nln < 2) {
                return NAN;
            }
            yi = nln - 1;
        }
        y = ln[yi].v;
        year_digits_short = ln[yi].digits <= 2 && ln[yi].v >= 0;
        for (int i = 0; i < nln; i++) {
            if (i != yi) {
                d = ln[i].v;
                break;
            }
        }
    }
    if (year_digits_short) {
        y += y < 50 ? 2000 : 1900;
    }
    if (ampm) {
        if (hour > 12) {
            return NAN;
        }
        if (pm && hour < 12) {
            hour += 12;
        } else if (!pm && hour == 12) {
            hour = 0;
        }
    }
    if (mo < 0 || mo > 11 || d < 1 || d > 31 || hour > 24 || min > 59 || sec > 59 ||
        (hour == 24 && (min != 0 || sec != 0 || ms != 0))) {
        return NAN;
    }
    double t = make_date(make_day(y, mo, d), make_time(hour, min, sec, ms));
    if (have_tz) {
        t -= (double)off * MS_PER_MIN;
    } else {
        t = utc_from_local(t);
    }
    return time_clip(t);
}

/* Date.parse on a string: the ISO format first, then the legacy format. */
static double parse_date_string(const embi_js_string *s)
{
    src z = {s, embi_js_str_len(s)};
    for (uint32_t i = 0; i < z.n; i++) {
        if (embi_js_str_at(s, i) > 0x7F) {
            return NAN;
        }
    }
    double t = NAN;
    if (parse_iso(&z, &t)) {
        return t;
    }
    return parse_legacy(&z);
}

/* ------------------------------------------------------------------ constructor helpers */

static double now_ms(void)
{
    SDL_Time ns = 0;
    if (!SDL_GetCurrentTime(&ns)) {
        return 0;
    }
    return (double)(ns / 1000000);
}

/* MakeDate of the first argument list (year, month, date, hours, minutes, seconds, ms), as the
 * Date constructor and Date.UTC read it. The result is neither UTC-converted nor clipped. Missing
 * fields default as the spec says: date is 1, the others 0. Every argument is converted, in order,
 * up to the seventh. Returns false when a conversion throws. */
static bool components_date(embi_js_runtime *rt, int argc, const embi_js_value *argv, double *out)
{
    double c[7] = {NAN, 0, 1, 0, 0, 0, 0};
    for (int i = 0; i < argc && i < 7; i++) {
        if (!embi_js_to_number(rt, argv[i], &c[i])) {
            return false;
        }
    }
    double yr = c[0];
    if (!isnan(yr)) {
        double yi = trunc(yr);
        if (yi >= 0 && yi <= 99) {
            yr = 1900 + yi;
        }
    }
    *out = make_date(make_day(yr, c[1], c[2]), make_time(c[3], c[4], c[5], c[6]));
    return true;
}

/* ------------------------------------------------------------------ constructor and statics */

static embi_js_value date_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic, embi_js_value new_target)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (embi_js_is_undefined(new_target)) {
        return format_date(rt, time_clip(now_ms()), DF_FULL);
    }
    double tv = NAN;
    if (argc == 0) {
        tv = time_clip(now_ms());
    } else if (argc == 1) {
        embi_js_value v = argv[0];
        if (is_date(v)) {
            tv = date_value(embi_js_as_object(v));
        } else {
            embi_js_value p = embi_js_to_primitive(rt, v, EMBI_JS_HINT_DEFAULT);
            if (p == EXC) {
                return EXC;
            }
            if (embi_js_is_string(p)) {
                tv = parse_date_string(embi_js_str_flat(rt, p));
            } else {
                double d = NAN;
                if (!embi_js_to_number(rt, p, &d)) {
                    return EXC;
                }
                tv = time_clip(d);
            }
        }
    } else {
        double local = NAN;
        if (!components_date(rt, argc, argv, &local)) {
            return EXC;
        }
        tv = time_clip(utc_from_local(local));
    }
    embi_js_value proto = proto_for_new(rt, new_target, date_proto_value(rt));
    if (proto == EXC) {
        return EXC;
    }
    embi_js_object *o = embi_js_object_new_class(rt, proto, &embi_js_class_date);
    set_date_value(o, tv);
    return embi_js_from_object(o);
}

static embi_js_value date_now(embi_js_realm *r, embi_js_value this_v, int argc,
                              const embi_js_value *argv, int magic)
{
    (void)r;
    (void)this_v;
    (void)argc;
    (void)argv;
    (void)magic;
    return embi_js_number(now_ms());
}

static embi_js_value date_parse(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value arg = argc > 0 ? argv[0] : UNDEF;
    embi_js_string *s = embi_js_to_string(rt, arg);
    if (!s) {
        return EXC;
    }
    return embi_js_number(parse_date_string(s));
}

static embi_js_value date_utc(embi_js_realm *r, embi_js_value this_v, int argc,
                              const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    double local = NAN;
    if (!components_date(r->rt, argc, argv, &local)) {
        return EXC;
    }
    return embi_js_number(time_clip(local));
}

/* ------------------------------------------------------------------ prototype: getters */

enum getter_kind {
    G_DATE,
    G_DAY,
    G_FULLYEAR,
    G_HOURS,
    G_MILLISECONDS,
    G_MINUTES,
    G_MONTH,
    G_SECONDS,
    G_TIME,
    G_TIMEZONE_OFFSET,
    G_YEAR, /* Annex B getYear: local year minus 1900 */
};

static embi_js_value date_getter(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    embi_js_object *o = this_date(r->rt, this_v);
    if (!o) {
        return EXC;
    }
    double t = date_value(o);
    int kind = magic & 0xff;
    bool utc = (magic & UTC_BIT) != 0;
    if (kind == G_TIME) {
        return embi_js_number(t);
    }
    if (isnan(t)) {
        return embi_js_number(NAN);
    }
    if (kind == G_TIMEZONE_OFFSET) {
        /* (t - LocalTime(t)) / msPerMinute, written so that a zero offset gives +0. */
        return embi_js_number(0.0 - (double)offset_ms(t) / MS_PER_MIN);
    }
    double lt = utc ? t : local_from_utc(t);
    tfields f;
    decompose((int64_t)lt, &f);
    switch (kind) {
    case G_DATE:
        return embi_js_number(f.day);
    case G_DAY:
        return embi_js_number(f.wday);
    case G_FULLYEAR:
        return embi_js_number((double)f.year);
    case G_HOURS:
        return embi_js_number(f.hour);
    case G_MILLISECONDS:
        return embi_js_number(f.ms);
    case G_MINUTES:
        return embi_js_number(f.min);
    case G_MONTH:
        return embi_js_number(f.month);
    case G_SECONDS:
        return embi_js_number(f.sec);
    case G_YEAR:
        return embi_js_number((double)f.year - 1900);
    }
    return embi_js_number(NAN);
}

/* ------------------------------------------------------------------ prototype: setters */

enum setter_kind {
    S_DATE,
    S_FULLYEAR,
    S_HOURS,
    S_MILLISECONDS,
    S_MINUTES,
    S_MONTH,
    S_SECONDS,
    S_TIME,
    S_YEAR, /* Annex B setYear */
};

/* The fields a setter writes: the first field index and the most arguments it reads. */
enum date_field { F_YEAR, F_MONTH, F_DATE, F_HOUR, F_MIN, F_SEC, F_MS };

static const struct {
    uint8_t first;
    uint8_t max;
} setter_fields[] = {
    [S_DATE] = {F_DATE, 1},
    [S_FULLYEAR] = {F_YEAR, 3},
    [S_HOURS] = {F_HOUR, 4},
    [S_MILLISECONDS] = {F_MS, 1},
    [S_MINUTES] = {F_MIN, 3},
    [S_MONTH] = {F_MONTH, 2},
    [S_SECONDS] = {F_SEC, 2},
};

static embi_js_value date_setter(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = this_date(rt, this_v);
    if (!o) {
        return EXC;
    }
    int kind = magic & 0xff;
    bool utc = (magic & UTC_BIT) != 0;
    /* The time value is read before the arguments are converted (a valueOf may change it). */
    double t = date_value(o);
    int count = argc < setter_fields[kind].max ? argc : setter_fields[kind].max;
    if (count < 1) {
        count = 1; /* a missing first argument is undefined, which converts to NaN */
    }
    double vals[4] = {NAN, NAN, NAN, NAN};
    for (int i = 0; i < count; i++) {
        embi_js_value a = i < argc ? argv[i] : UNDEF;
        if (!embi_js_to_number(rt, a, &vals[i])) {
            return EXC;
        }
    }
    bool year_setter = kind == S_FULLYEAR;
    double base;
    if (isnan(t)) {
        if (!year_setter) {
            return embi_js_number(NAN);
        }
        base = 0;
    } else {
        base = utc ? t : local_from_utc(t);
    }
    tfields f;
    decompose((int64_t)base, &f);
    double a[7] = {(double)f.year, f.month, f.day, f.hour, f.min, f.sec, f.ms};
    for (int i = 0; i < count; i++) {
        a[setter_fields[kind].first + i] = vals[i];
    }
    double u = make_date(make_day(a[0], a[1], a[2]), make_time(a[3], a[4], a[5], a[6]));
    if (!utc) {
        u = utc_from_local(u);
    }
    u = time_clip(u);
    set_date_value(o, u);
    return embi_js_number(u);
}

static embi_js_value date_set_time(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = this_date(rt, this_v);
    if (!o) {
        return EXC;
    }
    double v = NAN;
    if (!embi_js_to_number(rt, argc > 0 ? argv[0] : UNDEF, &v)) {
        return EXC;
    }
    double u = time_clip(v);
    set_date_value(o, u);
    return embi_js_number(u);
}

/* Annex B setYear (B.2.3.2). */
static embi_js_value date_set_year(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = this_date(rt, this_v);
    if (!o) {
        return EXC;
    }
    double t = date_value(o);
    double y = NAN;
    if (!embi_js_to_number(rt, argc > 0 ? argv[0] : UNDEF, &y)) {
        return EXC;
    }
    t = isnan(t) ? 0 : local_from_utc(t);
    if (isnan(y)) {
        set_date_value(o, NAN);
        return embi_js_number(NAN);
    }
    double yi = trunc(y);
    double yyyy = (yi >= 0 && yi <= 99) ? 1900 + yi : y;
    tfields f;
    decompose((int64_t)t, &f);
    double day = make_day(yyyy, f.month, f.day);
    double date = make_date(day, make_time(f.hour, f.min, f.sec, f.ms));
    double u = time_clip(utc_from_local(date));
    set_date_value(o, u);
    return embi_js_number(u);
}

/* ------------------------------------------------------------------ prototype: strings */

static embi_js_value date_format_method(embi_js_realm *r, embi_js_value this_v, int argc,
                                        const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    embi_js_object *o = this_date(r->rt, this_v);
    if (!o) {
        return EXC;
    }
    return format_date(r->rt, date_value(o), (enum date_format)magic);
}

static embi_js_value date_to_iso_string(embi_js_realm *r, embi_js_value this_v, int argc,
                                        const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = this_date(rt, this_v);
    if (!o) {
        return EXC;
    }
    double t = date_value(o);
    if (isnan(t)) {
        return embi_js_throw_range_error(rt, "Invalid time value");
    }
    tfields f;
    decompose((int64_t)t, &f);
    char buf[64];
    if (f.year >= 0 && f.year <= 9999) {
        SDL_snprintf(buf, sizeof buf, "%04d", (int)f.year);
    } else {
        SDL_snprintf(buf, sizeof buf, "%c%06d", f.year < 0 ? '-' : '+',
                     (int)(f.year < 0 ? -f.year : f.year));
    }
    size_t n = strlen(buf);
    SDL_snprintf(buf + n, sizeof buf - n, "-%02d-%02dT%02d:%02d:%02d.%03dZ", f.month + 1, f.day,
                 f.hour, f.min, f.sec, f.ms);
    return str_result(rt, buf);
}

/* Date.prototype.toJSON (ECMA-262 21.4.4.37). */
static embi_js_value date_to_json(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    embi_js_value self = embi_js_from_object(o);
    embi_js_value tv = embi_js_to_primitive(rt, self, EMBI_JS_HINT_NUMBER);
    if (tv == EXC) {
        return EXC;
    }
    if (embi_js_is_number(tv)) {
        double d = embi_js_as_double(tv);
        if (!isfinite(d)) {
            return EMBI_JS_NULL;
        }
    }
    return embi_js_invoke(rt, self, embi_js_key_c(rt, "toISOString"), 0, nullptr);
}

/* OrdinaryToPrimitive (ECMA-262 7.1.1.1). */
static embi_js_value ordinary_to_primitive(embi_js_runtime *rt, embi_js_object *o,
                                           bool string_first)
{
    embi_js_value self = embi_js_from_object(o);
    enum embi_js_name names[2] = {EMBI_JS_NAME_TO_STRING, EMBI_JS_NAME_VALUE_OF};
    if (!string_first) {
        names[0] = EMBI_JS_NAME_VALUE_OF;
        names[1] = EMBI_JS_NAME_TO_STRING;
    }
    for (int i = 0; i < 2; i++) {
        embi_js_value fn = embi_js_get(rt, o, embi_js_name_val(rt, names[i]));
        if (fn == EXC) {
            return EXC;
        }
        if (embi_js_is_callable(fn)) {
            embi_js_value res = embi_js_call(rt, fn, self, 0, nullptr);
            if (res == EXC) {
                return EXC;
            }
            if (!embi_js_is_object(res)) {
                return res;
            }
        }
    }
    return embi_js_throw_type_error(rt, "Cannot convert object to primitive value");
}

static bool str_equals(const embi_js_string *s, const char *lit)
{
    size_t n = strlen(lit);
    if (embi_js_str_len(s) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (embi_js_str_at(s, (uint32_t)i) != (uint8_t)lit[i]) {
            return false;
        }
    }
    return true;
}

/* Date.prototype[@@toPrimitive] (ECMA-262 21.4.4.45). */
static embi_js_value date_to_primitive(embi_js_realm *r, embi_js_value this_v, int argc,
                                       const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!embi_js_is_object(this_v)) {
        return embi_js_throw_type_error(rt, "Date.prototype[Symbol.toPrimitive] called on a "
                                            "non-object");
    }
    embi_js_value hint = argc > 0 ? argv[0] : UNDEF;
    if (!embi_js_is_string(hint)) {
        return embi_js_throw_type_error(rt, "Invalid hint");
    }
    const embi_js_string *hs = embi_js_str_flat(rt, hint);
    bool string_first;
    if (str_equals(hs, "string") || str_equals(hs, "default")) {
        string_first = true;
    } else if (str_equals(hs, "number")) {
        string_first = false;
    } else {
        return embi_js_throw_type_error(rt, "Invalid hint");
    }
    return ordinary_to_primitive(rt, embi_js_as_object(this_v), string_first);
}

/* ------------------------------------------------------------------ installation */

typedef struct method_def {
    const char *name;
    embi_js_native fn;
    int length;
    int magic;
} method_def;

static const method_def getter_defs[] = {
    {"getDate", date_getter, 0, G_DATE},
    {"getDay", date_getter, 0, G_DAY},
    {"getFullYear", date_getter, 0, G_FULLYEAR},
    {"getHours", date_getter, 0, G_HOURS},
    {"getMilliseconds", date_getter, 0, G_MILLISECONDS},
    {"getMinutes", date_getter, 0, G_MINUTES},
    {"getMonth", date_getter, 0, G_MONTH},
    {"getSeconds", date_getter, 0, G_SECONDS},
    {"getTime", date_getter, 0, G_TIME},
    {"getTimezoneOffset", date_getter, 0, G_TIMEZONE_OFFSET},
    {"getUTCDate", date_getter, 0, G_DATE | UTC_BIT},
    {"getUTCDay", date_getter, 0, G_DAY | UTC_BIT},
    {"getUTCFullYear", date_getter, 0, G_FULLYEAR | UTC_BIT},
    {"getUTCHours", date_getter, 0, G_HOURS | UTC_BIT},
    {"getUTCMilliseconds", date_getter, 0, G_MILLISECONDS | UTC_BIT},
    {"getUTCMinutes", date_getter, 0, G_MINUTES | UTC_BIT},
    {"getUTCMonth", date_getter, 0, G_MONTH | UTC_BIT},
    {"getUTCSeconds", date_getter, 0, G_SECONDS | UTC_BIT},
    {"getYear", date_getter, 0, G_YEAR},
    {"valueOf", date_getter, 0, G_TIME},
};

static const method_def setter_defs[] = {
    {"setDate", date_setter, 1, S_DATE},
    {"setFullYear", date_setter, 3, S_FULLYEAR},
    {"setHours", date_setter, 4, S_HOURS},
    {"setMilliseconds", date_setter, 1, S_MILLISECONDS},
    {"setMinutes", date_setter, 3, S_MINUTES},
    {"setMonth", date_setter, 2, S_MONTH},
    {"setSeconds", date_setter, 2, S_SECONDS},
    {"setTime", date_set_time, 1, S_TIME},
    {"setUTCDate", date_setter, 1, S_DATE | UTC_BIT},
    {"setUTCFullYear", date_setter, 3, S_FULLYEAR | UTC_BIT},
    {"setUTCHours", date_setter, 4, S_HOURS | UTC_BIT},
    {"setUTCMilliseconds", date_setter, 1, S_MILLISECONDS | UTC_BIT},
    {"setUTCMinutes", date_setter, 3, S_MINUTES | UTC_BIT},
    {"setUTCMonth", date_setter, 2, S_MONTH | UTC_BIT},
    {"setUTCSeconds", date_setter, 2, S_SECONDS | UTC_BIT},
    {"setYear", date_set_year, 1, S_YEAR},
};

static const method_def string_defs[] = {
    {"toDateString", date_format_method, 0, DF_DATE},
    {"toLocaleDateString", date_format_method, 0, DF_LOCALE_DATE},
    {"toLocaleString", date_format_method, 0, DF_LOCALE},
    {"toLocaleTimeString", date_format_method, 0, DF_LOCALE_TIME},
    {"toString", date_format_method, 0, DF_FULL},
    {"toTimeString", date_format_method, 0, DF_TIME},
    {"toUTCString", date_format_method, 0, DF_UTC},
    {"toISOString", date_to_iso_string, 0, 0},
    {"toJSON", date_to_json, 1, 0},
};

static void define_table(embi_js_realm *r, embi_js_object *o, const method_def *defs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        embi_js_define_method(r, o, defs[i].name, defs[i].fn, defs[i].length, defs[i].magic);
    }
}

void embi_js_init_builtins_date(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_varray *rec = embi_js_varray_new(rt, 1);
    embi_js_root_add(rt, date_root_mark, rec);

    embi_js_object *proto = embi_js_object_new(rt, rt->protos[EMBI_JS_PROTO_OBJECT]);
    EMBI_JS_WB(rec, proto);
    EMBI_JS_STORE(rec, rec->elems[0], embi_js_from_object(proto));

    embi_js_object *ctor = embi_js_native_constructor_new(r, "Date", 7, date_ctor, 0);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(proto), 0);
    embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(ctor), ATTR_WC);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "Date"), embi_js_from_object(ctor),
                       ATTR_WC);

    embi_js_define_method(r, ctor, "now", date_now, 0, 0);
    embi_js_define_method(r, ctor, "parse", date_parse, 1, 0);
    embi_js_define_method(r, ctor, "UTC", date_utc, 7, 0);

    define_table(r, proto, getter_defs, sizeof getter_defs / sizeof getter_defs[0]);
    define_table(r, proto, setter_defs, sizeof setter_defs / sizeof setter_defs[0]);
    define_table(r, proto, string_defs, sizeof string_defs / sizeof string_defs[0]);

    /* Annex B toGMTString is the same function object as toUTCString. */
    embi_js_value utc_fn = embi_js_get(rt, proto, embi_js_key_c(rt, "toUTCString"));
    embi_js_define_raw(rt, proto, embi_js_key_c(rt, "toGMTString"), utc_fn, ATTR_WC);

    embi_js_object *tp = embi_js_native_function_new(r, "[Symbol.toPrimitive]", 1,
                                                     date_to_primitive, 0);
    embi_js_define_raw(rt, proto, embi_js_wk_sym(rt, EMBI_JS_WK_TO_PRIMITIVE),
                       embi_js_from_object(tp), ATTR_C);
}
