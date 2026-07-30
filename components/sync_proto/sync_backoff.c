#include "sync_backoff.h"

void sync_backoff_reset(sync_backoff_t *b) {
    b->attempts_made = 0;
}

bool sync_backoff_should_retry(const sync_backoff_t *b) {
    return b->attempts_made < SYNC_BACKOFF_MAX_ATTEMPTS;
}

void sync_backoff_record_attempt(sync_backoff_t *b) {
    b->attempts_made++;
}

uint32_t sync_backoff_delay_ms(const sync_backoff_t *b) {
    uint32_t delay = 1000u;
    for (int i = 1; i < b->attempts_made; i++) {
        delay *= 2;
    }
    return delay;
}

int sync_effective_poll_interval(bool has_last_known_good, int last_known_good_seconds, int fallback_seconds) {
    return has_last_known_good ? last_known_good_seconds : fallback_seconds;
}

int sync_effective_alarm_interval_seconds(int seconds_until_next_deadline,
                                           bool have_future_reminder,
                                           int64_t seconds_until_reminder,
                                           int min_seconds,
                                           bool *out_is_reminder_wake) {
    bool reminder_wins = have_future_reminder && seconds_until_reminder < (int64_t)seconds_until_next_deadline;
    int64_t chosen = reminder_wins ? seconds_until_reminder : (int64_t)seconds_until_next_deadline;
    if (chosen < (int64_t)min_seconds) {
        chosen = (int64_t)min_seconds;
    }
    if (out_is_reminder_wake) {
        *out_is_reminder_wake = reminder_wins;
    }
    return (int)chosen;
}
