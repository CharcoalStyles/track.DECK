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

/* Growing backoff on top of the normal poll interval after consecutive
 * whole-cycle failures (wifi never connected, or /device/sync failed after
 * its own retries) -- doubles base_interval_seconds per consecutive
 * failure, capped at max_seconds, so a sustained outage doesn't keep
 * retrying (and burning battery) at the normal healthy cadence forever.
 * Returns base_interval_seconds unchanged when consecutive_failures <= 0. */
int sync_failure_backoff_seconds(int base_interval_seconds, int consecutive_failures, int max_seconds);

/* Floor on the scheduled alarm interval -- the RTC alarm write needs time
 * to actually take effect and the device still needs to get to sleep, so
 * the alarm is never scheduled for "now" or the past. */
#define SYNC_MIN_ALARM_INTERVAL_SECONDS 5

/* Decides the next sleep interval: seconds_until_next_deadline (the normal
 * sync/poll deadline), or seconds_until_reminder if a future reminder is
 * sooner. Floored at min_seconds either way. *out_is_reminder_wake reports
 * which one won, so the caller can persist whether its next "timer" wake
 * should take the lightweight reminder-only path or a full sync. */
int sync_effective_alarm_interval_seconds(int seconds_until_next_deadline,
                                           bool have_future_reminder,
                                           int64_t seconds_until_reminder,
                                           int min_seconds,
                                           bool *out_is_reminder_wake);

#ifdef __cplusplus
}
#endif

#endif // SYNC_BACKOFF_H
