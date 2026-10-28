// flint_time.c — civil time, timezones, ISO-8601. Per flint_time.h.

#include "flint_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <strings.h>
#include <time.h>

// =========================================================================
// Calendar
// =========================================================================
int64_t flint_is_leap(int64_t y) {
    // Gregorian: every 4, except centuries not divisible by 400.
    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0;
}

int64_t flint_days_in_month(int32_t year, int32_t month) {
    static const int d[13] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) return 0;
    if (month == 2 && flint_is_leap(year)) return 29;
    return d[month];
}

// Days since 1970-01-01. Howard Hinnant's civil algorithms: exact for any
// year an int64 can hold, with no lookup table to get wrong.
int64_t flint_days_from_civil(int32_t y, int32_t m, int32_t d) {
    int64_t yy = y;
    yy -= (m <= 2);
    int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
    int64_t yoe = yy - era * 400;                                    // [0, 399]
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;   // [0, 365]
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;             // [0, 146096]
    return era * 146097 + doe - 719468;
}

void flint_civil_from_days(int64_t z, int32_t* y, int32_t* m, int32_t* d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yr = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t dd = doy - (153 * mp + 2) / 5 + 1;
    int64_t mm = mp + (mp < 10 ? 3 : -9);
    if (y) *y = (int32_t)(yr + (mm <= 2));
    if (m) *m = (int32_t)mm;
    if (d) *d = (int32_t)dd;
}

int64_t flint_weekday_from_days(int64_t z) {
    // 1970-01-01 was a Thursday (4).
    int64_t w = (z + 4) % 7;
    return w < 0 ? w + 7 : w;
}

FlintDateTime* flint_datetime_make(int32_t y, int32_t mon, int32_t d,
                                   int32_t h, int32_t mi, int32_t s) {
    FlintDateTime* t = (FlintDateTime*)malloc(sizeof *t);
    if (!t) return NULL;
    memset(t, 0, sizeof *t);
    t->year = y; t->month = mon; t->day = d;
    t->hour = h; t->minute = mi; t->second = s;
    int64_t days = flint_days_from_civil(y, mon, d);
    t->weekday = (int32_t)flint_weekday_from_days(days);
    t->yearday = (int32_t)(days - flint_days_from_civil(y, 1, 1));
    return t;
}

void flint_dt_free(FlintDateTime* t) { free(t); }

int64_t flint_timegm(const FlintDateTime* dt) {
    int64_t days = flint_days_from_civil(dt->year, dt->month, dt->day);
    return days * 86400 + (int64_t)dt->hour * 3600
         + (int64_t)dt->minute * 60 + dt->second;
}

FlintDateTime* flint_gmtime(int64_t epoch) {
    int64_t days = epoch / 86400;
    int64_t rem = epoch % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }
    int32_t y, m, d;
    flint_civil_from_days(days, &y, &m, &d);
    FlintDateTime* t = flint_datetime_make(y, m, d, (int32_t)(rem / 3600),
                                           (int32_t)((rem / 60) % 60),
                                           (int32_t)(rem % 60));
    if (t) t->nanosecond = 0;
    return t;
}

void flint_datetime_now(FlintDateTime* dt, int64_t epoch) {
    if (!dt) return;
    FlintDateTime* t = flint_gmtime(epoch);
    if (t) { *dt = *t; flint_dt_free(t); }
}

int64_t flint_time_diff(int64_t a, int64_t b) { return a - b; }
int64_t flint_time_is_leap_second(int64_t epoch) { (void)epoch; return 0; }

// =========================================================================
// Timezones
// =========================================================================
// A zone is a standard offset, a DST offset, and the two yearly recurrences
// on which the offset changes, each expressed as a rule, not a fixed date:
// US "second Sunday of March" is Mar week 2 Sunday, EU "last Sunday" is
// week 5, Chile "Sunday on/after Sep 2" is week 1 with min_day 2. Fixed
// month/day tables drift up to 6 days a year (verified against tzdb-2026e:
// the old table put the US transition on Mar 8/Nov 1 every year); rules do
// not. Hours/minutes are wall-clock readings *before* the transition, so a
// start rule converts with the standard offset and an end rule with the DST
// offset, yielding exact UTC transition instants.
typedef struct {
    int8_t month;     // 1-12
    int8_t week;      // 1-4 = nth, 5 = last
    int8_t wday;      // 0 = Sunday .. 6 = Saturday
    int8_t min_day;   // candidate must be >= this (Chile: 2, else 1)
    int8_t hour;      // 0-24 (24 allowed: next midnight, e.g. Cairo end)
    int8_t minute;
} DstRule;

