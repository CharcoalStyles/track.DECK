#ifndef SYNC_BACKOFF_H
#define SYNC_BACKOFF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded retry count per spec §5 ("2-3 attempts") -- total attempts within
 * one wake cycle, including the first try. */
#define SYNC_BACKOFF_MAX_ATTEMPTS 3

typedef struct {
    int attempts_made;
} sync_backoff_t;

void sync_backoff_reset(sync_backoff_t *b);

/* Whether another attempt is allowed (attempts_made < SYNC_BACKOFF_MAX_ATTEMPTS).
 * Check before each attempt, including the first. */
bool sync_backoff_should_retry(const sync_backoff_t *b);

/* Call once per attempt made (regardless of outcome). */
void sync_backoff_record_attempt(sync_backoff_t *b);

/* Delay to wait before the next attempt, given attempts made so far:
 * 1000ms, 2000ms, 4000ms, ... (doubling per attempt already made). */
uint32_t sync_backoff_delay_ms(const sync_backoff_t *b);

/* poll_interval_seconds to sleep for after giving up on this wake cycle:
 * the last-known-good value if one exists, else a hardcoded fallback. */
int sync_effective_poll_interval(bool has_last_known_good, int last_known_good_seconds, int fallback_seconds);

#ifdef __cplusplus
}
#endif

#endif // SYNC_BACKOFF_H
