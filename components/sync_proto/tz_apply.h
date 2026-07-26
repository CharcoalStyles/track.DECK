#ifndef TZ_APPLY_H
#define TZ_APPLY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Applies `posix_tz` via setenv("TZ", ...)/tzset() if it's non-NULL and
 * non-empty. Falls back to `fallback_tz` (e.g. the last-known-good value)
 * if `posix_tz` is NULL/empty; if both are NULL/empty, leaves the current
 * TZ untouched. Returns the TZ string that ended up applied, or NULL if
 * nothing was applied. */
const char *sync_tz_apply(const char *posix_tz, const char *fallback_tz);

#ifdef __cplusplus
}
#endif

#endif // TZ_APPLY_H