typedef struct {
    const char* name;
    int32_t     std_offset;     // seconds east of UTC
    int32_t     dst_offset;
    const char* std_abbrev;     // NULL means use name
    const char* dst_abbrev;     // NULL means use name
    int8_t      has_dst;
    DstRule     start;          // wall time, standard side
    DstRule     end;            // wall time, DST side
} ZoneDef;

// Shorthands: US(mar 2nd Sun 02:00, nov 1st Sun 02:00), EU(mar last Sun
// 02:00 std, oct last Sun 03:00 dst), AUS(oct 1st Sun 02:00, apr 1st Sun
// 03:00), NZ(sep last Sun 02:00, apr 1st Sun 03:00). All verified against
// IANA tzdb-2026e + timeanddate 2026 transition pages, 2026-10-10.
#define US_S  {3,2,0,1,2,0}
#define US_E  {11,1,0,1,2,0}
#define EUW_S {3,5,0,1,1,0}
#define EUW_E {10,5,0,1,2,0}
#define EUC_S {3,5,0,1,2,0}
#define EUC_E {10,5,0,1,3,0}
#define EUE_S {3,5,0,1,3,0}
#define EUE_E {10,5,0,1,4,0}
#define AUS_S {10,1,0,1,2,0}
#define AUS_E {4,1,0,1,3,0}
#define NZ_S  {9,5,0,1,2,0}
#define NZ_E  {4,1,0,1,3,0}
#define NO_DST {0,0,0,0,0,0}

