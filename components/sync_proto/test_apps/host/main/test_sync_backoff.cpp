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
