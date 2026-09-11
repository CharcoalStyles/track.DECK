#ifndef SYNC_SNAPSHOT_H
#define SYNC_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNC_MAX_CHECKINS 8
#define SYNC_MAX_REMINDERS 8
#define SYNC_MAX_CALENDAR_EVENTS 8
#define SYNC_MAX_ALERT_SOUNDS 16
#define SYNC_STR_ID_LEN 40
#define SYNC_STR_SHORT_LEN 32
#define SYNC_STR_TEXT_LEN 256
#define SYNC_STR_SHA256_LEN 65 // 64 hex chars + NUL
#define SYNC_STR_URL_LEN 80

typedef struct {
    char id[SYNC_STR_ID_LEN];
    char category[SYNC_STR_SHORT_LEN];
    char prompt_text[SYNC_STR_TEXT_LEN];
    int64_t scheduled_at;
    bool has_fired_at;
    int64_t fired_at;
} sync_checkin_t;

typedef struct {
    char id[SYNC_STR_ID_LEN];
    char message[SYNC_STR_TEXT_LEN];
    int64_t due_at;
    bool has_event_uid;
    char event_uid[SYNC_STR_ID_LEN];
    bool has_alert_sound_id;
    char alert_sound_id[SYNC_STR_ID_LEN];
} sync_reminder_t;

typedef struct {
    char id[SYNC_STR_ID_LEN];
    char sha256[SYNC_STR_SHA256_LEN];
    int size_bytes;
    int volume; // 0-100, always populated (clamped; defaults to 100 if missing/malformed)
    char url[SYNC_STR_URL_LEN];
} sync_alert_sound_t;

typedef struct {
    char uid[SYNC_STR_ID_LEN];
    char summary[SYNC_STR_TEXT_LEN];
    int64_t start;
    bool has_end;
    int64_t end;
} sync_calendar_event_t;

typedef struct {
    double temperature_c;
    int cloud_cover_pct;
    double precipitation_mm;
    double temperature_min_c;
    double temperature_max_c;
    int64_t sunrise;
    int64_t sunset;
} sync_weather_t;

typedef struct {
    bool has_now;
    int64_t now;

    bool has_timezone_posix;
    char timezone_posix[64];

    bool has_poll_interval_seconds;
    int poll_interval_seconds;

    bool has_bedtime;
    char bedtime[8];

    bool has_next_wake_at;
    int64_t next_wake_at;

    /* Each section below degrades independently: a malformed or missing
       section clears its *_valid/has_* flag but does not fail the parse
       of the rest of the document (spec's "degrade gracefully" rule). */
    bool checkins_valid;
    int checkins_count;
    sync_checkin_t checkins[SYNC_MAX_CHECKINS];

    bool reminders_valid;
    int reminders_count;
    sync_reminder_t reminders[SYNC_MAX_REMINDERS];

    bool calendar_events_valid;
    int calendar_events_count;
    sync_calendar_event_t calendar_events[SYNC_MAX_CALENDAR_EVENTS];

    bool alert_sounds_valid;
    int alert_sounds_count;
    sync_alert_sound_t alert_sounds[SYNC_MAX_ALERT_SOUNDS];

    bool has_weather;
    sync_weather_t weather;
} sync_snapshot_t;

/* Parses `json_text` into `out` (zero-initialized internally first).
 * Returns false only if the root document itself isn't a parseable JSON
 * object; any other missing/malformed field just clears that field's own
 * validity flag instead of failing the whole parse. */
bool sync_snapshot_parse(const char *json_text, sync_snapshot_t *out);

typedef struct {
    bool have_reminder;
    char id[SYNC_STR_ID_LEN];
    char message[SYNC_STR_TEXT_LEN];
    int64_t due_at;
    char alert_sound_id[SYNC_STR_ID_LEN]; // empty string = unpinned
    int alert_sound_volume;               // meaningful only when alert_sound_id[0] != '\0'
} sync_soonest_reminder_t;

/* Soonest-due reminder by due_at (past or future -- caller compares
 * against "now" themselves). Calendar events are deliberately excluded --
 * only reminders drive the reminder-wake/chime feature, unlike
 * find_today_items()'s merged reminders+events display logic in the
 * firmware app itself. */
sync_soonest_reminder_t sync_find_soonest_reminder(const sync_snapshot_t *snap);

/* Resolves reminder->alert_sound_id (if set) against snap->alert_sounds[].
 * Writes out_id[0] = '\0' and *out_volume = 100 if the reminder has no pin,
 * if snap->alert_sounds_valid is false, or if the pinned id doesn't match
 * any parsed alert_sounds[] entry -- degrades to "no pin" rather than trust
 * an unresolvable id, matching this file's "malformed data degrades
 * gracefully" convention. */
void sync_resolve_alert_sound(const sync_snapshot_t *snap, const sync_reminder_t *reminder,
                               char *out_id, size_t out_id_size, int *out_volume);

#ifdef __cplusplus
}
#endif

#endif // SYNC_SNAPSHOT_H