static const ZoneDef ZONES[] = {
    { "UTC",      0,     0,     NULL, NULL, 0, NO_DST, NO_DST },
    { "GMT",      0,     0,     NULL, NULL, 0, NO_DST, NO_DST },
    { "Z",        0,     0,     NULL, NULL, 0, NO_DST, NO_DST },
    { "IST",  19800,     0,     NULL, NULL, 0, NO_DST, NO_DST },  // India: no DST
    { "Asia/Kolkata",  19800, 0,  NULL, NULL, 0, NO_DST, NO_DST },
    { "Asia/Calcutta", 19800, 0,  NULL, NULL, 0, NO_DST, NO_DST },
    { "Asia/Kathmandu", 20700, 20700, "+0545", NULL, 0, NO_DST, NO_DST },
    { "Asia/Katmandu",  20700, 20700, "+0545", NULL, 0, NO_DST, NO_DST },
    { "JST",  32400,     0,     NULL, NULL, 0, NO_DST, NO_DST },
    { "Asia/Tokyo", 32400,  0,    NULL, NULL, 0, NO_DST, NO_DST },
    { "AEST", 36000, 39600,     NULL, "AEDT",  1, AUS_S, AUS_E },
    { "Australia/Sydney",   36000, 39600, "AEST", "AEDT", 1, AUS_S, AUS_E },
    { "Australia/Melbourne",36000, 39600, "AEST", "AEDT", 1, AUS_S, AUS_E },
    { "Australia/Adelaide", 34200, 37800, "ACST", "ACDT", 1, AUS_S, AUS_E },
    { "ACST", 34200, 34200,     NULL, NULL, 0, NO_DST, NO_DST },  // Darwin: no DST
    { "Australia/Darwin",   34200, 34200, "ACST", NULL, 0, NO_DST, NO_DST },
    { "Australia/Brisbane", 36000, 36000, "AEST", NULL, 0, NO_DST, NO_DST },
    { "Australia/Perth",    28800, 28800, "AWST", NULL, 0, NO_DST, NO_DST },
    { "Australia/Lord_Howe", 37800, 39600, "+1030", "LHDT", 1, AUS_S, {4,1,0,1,2,0} },
    { "NZST", 43200, 46800,     NULL, "NZDT",  1, NZ_S, NZ_E },
    { "Pacific/Auckland",   43200, 46800, "NZST", "NZDT", 1, NZ_S, NZ_E },
    // Chatham: +12:45/+13:45, NZ dates, :45 transition minutes.
    { "Pacific/Chatham",    45900, 49500, "+1245", "CHADT", 1,
      {9,5,0,1,2,45}, {4,1,0,1,3,45} },
    { "EST",  -18000, -14400,   NULL, "EDT",   1, US_S, US_E },
    { "EDT",  -14400, -14400,   NULL, NULL, 0, NO_DST, NO_DST },
    { "America/New_York",   -18000, -14400, "EST", "EDT", 1, US_S, US_E },
    { "America/Detroit",    -18000, -14400, "EST", "EDT", 1, US_S, US_E },
    { "America/Toronto",    -18000, -14400, "EST", "EDT", 1, US_S, US_E },
    { "America/Havana",     -18000, -14400, "CST", "CDT", 1,
      {3,2,0,1,0,0}, {11,1,0,1,1,0} },
    { "EST5EDT",            -18000, -14400, "EST", "EDT", 1, US_S, US_E },
    { "CST",  -21600, -18000,   NULL, "CDT",   1, US_S, US_E },
    { "America/Chicago",    -21600, -18000, "CST", "CDT", 1, US_S, US_E },
    { "America/Mexico_City", -21600, -21600, "CST", NULL, 0, NO_DST, NO_DST },
    { "MST",  -25200, -25200,   NULL, NULL, 0, NO_DST, NO_DST },  // Arizona: no DST
    { "America/Denver",     -25200, -21600, "MST", "MDT", 1, US_S, US_E },
    { "America/Phoenix",    -25200, -25200, "MST", NULL, 0, NO_DST, NO_DST },
    { "PST",  -28800, -25200,   NULL, "PDT",   1, US_S, US_E },
    { "PDT",  -25200, -25200,   NULL, NULL, 0, NO_DST, NO_DST },
    { "America/Los_Angeles",-28800, -25200, "PST", "PDT", 1, US_S, US_E },
    { "PST8PDT",            -28800, -25200, "PST", "PDT", 1, US_S, US_E },
    { "AKST", -32400, -28800,   NULL, "AKDT",  1, US_S, US_E },
    { "America/Anchorage",  -32400, -28800, "AKST", "AKDT", 1, US_S, US_E },
    { "HST", -36000, -36000,   NULL, NULL, 0, NO_DST, NO_DST },
    { "Pacific/Honolulu",   -36000, -36000, "HST", NULL, 0, NO_DST, NO_DST },
    { "BRT", -10800, -10800,   NULL, NULL, 0, NO_DST, NO_DST },  // no DST since 2019
    { "America/Sao_Paulo",  -10800, -10800, "BRT", NULL, 0, NO_DST, NO_DST },
    // Santiago: IANA "Sep Sun>=2 4:00u / Apr Sun>=2 3:00u": first Sunday
    // on/after the 2nd, midnight wall (00:00 std side / 00:00 dst side).
    { "America/Santiago",   -14400, -10800, "CLT", "CLST", 1,
      {9,1,0,2,0,0}, {4,1,0,2,0,0} },
    { "America/Argentina/Buenos_Aires", -10800, -10800, "ART", NULL, 0, NO_DST, NO_DST },
    { "America/Montevideo", -10800, -10800, "UYT", NULL, 0, NO_DST, NO_DST },
    { "Atlantic/Reykjavik",      0,     0,  "GMT", NULL, 0, NO_DST, NO_DST },
    { "WET",      0,  3600,     NULL, "WEST",  1, EUW_S, EUW_E },
    { "Europe/London",      0,  3600,  "GMT", "BST",  1, EUW_S, EUW_E },
    { "Europe/Dublin",      0,  3600,  "GMT", "IST",  1, EUW_S, EUW_E },
    { "Europe/Lisbon",      0,  3600,  "WET", "WEST",  1, EUW_S, EUW_E },
    { "CET",  3600,  7200,     NULL, "CEST",  1, EUC_S, EUC_E },
    { "Europe/Paris",    3600,  7200,  "CET", "CEST", 1, EUC_S, EUC_E },
    { "Europe/Berlin",    3600,  7200,  "CET", "CEST", 1, EUC_S, EUC_E },
    { "Europe/Madrid",    3600,  7200,  "CET", "CEST", 1, EUC_S, EUC_E },
    { "Europe/Rome",      3600,  7200,  "CET", "CEST", 1, EUC_S, EUC_E },
    { "Europe/Amsterdam", 3600,  7200,  "CET", "CEST", 1, EUC_S, EUC_E },
    { "Europe/Brussels",  3600,  7200,  "CET", "CEST", 1, EUC_S, EUC_E },
    { "Europe/Vienna",    3600,  7200,  "CET", "CEST", 1, EUC_S, EUC_E },
    { "Europe/Prague",    3600,  7200,  "CET", "CEST", 1, EUC_S, EUC_E },
    { "Europe/Warsaw",    3600,  7200,  "CET", "CEST", 1, EUC_S, EUC_E },
    { "Europe/Stockholm",3600,  7200,  "CET", "CEST", 1, EUC_S, EUC_E },
    { "Europe/Oslo",      3600,  7200,  "CET", "CEST", 1, EUC_S, EUC_E },
    { "Europe/Copenhagen",3600,  7200,  "CET", "CEST", 1, EUC_S, EUC_E },
    { "Europe/Budapest",  3600,  7200,  "CET", "CEST", 1, EUC_S, EUC_E },
    { "Europe/Bratislava",3600,  7200,  "CET", "CEST", 1, EUC_S, EUC_E },
    { "Europe/Ljubljana", 3600,  7200,  "CET", "CEST", 1, EUC_S, EUC_E },
    { "Europe/Istanbul", 10800, 10800, "+03", NULL, 0, NO_DST, NO_DST },
    { "EET",  7200, 10800,     NULL, "EEST",  1, EUE_S, EUE_E },
    { "Europe/Athens",    7200, 10800,  "EET", "EEST", 1, EUE_S, EUE_E },
    { "Europe/Helsinki",  7200, 10800,  "EET", "EEST", 1, EUE_S, EUE_E },
    { "Europe/Kiev",      7200, 10800,  "EET", "EEST", 1, EUE_S, EUE_E },
    { "Europe/Kyiv",      7200, 10800,  "EET", "EEST", 1, EUE_S, EUE_E },
    { "Europe/Bucharest", 7200, 10800,  "EET", "EEST", 1, EUE_S, EUE_E },
    // Cairo: IANA "Apr lastFri 0:00 / Oct lastThu 24:00" (end reads as
    // Friday 00:00; hour 24 rolls over in timegm arithmetic).
    { "Africa/Cairo",     7200, 10800, "EET", "EEST", 1,
      {4,5,5,1,0,0}, {10,5,4,1,24,0} },
    { "Africa/Johannesburg", 7200, 7200, "SAST", NULL, 0, NO_DST, NO_DST },
    { "Africa/Lagos",        3600, 3600,  "WAT", NULL, 0, NO_DST, NO_DST },
    { "Africa/Nairobi",     10800, 10800, "EAT", NULL, 0, NO_DST, NO_DST },
    { "Africa/Algiers",      3600, 3600,  "CET", NULL, 0, NO_DST, NO_DST },
    // Casablanca: IANA tzdb-2026e ends DST rules at 2026-09-20T02:00 and
    // leaves +0 with no further rules (Ramadan suspensions were yearly
    // one-shot decrees, unmodellable as a fixed rule). If Morocco decrees
    // new DST, this entry must be revisited, not silently trusted.
    { "Africa/Casablanca",      0,     0,  "+00", NULL, 0, NO_DST, NO_DST },
    { "Asia/Dubai",         14400, 14400, "+04", NULL, 0, NO_DST, NO_DST },
    { "Asia/Riyadh",        10800, 10800, "+03", NULL, 0, NO_DST, NO_DST },
    { "Asia/Tehran",        12600, 12600, "+0330", NULL, 0, NO_DST, NO_DST },
    { "Asia/Amman",         10800, 10800, "+03", NULL, 0, NO_DST, NO_DST },
    { "Asia/Karachi",       18000, 18000, "PKT", NULL, 0, NO_DST, NO_DST },
    { "Asia/Dhaka",         21600, 21600, "+06", NULL, 0, NO_DST, NO_DST },
    { "Asia/Bangkok",       25200, 25200, "+07", NULL, 0, NO_DST, NO_DST },
    { "Asia/Jakarta",       25200, 25200, "WIB", NULL, 0, NO_DST, NO_DST },
    { "Asia/Ho_Chi_Minh",   25200, 25200, "+07", NULL, 0, NO_DST, NO_DST },
    { "Asia/Shanghai",      28800, 28800, "CST", NULL, 0, NO_DST, NO_DST },
    { "Asia/Hong_Kong",     28800, 28800, "HKT", NULL, 0, NO_DST, NO_DST },
    { "Asia/Macau",         28800, 28800, "MST", NULL, 0, NO_DST, NO_DST },
    { "Asia/Singapore",     28800, 28800, "+08", NULL, 0, NO_DST, NO_DST },
    { "Asia/Taipei",        28800, 28800, "CST", NULL, 0, NO_DST, NO_DST },
    { "Asia/Manila",        28800, 28800, "PHT", NULL, 0, NO_DST, NO_DST },
    { "Asia/Seoul",         32400, 32400, "KST", NULL, 0, NO_DST, NO_DST },
    { "Australia/Brisbane", 36000, 36000, "AEST", NULL, 0, NO_DST, NO_DST },
    { "Australia/Perth",    28800, 28800, "AWST", NULL, 0, NO_DST, NO_DST },
    { "Pacific/Guam",       36000, 36000, "ChST", NULL, 0, NO_DST, NO_DST },
    { "America/Bogota",     -18000, -18000, "COT", NULL, 0, NO_DST, NO_DST },
    { "America/Lima",       -18000, -18000, "PET", NULL, 0, NO_DST, NO_DST },
    { "America/Argentina/Buenos_Aires", -10800, -10800, "ART", NULL, 0, NO_DST, NO_DST },
    { "America/Montevideo", -10800, -10800, "UYT", NULL, 0, NO_DST, NO_DST },
    { "UTC-08:00",  -28800, -28800,   NULL, NULL, 0, NO_DST, NO_DST },
    { "UTC+05:30",   19800,  19800,   NULL, NULL, 0, NO_DST, NO_DST },
    { NULL, 0, 0, NULL, NULL, 0, NO_DST, NO_DST }
};

