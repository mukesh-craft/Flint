// flint_time.h — civil time, timezones, and ISO-8601 formatting.
//
// Two representations, deliberately separate:
//
//   * "local" civil time   -- year..nanosecond with no zone attached. This is
//     what a calendar is: 2026-10-07 09:30:00 is a fact, "3pm" is not.
//   * "instant"            -- seconds since the Unix epoch (UTC), plus a
//     zone name for display.
//
// A timezone is a set of UTC-offset RULES (DST included), not a fixed offset.
// The built-in table covers ~100 zones with yearly-recurrence rules verified
// against IANA tzdb-2026e (US 2nd-Sun-Mar/1st-Sun-Nov, EU last-Sun, Sydney
// 1st-Sun-Oct/Apr, NZ last-Sun-Sep, Cairo last-Fri-Apr/last-Thu-Oct, Havana,
// Santiago, Lord Howe :30 shift, Chatham :45), plus fixed-offset zones
// (Kathmandu +5:45, Tehran +3:30, Guam +10 after a +9 data fix). There is no
// TZif parser: Android ships a compiled tzdata blob, not a zoneinfo tree.
// Zones not in the table resolve to UTC and say so, rather than silently
// claiming a zone they do not have.
//
// Known limits (documented, not hidden): "IST" means India (+5:30); Ireland
// and Israel need Europe/Dublin / an Asia/Jerusalem entry that does not exist
// yet. Africa/Casablanca is fixed +0 per tzdb-2026e (DST rules ended
// 2026-09-20; Moroccan Ramadan suspensions were yearly decrees no fixed
// rule can model). Historical pre-1970 DST rules are not modelled; fixed
// offsets are exact for all instants, DST rules for 1900-2100 wall dates.
//
// Rules are expressed as transitions, not as "is DST": an offset is a
// function of the instant, and the rule set for a zone says when it changes.
// That is what makes DST and historical offsets fall out rather than being a
// special case.

#ifndef FLINT_TIME_H
#define FLINT_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Civil time. Valid ranges: year 1..9999, month 1..12, day 1..31.
typedef struct {
    int32_t year;
    int32_t month;        // 1-12
    int32_t day;          // 1-31
    int32_t hour;         // 0-23
    int32_t minute;       // 0-59
    int32_t second;       // 0-60 (leap second)
    int32_t nanosecond;   // 0-999999999
    int32_t weekday;      // 0 = Sunday .. 6 = Saturday
    int32_t yearday;      // 0-365, day 0 = Jan 1
} FlintDateTime;

typedef struct {
    int32_t offset_seconds;  // east of UTC, e.g. -18000 for EST
    const char* abbrev;     // "UTC", "IST", "EST" ...
    int32_t is_dst;
} FlintZone;

// ---- calendar ------------------------------------------------------------
// Days in a month, honouring the Gregorian leap rule.
int64_t flint_days_in_month(int32_t year, int32_t month);
int64_t flint_is_leap(int64_t year);
// Proleptic Gregorian day number (1970-01-01 == 0). Valid far beyond int32
// years, which int32 arithmetic would not survive.
int64_t flint_days_from_civil(int32_t y, int32_t m, int32_t d);
void    flint_civil_from_days(int64_t z, int32_t* y, int32_t* m, int32_t* d);
int64_t flint_weekday_from_days(int64_t z);          // 0 = Sunday
// Epoch <-> civil. Both directions round-trip exactly. The four
// constructors return heap-allocated, opaque FlintDateTime* (never a
// struct by value: Flint calls across FFI are pointer-sized); release
// with flint_dt_free.
int64_t       flint_timegm(const FlintDateTime* dt);
FlintDateTime* flint_gmtime(int64_t epoch);
void flint_datetime_now(FlintDateTime* dt, int64_t epoch);
FlintDateTime* flint_datetime_make(int32_t y, int32_t mon, int32_t d,
                                   int32_t h, int32_t mi, int32_t s);
void flint_dt_free(FlintDateTime* t);

