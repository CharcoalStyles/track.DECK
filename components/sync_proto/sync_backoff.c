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