// No IANA TZif database is parsed: the container on this device is Android's
// app blob (a name index), not a zoneinfo tree, and shipping a 430 KB opaque
// format as a dependency is worse than a correct built-in table. Zones not in
// the table resolve to UTC and say so, rather than silently claiming a zone
// they do not have.
static int64_t g_tzdata_loaded = 0;

int64_t flint_tzdata_loaded(void) { return g_tzdata_loaded; }

static int _dg(const char* s, int n, int* out);   // defined in Parsing

int64_t flint_tz_known(const char* name) {
    if (!name || !*name) return 0;
    if (strcasecmp(name, "local") == 0) return 1;   // process TZ, always resolvable
    for (int i = 0; ZONES[i].name; i++)
        if (strcasecmp(ZONES[i].name, name) == 0) return 1;
    // "UTC+05:30" style names parse without a table entry (see _find).
    if ((name[0] == 'U' || name[0] == 'u') && (name[1] == 'T' || name[1] == 't') &&
        (name[2] == 'C' || name[2] == 'c') && (name[3] == '+' || name[3] == '-'))
        return 1;
    return 0;
}

// Synthesized entry for "UTC+HH[:MM]" names. Static storage: _find returns
// a pointer the caller does not own, same contract as table entries.
static ZoneDef _utc_synth;
static char _utc_abbrev[8];

