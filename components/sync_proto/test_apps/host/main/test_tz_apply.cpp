#include <catch2/catch_test_macros.hpp>
#include <ctime>
#include <string>

extern "C" {
#include "tz_apply.h"
}

// A fixed UTC instant: 2026-01-15 00:00:00 UTC (epoch seconds), safely outside
// the AEDT/AEST DST-transition window used below.
static const time_t kJanInstant = 1768435200;

TEST_CASE("applies a posix TZ string via setenv/tzset", "[tz_apply]") {
    const char *applied = sync_tz_apply("AEST-10AEDT,M10.1.0,M4.1.0/3", nullptr);
    REQUIRE(applied != nullptr);

    struct tm local_tm;
    localtime_r(&kJanInstant, &local_tm);
    // January in Canberra is AEDT (UTC+11): 00:00 UTC -> 11:00 local.
    REQUIRE(local_tm.tm_hour == 11);
}

TEST_CASE("falls back to fallback_tz when posix_tz is NULL", "[tz_apply]") {
    const char *applied = sync_tz_apply(nullptr, "UTC0");
    REQUIRE(applied != nullptr);
    REQUIRE(std::string(applied) == "UTC0");

    struct tm local_tm;
    localtime_r(&kJanInstant, &local_tm);
    REQUIRE(local_tm.tm_hour == 0);
}

TEST_CASE("falls back to fallback_tz when posix_tz is empty", "[tz_apply]") {
    const char *applied = sync_tz_apply("", "UTC0");
    REQUIRE(applied != nullptr);
    REQUIRE(std::string(applied) == "UTC0");
}

TEST_CASE("returns NULL and leaves TZ untouched when both are NULL", "[tz_apply]") {
    sync_tz_apply("UTC0", nullptr); // establish a known baseline
    const char *applied = sync_tz_apply(nullptr, nullptr);
    REQUIRE(applied == nullptr);

    struct tm local_tm;
    localtime_r(&kJanInstant, &local_tm);
    REQUIRE(local_tm.tm_hour == 0); // still UTC0 from the baseline call
}
