// time_check.c — timezone/civil-time verification against the libc oracle.
//
// Method: for each DST zone, a POSIX TZ string expressing the IANA rule is
// installed (setenv/TZ/tzset) and bionic's localtime tm_gmtoff is compared
// against flint_tz_offset_at over weekly sweeps (2025-2027) plus exact
// transition edges. POSIX M-rules are an independent implementation of the
// same IANA rules (tzdb-2026e, verified 2026-10-10), so agreement is real
// evidence, not self-consistency. Known transition instants (UTC) below are
// from timeanddate.com 2026 pages + worldtimeandweather.com/dst/2026.
//
// Build: cc -O1 tests/time_check.c -Iruntime flint_time.o -lm -o time_check
// Exit nonzero on any FAIL. Gate greps ^FAIL.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "flint_time.h"

static int fails = 0;
static int checks = 0;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { \
    fails++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// bionic oracle: offset in effect at epoch under a POSIX TZ rule.
static long oracle_offset(const char* posix_tz, int64_t epoch) {
    setenv("TZ", posix_tz, 1);
    tzset();
    time_t t = (time_t)epoch;
    struct tm lt;
    if (!localtime_r(&t, &lt)) return 0x7fffffffL;
    return (long)lt.tm_gmtoff;
}

typedef struct { const char* flint; const char* posix; } ZoneCase;

// POSIX M-rules mirror the IANA rules verified 2026-10-10 (tzdb-2026e).
// M<mon>.<week>.<wday>/<hh>[:mm]: week 1-4 nth, 5 = last; wday 0 = Sun.
static const ZoneCase DST_ZONES[] = {
    { "America/New_York", "EST5EDT,M3.2.0/2,M11.1.0/2" },
    { "America/Chicago",  "CST6CDT,M3.2.0/2,M11.1.0/2" },
    { "America/Denver",   "MST7MDT,M3.2.0/2,M11.1.0/2" },
    { "America/Los_Angeles", "PST8PDT,M3.2.0/2,M11.1.0/2" },
    { "America/Anchorage", "AKST9AKDT,M3.2.0/2,M11.1.0/2" },
    { "EST", "EST5EDT,M3.2.0/2,M11.1.0/2" },
    { "CST", "CST6CDT,M3.2.0/2,M11.1.0/2" },
    { "PST", "PST8PDT,M3.2.0/2,M11.1.0/2" },
    { "America/Havana",  "CST5CDT,M3.2.0/0,M11.1.0/1" },
    { "Europe/London",   "GMT0BST,M3.5.0/1,M10.5.0/2" },
    { "Europe/Paris",    "CET-1CEST,M3.5.0/2,M10.5.0/3" },
    { "Europe/Athens",   "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "CET", "CET-1CEST,M3.5.0/2,M10.5.0/3" },
    { "EET", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "WET", "WET0WEST,M3.5.0/1,M10.5.0/2" },
    { "Australia/Sydney", "AEST-10AEDT,M10.1.0/2,M4.1.0/3" },
    { "Australia/Adelaide", "ACST-9:30ACDT,M10.1.0/2,M4.1.0/3" },
    { "AEST", "AEST-10AEDT,M10.1.0/2,M4.1.0/3" },
    { "Pacific/Auckland", "NZST-12NZDT,M9.5.0/2,M4.1.0/3" },
    { "NZST", "NZST-12NZDT,M9.5.0/2,M4.1.0/3" },
    { "Pacific/Chatham", "CHAST-12:45CHADT,M9.5.0/2:45,M4.1.0/3:45" },
    { "Australia/Lord_Howe", "LHST-10:30LHDT-11,M10.1.0/2,M4.1.0/2" },
    { "America/Santiago", "CLT4CLST,M9.1.0/0,M4.1.0/0" },
    { "Africa/Cairo",    "EET-2EEST,M4.5.5/0,M10.5.4/24" },
    { NULL, NULL }
};

// Exact 2026 transition instants (UTC), start then end per zone.
typedef struct { const char* flint; int64_t start; int64_t end; int std_off; int dst_off; } Edge;
static int64_t U(int y, int mo, int d, int h, int mi) {
    FlintDateTime* t = flint_datetime_make(y, mo, d, h, mi, 0);
    int64_t e = t ? flint_timegm(t) : -1;
    flint_dt_free(t);
    return e;
}
static const Edge* edges(void) {
    static Edge e[16]; static int init = 0;
    if (!init) {
        init = 1;
        // US spring forward Mar 8 07:00Z, fall back Nov 1 06:00Z.
        e[0]  = (Edge){ "America/New_York", U(2026,3,8,7,0), U(2026,11,1,6,0), -18000, -14400 };
        // EU: Mar 29 01:00Z -> +2; Oct 25 01:00Z -> +1.
        e[1]  = (Edge){ "Europe/Paris", U(2026,3,29,1,0), U(2026,10,25,1,0), 3600, 7200 };
        e[2]  = (Edge){ "Europe/London", U(2026,3,29,1,0), U(2026,10,25,1,0), 0, 3600 };
        // Sydney: Oct 4 02:00+10 = Oct 3 16:00Z; Apr 5 03:00+11 = Apr 4 16:00Z.
        e[3]  = (Edge){ "Australia/Sydney", U(2026,10,3,16,0), U(2026,4,4,16,0), 36000, 39600 };
        // Adelaide: Oct 4 02:00+9:30 = Oct 3 16:30Z; Apr 5 03:00+10:30 = Apr 4 16:30Z.
        e[4]  = (Edge){ "Australia/Adelaide", U(2026,10,3,16,30), U(2026,4,4,16,30), 34200, 37800 };
        // NZ: Sep 27 02:00+12 = Sep 26 14:00Z; Apr 5 03:00+13 = Apr 4 14:00Z.
        e[5]  = (Edge){ "Pacific/Auckland", U(2026,9,26,14,0), U(2026,4,4,14,0), 43200, 46800 };
        // Chatham: Sep 27 02:45+12:45 = Sep 26 14:00Z; Apr 5 03:45+13:45 = Apr 4 14:00Z.
        e[6]  = (Edge){ "Pacific/Chatham", U(2026,9,26,14,0), U(2026,4,4,14,0), 45900, 49500 };
        // Lord Howe: Oct 4 02:00+10:30 = Oct 3 15:30Z; Apr 5 02:00+11 = Apr 4 15:00Z.
        e[7]  = (Edge){ "Australia/Lord_Howe", U(2026,10,3,15,30), U(2026,4,4,15,0), 37800, 39600 };
        // Cairo: Apr 24 00:00+2 = Apr 23 22:00Z; Oct 30 00:00+3 = Oct 29 21:00Z.
        e[8]  = (Edge){ "Africa/Cairo", U(2026,4,23,22,0), U(2026,10,29,21,0), 7200, 10800 };
        // Havana: Mar 8 00:00-5 = Mar 8 05:00Z; Nov 1 01:00-4 = Nov 1 05:00Z.
        e[9]  = (Edge){ "America/Havana", U(2026,3,8,5,0), U(2026,11,1,5,0), -18000, -14400 };
        // Santiago: Sep 6 00:00-4 = Sep 6 04:00Z; Apr 5 00:00-3 = Apr 5 03:00Z.
        e[10] = (Edge){ "America/Santiago", U(2026,9,6,4,0), U(2026,4,5,3,0), -14400, -10800 };
        e[11] = (Edge){ NULL, 0, 0, 0, 0 };
    }
    return e;
}

int main(void) {
    // ---- A. weekly sweep vs oracle, 2025-2027 ----
    int64_t t0 = U(2025, 1, 1, 0, 0);
    for (int zi = 0; DST_ZONES[zi].flint; zi++) {
        int mism = 0;
        for (int w = 0; w < 157; w++) {
            int64_t ep = t0 + (int64_t)w * 7 * 86400 + 43200;  // Wednesdays
            long want = oracle_offset(DST_ZONES[zi].posix, ep);
            int64_t got = flint_tz_offset_at(DST_ZONES[zi].flint, ep);
            if (want != got) {
                if (mism < 4)
                    printf("FAIL sweep %s epoch=%lld want=%ld got=%lld\n",
                           DST_ZONES[zi].flint, (long long)ep, want, (long long)got);
                mism++;
            }
            checks++;
        }
        if (mism) { fails++; printf("FAIL sweep %s: %d/157 weeks mismatch\n", DST_ZONES[zi].flint, mism); }
    }

    // ---- B. exact transition edges ----
    // Probes at instant+delta must see the pre-edge offset for delta<0
    // and the post-edge offset for delta>=0 (at the exact instant the
    // new offset is already in effect).
    const Edge* e = edges();
    static const int64_t deltas[] = { -72*3600, -8*3600, -3600, -1, 0, 1, 3600, 8*3600, 72*3600 };
    for (int i = 0; e[i].flint; i++) {
        for (int k = 0; k < 9; k++) {
            int64_t ep = e[i].start + deltas[k];
            int want = (deltas[k] < 0) ? e[i].std_off : e[i].dst_off;
            CHECK(flint_tz_offset_at(e[i].flint, ep) == want,
                  "edge %s start%+lld want=%d got=%lld", e[i].flint,
                  (long long)deltas[k], want,
                  (long long)flint_tz_offset_at(e[i].flint, ep));
            ep = e[i].end + deltas[k];
            want = (deltas[k] < 0) ? e[i].dst_off : e[i].std_off;
            CHECK(flint_tz_offset_at(e[i].flint, ep) == want,
                  "edge %s end%+lld want=%d got=%lld", e[i].flint,
                  (long long)deltas[k], want,
                  (long long)flint_tz_offset_at(e[i].flint, ep));
        }
    }

    // ---- C. round-trip convert->fields->timegm ----
    const char* rt_zones[] = { "UTC", "IST", "Asia/Tokyo", "America/New_York",
        "Australia/Sydney", "Pacific/Chatham", "Australia/Lord_Howe",
        "Africa/Cairo", "America/Santiago", "America/Havana", "Asia/Kathmandu",
        "Europe/Paris", "Pacific/Auckland", "Nowhere/X", NULL };
    int64_t samples[] = { 0, 1, -1, -86400, -2208988800LL, 4102444800LL,
        U(2026,3,8,6,59), U(2026,3,8,7,0), U(2038,1,19,3,14), -62135596800LL };
    for (int zi = 0; rt_zones[zi]; zi++)
        for (unsigned si = 0; si < sizeof(samples)/sizeof(samples[0]); si++) {
            FlintDateTime* t = flint_tz_convert(samples[si], rt_zones[zi]);
            CHECK(t != NULL, "convert null %s ep=%lld", rt_zones[zi], (long long)samples[si]);
            int64_t back = t ? flint_timegm(t) - flint_tz_offset_at(rt_zones[zi], samples[si]) : -1;
            flint_dt_free(t);
            CHECK(back == samples[si], "roundtrip %s ep=%lld got=%lld",
                  rt_zones[zi], (long long)samples[si], (long long)back);
        }

    // ---- D. calendar vs libc (TZ=UTC) ----
    setenv("TZ", "UTC0", 1); tzset();
    for (int64_t d = -73000; d < 73000; d += 97) {
        int32_t y, m, dd;
        flint_civil_from_days(d, &y, &m, &dd);
        time_t tt = (time_t)(d * 86400 + 43200);
        struct tm tmv;
        gmtime_r(&tt, &tmv);
        CHECK(y == tmv.tm_year + 1900 && m == tmv.tm_mon + 1 && dd == tmv.tm_mday,
              "civil day=%lld flint=%d-%d-%d libc=%d-%d-%d", (long long)d,
              y, m, dd, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        CHECK(flint_weekday_from_days(d) == tmv.tm_wday,
              "weekday day=%lld flint=%lld libc=%d", (long long)d,
              (long long)flint_weekday_from_days(d), tmv.tm_wday);
        CHECK(flint_days_from_civil(y, m, dd) == d,
              "days_from_civil %d-%d-%d", y, m, dd);
    }

    // ---- E. parse/format/duration ----
    {
        int64_t out = -1;
        // zone argument applies when the text carries no offset (new behaviour)
        CHECK(flint_time_parse("2026-07-15T12:00", "America/New_York", &out) == 0
              && out == U(2026,7,15,16,0), "parse with zone, got=%lld", (long long)out);
        out = -1;
        CHECK(flint_time_parse("2026-01-15T12:00", "Australia/Sydney", &out) == 0
              && out == U(2026,1,15,1,0), "parse sydney summer, got=%lld", (long long)out);
        // explicit offset still wins
        out = -1;
        CHECK(flint_time_parse("2026-07-15T12:00+02:00", "America/New_York", &out) == 0
              && out == U(2026,7,15,10,0), "parse offset wins, got=%lld", (long long)out);
        // malformed stays rejected
        CHECK(flint_time_parse("garbage", "UTC", &out) != 0, "parse garbage accepted");
        CHECK(flint_time_parse("2026-02-30", "UTC", &out) != 0, "parse feb30 accepted");
        CHECK(flint_time_parse("2026-13-01", "UTC", &out) != 0, "parse mo13 accepted");
        CHECK(flint_time_parse("2026-01-01T25:00", "UTC", &out) != 0, "parse h25 accepted");
        // DST abbrevs
        char* s = flint_time_format("%Z %z", U(2026,7,15,12,0), "America/New_York");
        CHECK(s && strcmp(s, "EDT -0400") == 0, "fmt %%Z winter=%s", s ? s : "(null)");
        flint_time_free(s);
        s = flint_time_format("%Z %z", U(2026,1,15,12,0), "America/New_York");
        CHECK(s && strcmp(s, "EST -0500") == 0, "fmt %%Z summer=%s", s ? s : "(null)");
        flint_time_free(s);
        s = flint_time_format("%Z", U(2026,7,15,12,0), "Europe/London");
        CHECK(s && strcmp(s, "BST") == 0, "fmt BST=%s", s ? s : "(null)");
        flint_time_free(s);
        // durations incl. the legacy -0d contract from t_time.fl
        char* d1 = flint_time_duration(90061);
        CHECK(d1 && strcmp(d1, "1d01:01:01") == 0, "dur 90061=%s", d1 ? d1 : "(null)");
        free(d1);
        char* d2 = flint_time_duration(-3661);
        CHECK(d2 && strcmp(d2, "-0d01:01:01") == 0, "dur -3661=%s", d2 ? d2 : "(null)");
        free(d2);
        // tz_known: local is a real name; unknown is not
        CHECK(flint_tz_known("local") == 1, "tz_known local");
        CHECK(flint_tz_known("America/New_York") == 1, "tz_known ny");
        CHECK(flint_tz_known("Australia/Lord_Howe") == 1, "tz_known lordhowe");
        CHECK(flint_tz_known("Asia/Kathmandu") == 1, "tz_known ktm");
        CHECK(flint_tz_known("Nowhere/X") == 0, "tz_known bogus");
    }

    // ---- F. fixed-zone absolute offsets (hand-verified vs IANA STDOFF) ----
    // These caught the Pacific/Guam +9-vs-+10 data bug: constants, not oracle.
    struct { const char* z; int64_t off; } fx[] = {
        { "UTC", 0 }, { "IST", 19800 }, { "JST", 32400 },
        { "Pacific/Guam", 36000 }, { "Asia/Kathmandu", 20700 },
        { "Asia/Tehran", 12600 }, { "Asia/Amman", 10800 },
        { "America/Mexico_City", -21600 }, { "Europe/Istanbul", 10800 },
        { "Australia/Darwin", 34200 }, { "ACST", 34200 },
        { "MST", -25200 }, { "America/Phoenix", -25200 },
        { "Africa/Casablanca", 0 }, { "UTC+05:30", 19800 },
        { "UTC-08:00", -28800 }, { "Nowhere/X", 0 }, { NULL, 0 }
    };
    int64_t summer = U(2026, 7, 15, 12, 0), winter = U(2026, 1, 15, 12, 0);
    for (int i = 0; fx[i].z; i++) {
        CHECK(flint_tz_offset_at(fx[i].z, summer) == fx[i].off,
              "fixed %s summer", fx[i].z);
        CHECK(flint_tz_offset_at(fx[i].z, winter) == fx[i].off,
              "fixed %s winter", fx[i].z);
    }
    // pre-1970 floor check: 1969-07-01 is DST under any US-rule era.
    CHECK(flint_tz_offset_at("America/New_York", U(1969,7,1,12,0)) == -14400,
          "pre1970 ny summer");
    CHECK(flint_tz_offset_at("America/New_York", U(1969,1,15,12,0)) == -18000,
          "pre1970 ny winter");

    printf("time_check: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