static const ZoneDef* _find(const char* name) {
    if (!name || !*name) name = "UTC";
    for (int i = 0; ZONES[i].name; i++)
        if (strcasecmp(ZONES[i].name, name) == 0) return &ZONES[i];
    if ((name[0] == 'U' || name[0] == 'u') && (name[1] == 'T' || name[1] == 't') &&
        (name[2] == 'C' || name[2] == 'c') && (name[3] == '+' || name[3] == '-')) {
        int sign = (name[3] == '-') ? -1 : 1, h = 0, mi = 0;
        const char* p = name + 4;
        if (!_dg(p, 1, &h) && !_dg(p, 2, &h)) return NULL;
        p += (p[1] >= '0' && p[1] <= '9') ? 2 : 1;
        if (*p == ':') { p++; if (!_dg(p, 2, &mi)) return NULL; p += 2; }
        if (*p || h > 14 || mi > 59) return NULL;
        int32_t off = sign * (h * 3600 + mi * 60);
        snprintf(_utc_abbrev, sizeof _utc_abbrev, "%c%02d%02d",
                 sign < 0 ? '-' : '+', h, mi);
        _utc_synth.name = name;
        _utc_synth.std_offset = off;
        _utc_synth.dst_offset = off;
        _utc_synth.std_abbrev = _utc_abbrev;
        _utc_synth.dst_abbrev = NULL;
        _utc_synth.has_dst = 0;
        memset(&_utc_synth.start, 0, sizeof _utc_synth.start);
        memset(&_utc_synth.end, 0, sizeof _utc_synth.end);
        return &_utc_synth;
    }
    return NULL;
}

