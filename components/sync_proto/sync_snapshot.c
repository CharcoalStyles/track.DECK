#include "sync_snapshot.h"

#include <string.h>

#include "cJSON.h"

static void copy_str(char *dst, size_t dst_size, const char *src) {
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static bool get_str_field(const cJSON *obj, const char *key, char *dst, size_t dst_size) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    copy_str(dst, dst_size, item->valuestring);
    return true;
}

static bool get_int64_field(const cJSON *obj, const char *key, int64_t *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *out = (int64_t)item->valuedouble;
    return true;
}

static bool get_int_field(const cJSON *obj, const char *key, int *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *out = item->valueint;
    return true;
}

static bool get_double_field(const cJSON *obj, const char *key, double *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *out = item->valuedouble;
    return true;
}

static void parse_checkins(const cJSON *root, sync_snapshot_t *out) {
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "checkins");
    if (!cJSON_IsArray(arr)) {
        out->checkins_valid = false;
        return;
    }
    out->checkins_valid = true;
    out->checkins_count = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, arr) {
        if (out->checkins_count >= SYNC_MAX_CHECKINS) {
            break;
        }
        if (!cJSON_IsObject(entry)) {
            continue;
        }
        sync_checkin_t *c = &out->checkins[out->checkins_count];
        memset(c, 0, sizeof(*c));
        get_str_field(entry, "id", c->id, sizeof(c->id));
        get_str_field(entry, "category", c->category, sizeof(c->category));
        get_str_field(entry, "prompt_text", c->prompt_text, sizeof(c->prompt_text));
        get_int64_field(entry, "scheduled_at", &c->scheduled_at);
        c->has_fired_at = get_int64_field(entry, "fired_at", &c->fired_at);
        out->checkins_count++;
    }
}

static void parse_reminders(const cJSON *root, sync_snapshot_t *out) {
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "reminders");
    if (!cJSON_IsArray(arr)) {
        out->reminders_valid = false;
        return;
    }
    out->reminders_valid = true;
    out->reminders_count = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, arr) {
        if (out->reminders_count >= SYNC_MAX_REMINDERS) {
            break;
        }
        if (!cJSON_IsObject(entry)) {
            continue;
        }
        sync_reminder_t *r = &out->reminders[out->reminders_count];
        memset(r, 0, sizeof(*r));
        get_str_field(entry, "id", r->id, sizeof(r->id));
        get_str_field(entry, "message", r->message, sizeof(r->message));
        get_int64_field(entry, "due_at", &r->due_at);
        r->has_event_uid = get_str_field(entry, "event_uid", r->event_uid, sizeof(r->event_uid));
        out->reminders_count++;
    }
}

static void parse_calendar_events(const cJSON *root, sync_snapshot_t *out) {
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "calendar_events");
    if (!cJSON_IsArray(arr)) {
        out->calendar_events_valid = false;
        return;
    }
    out->calendar_events_valid = true;
    out->calendar_events_count = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, arr) {
        if (out->calendar_events_count >= SYNC_MAX_CALENDAR_EVENTS) {
            break;
        }
        if (!cJSON_IsObject(entry)) {
            continue;
        }
        sync_calendar_event_t *e = &out->calendar_events[out->calendar_events_count];
        memset(e, 0, sizeof(*e));
        get_str_field(entry, "uid", e->uid, sizeof(e->uid));
        get_str_field(entry, "summary", e->summary, sizeof(e->summary));
        get_int64_field(entry, "start", &e->start);
        e->has_end = get_int64_field(entry, "end", &e->end);
        out->calendar_events_count++;
    }
}

static void parse_weather(const cJSON *root, sync_snapshot_t *out) {
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(root, "weather");
    if (!cJSON_IsObject(w)) {
        out->has_weather = false;
        return;
    }
    sync_weather_t weather;
    memset(&weather, 0, sizeof(weather));
    bool ok = true;
    ok = get_double_field(w, "temperature_c", &weather.temperature_c) && ok;
    ok = get_int_field(w, "cloud_cover_pct", &weather.cloud_cover_pct) && ok;
    ok = get_double_field(w, "precipitation_mm", &weather.precipitation_mm) && ok;
    ok = get_double_field(w, "temperature_min_c", &weather.temperature_min_c) && ok;
    ok = get_double_field(w, "temperature_max_c", &weather.temperature_max_c) && ok;
    ok = get_int64_field(w, "sunrise", &weather.sunrise) && ok;
    ok = get_int64_field(w, "sunset", &weather.sunset) && ok;
    if (!ok) {
        out->has_weather = false;
        return;
    }
    out->has_weather = true;
    out->weather = weather;
}

bool sync_snapshot_parse(const char *json_text, sync_snapshot_t *out) {
    memset(out, 0, sizeof(*out));
    if (!json_text) {
        return false;
    }

    cJSON *root = cJSON_Parse(json_text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    out->has_now = get_int64_field(root, "now", &out->now);

    const cJSON *tz = cJSON_GetObjectItemCaseSensitive(root, "timezone");
    if (cJSON_IsObject(tz)) {
        out->has_timezone_posix = get_str_field(tz, "posix", out->timezone_posix, sizeof(out->timezone_posix));
    }

    out->has_poll_interval_seconds = get_int_field(root, "poll_interval_seconds", &out->poll_interval_seconds);
    out->has_bedtime = get_str_field(root, "bedtime", out->bedtime, sizeof(out->bedtime));
    out->has_next_wake_at = get_int64_field(root, "next_wake_at", &out->next_wake_at);

    parse_checkins(root, out);
    parse_reminders(root, out);
    parse_calendar_events(root, out);
    parse_weather(root, out);

    cJSON_Delete(root);
    return true;
}

sync_soonest_reminder_t sync_find_soonest_reminder(const sync_snapshot_t *snap) {
    sync_soonest_reminder_t result;
    memset(&result, 0, sizeof(result));

    if (!snap->reminders_valid) {
        return result;
    }
    for (int i = 0; i < snap->reminders_count; i++) {
        if (!result.have_reminder || snap->reminders[i].due_at < result.due_at) {
            result.have_reminder = true;
            result.due_at = snap->reminders[i].due_at;
            copy_str(result.id, sizeof(result.id), snap->reminders[i].id);
            copy_str(result.message, sizeof(result.message), snap->reminders[i].message);
        }
    }
    return result;
}
