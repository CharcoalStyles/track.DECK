#include <catch2/catch_test_macros.hpp>

extern "C" {
#include "sync_backoff.h"
}

TEST_CASE("allows retries up to the bounded max, then gives up", "[sync_backoff]") {
    sync_backoff_t b;
    sync_backoff_reset(&b);

    REQUIRE(sync_backoff_should_retry(&b)); // before the 1st attempt
    sync_backoff_record_attempt(&b);
    REQUIRE(sync_backoff_should_retry(&b)); // before the 2nd attempt
    sync_backoff_record_attempt(&b);
    REQUIRE(sync_backoff_should_retry(&b)); // before the 3rd attempt
    sync_backoff_record_attempt(&b);

    REQUIRE_FALSE(sync_backoff_should_retry(&b)); // 3 attempts made, give up
}

TEST_CASE("never exceeds SYNC_BACKOFF_MAX_ATTEMPTS even if recorded further", "[sync_backoff]") {
    sync_backoff_t b;
    sync_backoff_reset(&b);
    for (int i = 0; i < SYNC_BACKOFF_MAX_ATTEMPTS + 5; i++) {
        sync_backoff_record_attempt(&b);
    }
    REQUIRE_FALSE(sync_backoff_should_retry(&b));
}

TEST_CASE("delay doubles per attempt already made", "[sync_backoff]") {
    sync_backoff_t b;
    sync_backoff_reset(&b);

    sync_backoff_record_attempt(&b); // 1 attempt made
    REQUIRE(sync_backoff_delay_ms(&b) == 1000);

    sync_backoff_record_attempt(&b); // 2 attempts made
    REQUIRE(sync_backoff_delay_ms(&b) == 2000);

    sync_backoff_record_attempt(&b); // 3 attempts made
    REQUIRE(sync_backoff_delay_ms(&b) == 4000);
}

TEST_CASE("reset clears attempt count", "[sync_backoff]") {
    sync_backoff_t b;
    sync_backoff_reset(&b);
    sync_backoff_record_attempt(&b);
    sync_backoff_record_attempt(&b);
    sync_backoff_reset(&b);
    REQUIRE(sync_backoff_should_retry(&b));
    REQUIRE(sync_backoff_delay_ms(&b) == 1000);
}

TEST_CASE("effective poll interval prefers last-known-good over fallback", "[sync_backoff]") {
    REQUIRE(sync_effective_poll_interval(true, 300, 900) == 300);
}

TEST_CASE("effective poll interval uses fallback when no sync has ever succeeded", "[sync_backoff]") {
    REQUIRE(sync_effective_poll_interval(false, 300, 900) == 900);
}

TEST_CASE("alarm interval: no future reminder uses the deadline unchanged", "[sync_effective_alarm_interval]") {
    bool is_reminder_wake = true; // seed with the opposite to prove it gets set false
    int interval = sync_effective_alarm_interval_seconds(300, false, 0, SYNC_MIN_ALARM_INTERVAL_SECONDS, &is_reminder_wake);
    REQUIRE(interval == 300);
    REQUIRE_FALSE(is_reminder_wake);
}

TEST_CASE("alarm interval: a reminder sooner than the deadline wins", "[sync_effective_alarm_interval]") {
    bool is_reminder_wake = false;
    int interval = sync_effective_alarm_interval_seconds(300, true, 60, SYNC_MIN_ALARM_INTERVAL_SECONDS, &is_reminder_wake);
    REQUIRE(interval == 60);
    REQUIRE(is_reminder_wake);
}

TEST_CASE("alarm interval: a reminder later than the deadline loses to the deadline", "[sync_effective_alarm_interval]") {
    bool is_reminder_wake = true;
    int interval = sync_effective_alarm_interval_seconds(300, true, 900, SYNC_MIN_ALARM_INTERVAL_SECONDS, &is_reminder_wake);
    REQUIRE(interval == 300);
    REQUIRE_FALSE(is_reminder_wake);
}

TEST_CASE("alarm interval: a near-immediate reminder clamps to min_seconds", "[sync_effective_alarm_interval]") {
    bool is_reminder_wake = false;
    int interval = sync_effective_alarm_interval_seconds(300, true, 1, SYNC_MIN_ALARM_INTERVAL_SECONDS, &is_reminder_wake);
    REQUIRE(interval == SYNC_MIN_ALARM_INTERVAL_SECONDS);
    REQUIRE(is_reminder_wake);
}

TEST_CASE("alarm interval: a negative seconds-until-reminder also clamps (defensive)", "[sync_effective_alarm_interval]") {
    bool is_reminder_wake = false;
    int interval = sync_effective_alarm_interval_seconds(300, true, -50, SYNC_MIN_ALARM_INTERVAL_SECONDS, &is_reminder_wake);
    REQUIRE(interval == SYNC_MIN_ALARM_INTERVAL_SECONDS);
    REQUIRE(is_reminder_wake);
}

TEST_CASE("alarm interval: a near-immediate deadline (no reminder) also clamps", "[sync_effective_alarm_interval]") {
    bool is_reminder_wake = true;
    int interval = sync_effective_alarm_interval_seconds(1, false, 0, SYNC_MIN_ALARM_INTERVAL_SECONDS, &is_reminder_wake);
    REQUIRE(interval == SYNC_MIN_ALARM_INTERVAL_SECONDS);
    REQUIRE_FALSE(is_reminder_wake);
}

TEST_CASE("alarm interval: out_is_reminder_wake is optional (nullptr doesn't crash)", "[sync_effective_alarm_interval]") {
    int interval = sync_effective_alarm_interval_seconds(300, true, 60, SYNC_MIN_ALARM_INTERVAL_SECONDS, nullptr);
    REQUIRE(interval == 60);
}

TEST_CASE("failure backoff: zero consecutive failures returns base unchanged", "[sync_failure_backoff]") {
    REQUIRE(sync_failure_backoff_seconds(300, 0, 1800) == 300);
}

TEST_CASE("failure backoff: doubles per consecutive failure", "[sync_failure_backoff]") {
    REQUIRE(sync_failure_backoff_seconds(300, 1, 1800) == 600);
    REQUIRE(sync_failure_backoff_seconds(300, 2, 1800) == 1200);
}

TEST_CASE("failure backoff: caps at max_seconds", "[sync_failure_backoff]") {
    REQUIRE(sync_failure_backoff_seconds(300, 3, 1800) == 1800); // uncapped would be 2400
    REQUIRE(sync_failure_backoff_seconds(300, 20, 1800) == 1800); // stays capped, no overflow
}