// ---- timezones -----------------------------------------------------------
// "UTC", "EST", "PST8PDT", "CET", "JST", "AEST", "Europe/Paris" etc., plus
// generic "UTC+HH[:MM]" names. An unknown name returns a UTC zone rather
// than failing, because refusing to format a time is worse than formatting
// it with the wrong offset -- and the caller can see which via abbrev.
// "local" defers to the process TZ and counts as known.
// UTC offset in effect at `epoch` for `name`. This is where DST is applied.
int64_t    flint_tz_offset_at(const char* name, int64_t epoch);
// Convert an instant to civil time in a zone (opaque pointer; free it).
FlintDateTime* flint_tz_convert(int64_t epoch, const char* name);
// Lookup is also boxed: the zone name lookup allocates nothing but the
// handle keeps the ABI uniform (one pointer in, one pointer out).
FlintZone* flint_tz_lookup(const char* name);
void flint_tz_free(FlintZone* z);

// Field accessors. FlintPointers are opaque, so a caller in Flint cannot
// reach into a returned FlintDateTime -- these read the field instead of
// exposing the struct, which also keeps the ABI to one pointer.
int32_t flint_dt_year(const FlintDateTime* t);
int32_t flint_dt_month(const FlintDateTime* t);
int32_t flint_dt_day(const FlintDateTime* t);
int32_t flint_dt_hour(const FlintDateTime* t);
int32_t flint_dt_minute(const FlintDateTime* t);
int32_t flint_dt_second(const FlintDateTime* t);
int32_t flint_dt_weekday(const FlintDateTime* t);
int32_t flint_dt_yearday(const FlintDateTime* t);
void        flint_tz_info(int64_t epoch, const char* name, FlintZone* out);
// Scalar Flint-facing accessors over flint_tz_info's FlintZone fields, so
// every piece of zone info is reachable from Flint without the out-param
// struct form. Both fall back like flint_tz_info: unknown zone -> 0/"UTC".
// Returned abbrev strings have static lifetime (do not free); the
// "UTC+HH[:MM]" synth path shares one buffer (last synth wins).
int64_t     flint_tz_is_dst_at(const char* name, int64_t epoch);
const char* flint_tz_abbrev_at(const char* name, int64_t epoch);
// 1 when a TZif database was found and loaded.
int64_t     flint_tzdata_loaded(void);
// 1 if the named zone is known to the built-in table or the loaded database.
int64_t     flint_tz_known(const char* name);

// ---- formatting and parsing ----------------------------------------------
// strftime-like. Supports %Y %m %d %H %M %S %z %Z %j %e %A %B %a %b %p %y %s
// and %f (microseconds). Returns a malloc'd string; release it with
// flint_time_free.
char* flint_time_format(const char* fmt, int64_t epoch, const char* zone);
void  flint_time_free(char* s);
// Parse ISO-8601: "YYYY-MM-DD", "YYYY-MM-DDTHH:MM", "YYYY-MM-DD HH:MM:SS",
// with optional fractional seconds and a trailing Z or +/-HH:MM. A trailing
// zone offset in the text WINS over `zone`; with no offset the wall reading
// is interpreted in `zone` (a fall-back ambiguous time resolves to the
// second, standard-time occurrence; a spring-forward nonexistent time folds
// forward into DST). Returns 0 on success and fills `out_epoch`; returns
// nonzero on a malformed string.
int64_t flint_time_parse(const char* text, const char* zone, int64_t* out_epoch);

// ---- duration and arithmetic ---------------------------------------------
int64_t flint_time_diff(int64_t a, int64_t b);          // a - b, seconds
char*   flint_time_duration(int64_t seconds);           // "1d02:03:04"
void    flint_time_sleep_ms(int64_t ms);
// Leap seconds are ignored (the Unix epoch has none in the modern era) and
// leap years follow the Gregorian rule exactly.
int64_t flint_time_is_leap_second(int64_t epoch);

#ifdef __cplusplus
}
#endif

#endif // FLINT_TIME_H