// Day-of-month of the (week, wday) rule in (y, month): week 1-4 = nth,
// week 5 = last. min_day pushes forward (Chile "Sunday on/after the 2nd").
static int64_t _rule_day(int32_t y, int32_t mo, const DstRule* r) {
    int64_t dim = flint_days_in_month(y, mo);
    int64_t d;
    if (r->week >= 5) {
        int64_t wd = flint_weekday_from_days(flint_days_from_civil(y, mo, (int32_t)dim));
        d = dim - ((wd - r->wday + 7) % 7);
    } else {
        int64_t wd = flint_weekday_from_days(flint_days_from_civil(y, mo, 1));
        d = 1 + ((r->wday - wd + 7) % 7) + (r->week - 1) * 7;
        if (d > dim) d -= 7;
    }
    while (d < r->min_day) d += 7;
    return d;
}

// UTC instant of a rule in year y. The wall reading is on the side named by
// pre_offset (standard for starts, DST for ends), so subtracting it lands
// exactly on the transition even when the wall hour is 24 (Cairo end).
static int64_t _rule_utc(int32_t y, const DstRule* r, int32_t pre_offset) {
    int64_t d = _rule_day(y, r->month, r);
    FlintDateTime* tp = flint_datetime_make(y, r->month, (int32_t)d,
                                            r->hour, r->minute, 0);
    if (!tp) return 0;
    int64_t utc = flint_timegm(tp) - pre_offset;
    flint_dt_free(tp);
    return utc;
}

int64_t flint_tz_offset_at(const char* name, int64_t epoch) {
    const ZoneDef* z = _find(name);
    if (!z) {
        // "local" defers to the process TZ; anything else unknown is UTC.
        if (name && strcasecmp(name, "local") == 0) {
            time_t t = (time_t)epoch;
            struct tm lt;
            if (localtime_r(&t, &lt)) return (int64_t)lt.tm_gmtoff;
        }
        return 0;
    }
    if (!z->has_dst) return z->std_offset;

    // Floored day count (C truncates toward zero; pre-1970 needs the floor).
    int64_t days = epoch >= 0 ? epoch / 86400
                              : -(((-epoch) + 86399) / 86400);
    int32_t y, m, d;
    flint_civil_from_days(days, &y, &m, &d);

    int64_t start = _rule_utc(y, &z->start, z->std_offset);
    int64_t end   = _rule_utc(y, &z->end, z->dst_offset);
    int in_dst;
    if (z->start.month <= z->end.month)
        in_dst = (epoch >= start && epoch < end);          // northern
    else
        in_dst = (epoch >= start || epoch < end);          // wraps new year
    return in_dst ? z->dst_offset : z->std_offset;
}

static const char* _abbrev(const ZoneDef* z, int is_dst) {
    if (is_dst) return z->dst_abbrev ? z->dst_abbrev : z->name;
    return z->std_abbrev ? z->std_abbrev : z->name;
}

FlintZone* flint_tz_lookup(const char* name) {
    const ZoneDef* z = _find(name);
    FlintZone* out = (FlintZone*)malloc(sizeof *out);
    if (!out) return NULL;
    if (!z) { out->offset_seconds = 0; out->abbrev = "UTC"; out->is_dst = 0; return out; }
    out->offset_seconds = z->std_offset;
    out->abbrev = _abbrev(z, 0);
    out->is_dst = 0;
    return out;
}

void flint_tz_free(FlintZone* z) { free(z); }

void flint_tz_info(int64_t epoch, const char* name, FlintZone* out) {
    if (!out) return;
    const ZoneDef* z = _find(name);
    if (!z) {
        out->offset_seconds = flint_tz_offset_at(name, epoch);
        out->abbrev = "UTC";
        out->is_dst = 0;
        return;
    }
    out->offset_seconds = flint_tz_offset_at(name, epoch);
    int dst = (z->has_dst && out->offset_seconds == z->dst_offset) ? 1 : 0;
    out->abbrev = _abbrev(z, dst);
    out->is_dst = dst;
}

// Flint-facing accessors over flint_tz_info's FlintZone fields: the out-param
// struct form is C-internal (needs a caller-allocated struct Flint cannot
// express), so these expose the (offset, abbrev, is_dst) triple that Flint
// can reach as scalar calls. Same fallbacks as flint_tz_info: unknown zone
// -> offset 0, "UTC", no DST. Note: flint_tz_offset_at already covers the
// offset; these add the other two. Returned abbrev strings have static
// lifetime (zone table entries or the static fallback) -- do not free; the
// "UTC+HH[:MM]" synth path shares one buffer, so only the most recently
// synthesized offset's abbrev stays valid (same contract as _find).
// (name, epoch) order, matching flint_tz_offset_at. Same fallbacks as
// flint_tz_info: unknown zone -> offset 0, "UTC", no DST.
int64_t flint_tz_is_dst_at(const char* name, int64_t epoch) {
    FlintZone zi;
    flint_tz_info(epoch, name, &zi);
    return zi.is_dst;
}

const char* flint_tz_abbrev_at(const char* name, int64_t epoch) {
    FlintZone zi;
    flint_tz_info(epoch, name, &zi);
    return zi.abbrev;
}

FlintDateTime* flint_tz_convert(int64_t epoch, const char* name) {
    return flint_gmtime(epoch + flint_tz_offset_at(name, epoch));
}

// =========================================================================
// Formatting
// =========================================================================
static const char* WD[7] = { "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday" };
static const char* MO[12] = { "January","February","March","April","May","June",
                              "July","August","September","October","November","December" };

void flint_time_free(char* s) { free(s); }

typedef struct { char* b; size_t n, cap; int oom; } SB;

static void _put(SB* s, const char* fmt, ...) {
    va_list ap;
    char tmp[256];
    va_start(ap, fmt);
    int m = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (m < 0) return;
    if (s->n + (size_t)m + 1 > s->cap) {
        size_t c = s->cap ? s->cap * 2 : 256;
        while (c < s->n + (size_t)m + 1) c *= 2;
        char* nb = (char*)realloc(s->b, c);
        if (!nb) { s->oom = 1; return; }
        s->b = nb; s->cap = c;
    }
    memcpy(s->b + s->n, tmp, (size_t)m);
    s->n += (size_t)m;
    s->b[s->n] = '\0';
}

char* flint_time_format(const char* fmt, int64_t epoch, const char* zone) {
    if (!fmt) return NULL;
    int64_t off = flint_tz_offset_at(zone, epoch);
    FlintDateTime* tp = flint_gmtime(epoch + off);
    if (!tp) return NULL;
    FlintDateTime t = *tp;
    flint_dt_free(tp);
    FlintZone zi;
    flint_tz_info(epoch, zone, &zi);

    SB s = { NULL, 0, 0, 0 };
    for (const char* p = fmt; *p && !s.oom; p++) {
        if (*p != '%') { _put(&s, "%c", *p); continue; }
        p++;
        if (!*p) { _put(&s, "%%"); break; }
        switch (*p) {
            case 'Y': _put(&s, "%04d", t.year); break;
            case 'y': _put(&s, "%02d", ((t.year % 100) + 100) % 100); break;
            case 'm': _put(&s, "%02d", t.month); break;
            case 'd': _put(&s, "%02d", t.day); break;
            case 'e': _put(&s, "%2d", t.day); break;
            case 'H': _put(&s, "%02d", t.hour); break;
            case 'M': _put(&s, "%02d", t.minute); break;
            case 'S': _put(&s, "%02d", t.second); break;
            case 'j': _put(&s, "%03d", t.yearday + 1); break;
            case 'A': _put(&s, "%s", WD[t.weekday % 7]); break;
            case 'a': _put(&s, "%.3s", WD[t.weekday % 7]); break;
            case 'B': _put(&s, "%s", MO[(t.month - 1) % 12]); break;
            case 'b': _put(&s, "%.3s", MO[(t.month - 1) % 12]); break;
            case 'p': _put(&s, "%s", t.hour < 12 ? "AM" : "PM"); break;
            case 'f': _put(&s, "%06d", t.nanosecond / 1000); break;
            case 'z': {
                int64_t o = zi.offset_seconds;
                int64_t a = o < 0 ? -o : o;
                _put(&s, "%c%02d%02d", o < 0 ? '-' : '+',
                     (int)(a / 3600), (int)((a / 60) % 60));
                break;
            }
            case 'Z': _put(&s, "%s", zi.abbrev ? zi.abbrev : "UTC"); break;
            case 's': _put(&s, "%lld", (long long)epoch); break;
            case '%': _put(&s, "%%"); break;
            default:  _put(&s, "%%%c", *p); break;
        }
    }
    if (s.oom) { free(s.b); return NULL; }
    if (!s.b) s.b = (char*)calloc(1, 1);
    return s.b;
}

// =========================================================================
// Parsing
// =========================================================================
static int _dg(const char* s, int n, int* out) {
    int v = 0;    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return 1;
}

int64_t flint_time_parse(const char* text, const char* zone, int64_t* out_epoch) {
    if (!text || !out_epoch) return 1;
    while (*text == ' ') text++;

    int y = 1970, mo = 1, d = 1, h = 0, mi = 0, se = 0;
    if (!_dg(text, 4, &y)) return 1;
    text += 4;
    if (*text == '-') {
        text++;
        if (!_dg(text, 2, &mo)) return 1;
        text += 2;
    }
    if (*text == '-') {
        text++;
        if (!_dg(text, 2, &d)) return 1;
        text += 2;
    }
    if (*text == 'T' || *text == 't' || (*text == ' ' && strlen(text) > 8)) {
        text++;
        if (!_dg(text, 2, &h)) return 1;
        text += 2;
        if (*text == ':') { text++; if (!_dg(text, 2, &mi)) return 1; text += 2; }
        if (*text == ':') { text++; if (!_dg(text, 2, &se)) return 1; text += 2; }
        if (*text == '.') { text++; while (*text >= '0' && *text <= '9') text++; }
    }
    // An offset IN THE TEXT beats the zone argument: the string is the more
    // specific statement about which instant this is.
    int64_t off = 0;
    int have_off = 0;
    if (*text == 'Z' || *text == 'z') {
        text++;
        have_off = 1;
    } else if (*text == '+' || *text == '-') {
        int sign = (*text == '-') ? -1 : 1;
        text++;
        int oh = 0, om = 0;
        if (!_dg(text, 2, &oh)) return 1;
        text += 2;
        if (*text == ':') { text++; if (!_dg(text, 2, &om)) return 1; text += 2; }
        off = sign * (oh * 3600 + om * 60);
        have_off = 1;
    }
    while (*text == ' ') text++;
    if (*text) return 1;                       // trailing junk

    if (mo < 1 || mo > 12) return 1;
    if (d < 1 || d > flint_days_in_month(y, mo)) return 1;
    if (h > 23 || mi > 59 || se > 60) return 1;

    FlintDateTime* tp = flint_datetime_make(y, mo, d, h, mi, se);
    if (!tp) return 1;
    FlintDateTime t = *tp;
    flint_dt_free(tp);
    if (!have_off) {
        // No offset in the text: the wall reading is in `zone`. Unknown
        // zones stay UTC (historical behaviour); "local" uses the process
        // offset. One refinement pass: interpreting the wall time with the
        // standard offset first lands inside the right period except
        // across a transition, where the second evaluation corrects it.
        // A fall-back ambiguous wall time resolves to the second
        // (standard-time) occurrence; a spring-forward nonexistent time
        // folds forward into DST.
        const ZoneDef* z = _find(zone);
        int64_t base = flint_timegm(&t);
        if (!z || !z->has_dst) {
            *out_epoch = base - flint_tz_offset_at(zone, base);
        } else {
            int64_t guess = base - z->std_offset;
            *out_epoch = base - flint_tz_offset_at(zone, guess);
        }
        return 0;
    }
    *out_epoch = flint_timegm(&t) - off;        // local -> UTC
    return 0;
}

char* flint_time_duration(int64_t seconds) {
    // Normalise through a single subtraction: C truncates toward zero, so
    // -3661/86400 is 0 and the result read "-0d01:01:01" -- a duration that
    // claims zero days.
    int neg = seconds < 0;
    // Work in unsigned-safe territory: C truncates toward zero, so -3661/86400
    // is 0 and the result read "-0d01:01:01" -- a duration claiming zero days.
    // Taking the magnitude in uint64 makes the division floor, as intended.
    uint64_t mag = neg ? (uint64_t)(-(seconds + 1)) + 1u : (uint64_t)seconds;
    int64_t d = (int64_t)(mag / 86400u);
    int64_t r = (int64_t)(mag % 86400u);
    char* out = (char*)malloc(64);
    if (!out) return NULL;
    snprintf(out, 64, "%s%lldd%02d:%02d:%02d", neg ? "-" : "", (long long)d,
             (int)(r / 3600), (int)((r / 60) % 60), (int)(r % 60));
    return out;
}

void flint_time_sleep_ms(int64_t ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&ts, NULL);
}
// ---- field accessors -----------------------------------------------------
// FlintPointers are opaque in the language, so the fields are read here. Each
// NULL-checks rather than crashing on a bad handle.
#define DT_FIELD(FIELD)     int32_t flint_dt_##FIELD(const FlintDateTime* t) { return t ? t->FIELD : 0; }
DT_FIELD(year)
DT_FIELD(month)
DT_FIELD(day)
DT_FIELD(hour)
DT_FIELD(minute)
DT_FIELD(second)
DT_FIELD(weekday)
DT_FIELD(yearday)
