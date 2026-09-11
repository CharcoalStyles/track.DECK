#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>

extern "C" {
#include "sync_snapshot.h"
}

// Real example payload from ESP32_FIRMWARE_SPEC.md section 3.1.
static const char *kFullResponse = R"({
  "now": 1785096505,
  "timezone": {
    "iana": "Australia/Canberra",
    "posix": "AEST-10AEDT,M10.1.0,M4.1.0/3"
  },
  "poll_interval_seconds": 300,
  "bedtime": "21:20",
  "next_wake_at": 1785099600,
  "checkins": [
    {
      "id": "ae5e8021-05d2-4ad5-96c8-e8c62fe75020",
      "category": "low",
      "prompt_text": "Find the furthest object you can see and focus on it for a few seconds. How do your eyes feel now?",
      "scheduled_at": 1785096504,
      "fired_at": 1785096504
    }
  ],
  "reminders": [
    {
      "id": "8bbcd662-a4c7-4dae-a6c5-e1fd033abd12",
      "message": "Test reminder from the dashboard",
      "due_at": 1785096515,
      "event_uid": null
    }
  ],
  "calendar_events": [
    {
      "uid": "a1b2c3d4",
      "summary": "Dentist appointment",
      "start": 1785171600,
      "end": 1785175200
    }
  ],
  "weather": {
    "temperature_c": 0.5,
    "cloud_cover_pct": 100,
    "precipitation_mm": 0.0,
    "temperature_min_c": 0.2,
    "temperature_max_c": 11.6,
    "sunrise": 1785099720,
    "sunset": 1785136620
  }
})";

TEST_CASE("parses a full well-formed response", "[sync_snapshot]") {
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(kFullResponse, &snap));

    REQUIRE(snap.has_now);
    REQUIRE(snap.now == 1785096505);

    REQUIRE(snap.has_timezone_posix);
    REQUIRE(std::string(snap.timezone_posix) == "AEST-10AEDT,M10.1.0,M4.1.0/3");

    REQUIRE(snap.has_poll_interval_seconds);
    REQUIRE(snap.poll_interval_seconds == 300);

    REQUIRE(snap.has_bedtime);
    REQUIRE(std::string(snap.bedtime) == "21:20");

    REQUIRE(snap.has_next_wake_at);
    REQUIRE(snap.next_wake_at == 1785099600);

    REQUIRE(snap.checkins_valid);
    REQUIRE(snap.checkins_count == 1);
    REQUIRE(std::string(snap.checkins[0].id) == "ae5e8021-05d2-4ad5-96c8-e8c62fe75020");
    REQUIRE(std::string(snap.checkins[0].category) == "low");
    REQUIRE(snap.checkins[0].scheduled_at == 1785096504);
    REQUIRE(snap.checkins[0].has_fired_at);
    REQUIRE(snap.checkins[0].fired_at == 1785096504);

    REQUIRE(snap.reminders_valid);
    REQUIRE(snap.reminders_count == 1);
    REQUIRE(std::string(snap.reminders[0].message) == "Test reminder from the dashboard");
    REQUIRE_FALSE(snap.reminders[0].has_event_uid); // null event_uid

    REQUIRE(snap.calendar_events_valid);
    REQUIRE(snap.calendar_events_count == 1);
    REQUIRE(std::string(snap.calendar_events[0].summary) == "Dentist appointment");
    REQUIRE(snap.calendar_events[0].has_end);
    REQUIRE(snap.calendar_events[0].end == 1785175200);

    REQUIRE(snap.has_weather);
    REQUIRE(snap.weather.cloud_cover_pct == 100);
    REQUIRE(snap.weather.sunrise == 1785099720);
}

TEST_CASE("a checkin with fired_at: null is not live", "[sync_snapshot]") {
    const char *json = R"({
      "checkins": [
        {"id": "x", "category": "low", "prompt_text": "p", "scheduled_at": 100, "fired_at": null}
      ]
    })";
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(json, &snap));
    REQUIRE(snap.checkins_valid);
    REQUIRE(snap.checkins_count == 1);
    REQUIRE_FALSE(snap.checkins[0].has_fired_at);
}

TEST_CASE("weather: null degrades to has_weather=false, not a parse failure", "[sync_snapshot]") {
    const char *json = R"({"now": 1, "weather": null})";
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(json, &snap));
    REQUIRE(snap.has_now);
    REQUIRE_FALSE(snap.has_weather);
}

TEST_CASE("a malformed weather object degrades only that section", "[sync_snapshot]") {
    const char *json = R"({
      "now": 1,
      "poll_interval_seconds": 300,
      "weather": {"temperature_c": "not a number"}
    })";
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(json, &snap));
    REQUIRE(snap.has_now);
    REQUIRE(snap.has_poll_interval_seconds);
    REQUIRE_FALSE(snap.has_weather);
}

TEST_CASE("checkins as the wrong type degrades only that section", "[sync_snapshot]") {
    const char *json = R"({"now": 1, "checkins": "not an array"})";
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(json, &snap));
    REQUIRE(snap.has_now);
    REQUIRE_FALSE(snap.checkins_valid);
}

TEST_CASE("missing timezone.posix falls back gracefully", "[sync_snapshot]") {
    const char *json = R"({"now": 1, "timezone": {"iana": "UTC", "posix": null}})";
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(json, &snap));
    REQUIRE_FALSE(snap.has_timezone_posix);
}

TEST_CASE("garbage input fails the whole parse", "[sync_snapshot]") {
    sync_snapshot_t snap;
    REQUIRE_FALSE(sync_snapshot_parse("not json at all {{{", &snap));
}

TEST_CASE("a JSON array at the root (not an object) fails the whole parse", "[sync_snapshot]") {
    sync_snapshot_t snap;
    REQUIRE_FALSE(sync_snapshot_parse("[1, 2, 3]", &snap));
}

TEST_CASE("null input fails the whole parse without crashing", "[sync_snapshot]") {
    sync_snapshot_t snap;
    REQUIRE_FALSE(sync_snapshot_parse(nullptr, &snap));
}

TEST_CASE("empty object parses successfully with everything degraded", "[sync_snapshot]") {
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse("{}", &snap));
    REQUIRE_FALSE(snap.has_now);
    REQUIRE_FALSE(snap.has_timezone_posix);
    REQUIRE_FALSE(snap.has_poll_interval_seconds);
    REQUIRE_FALSE(snap.checkins_valid);
    REQUIRE_FALSE(snap.reminders_valid);
    REQUIRE_FALSE(snap.calendar_events_valid);
    REQUIRE_FALSE(snap.has_weather);
}

TEST_CASE("sync_find_soonest_reminder: no reminders section -> have_reminder=false", "[sync_soonest_reminder]") {
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse("{}", &snap));
    sync_soonest_reminder_t soonest = sync_find_soonest_reminder(&snap);
    REQUIRE_FALSE(soonest.have_reminder);
}

TEST_CASE("sync_find_soonest_reminder: reminders present but empty array -> have_reminder=false", "[sync_soonest_reminder]") {
    const char *json = R"({"reminders": []})";
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(json, &snap));
    REQUIRE(snap.reminders_valid);
    sync_soonest_reminder_t soonest = sync_find_soonest_reminder(&snap);
    REQUIRE_FALSE(soonest.have_reminder);
}

TEST_CASE("sync_find_soonest_reminder: single reminder", "[sync_soonest_reminder]") {
    const char *json = R"({
      "reminders": [
        {"id": "r1", "message": "Take pills", "due_at": 1000}
      ]
    })";
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(json, &snap));
    sync_soonest_reminder_t soonest = sync_find_soonest_reminder(&snap);
    REQUIRE(soonest.have_reminder);
    REQUIRE(std::string(soonest.id) == "r1");
    REQUIRE(std::string(soonest.message) == "Take pills");
    REQUIRE(soonest.due_at == 1000);
}

TEST_CASE("sync_find_soonest_reminder: picks the smallest due_at among several", "[sync_soonest_reminder]") {
    const char *json = R"({
      "reminders": [
        {"id": "r1", "message": "later", "due_at": 5000},
        {"id": "r2", "message": "soonest", "due_at": 1000},
        {"id": "r3", "message": "middle", "due_at": 3000}
      ]
    })";
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(json, &snap));
    sync_soonest_reminder_t soonest = sync_find_soonest_reminder(&snap);
    REQUIRE(soonest.have_reminder);
    REQUIRE(std::string(soonest.id) == "r2");
    REQUIRE(soonest.due_at == 1000);
}

TEST_CASE("sync_find_soonest_reminder: calendar events are ignored, only reminders count", "[sync_soonest_reminder]") {
    const char *json = R"({
      "reminders": [
        {"id": "r1", "message": "the only reminder", "due_at": 5000}
      ],
      "calendar_events": [
        {"uid": "e1", "summary": "earlier but not a reminder", "start": 100}
      ]
    })";
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(json, &snap));
    sync_soonest_reminder_t soonest = sync_find_soonest_reminder(&snap);
    REQUIRE(soonest.have_reminder);
    REQUIRE(std::string(soonest.id) == "r1");
    REQUIRE(soonest.due_at == 5000);
}

TEST_CASE("parses a full alert_sounds array, ignoring duration_seconds", "[alert_sounds]") {
    const char *json = R"({
      "alert_sounds": [
        {
          "id": "3f1e9c2a-1111",
          "sha256": "b5b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1",
          "size_bytes": 48044,
          "duration_seconds": 1.5,
          "volume": 75,
          "url": "/device/alert-sounds/3f1e9c2a-1111"
        },
        {
          "id": "3f1e9c2a-2222",
          "sha256": "c6c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2",
          "size_bytes": 12000,
          "volume": 100,
          "url": "/device/alert-sounds/3f1e9c2a-2222"
        }
      ]
    })";
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(json, &snap));
    REQUIRE(snap.alert_sounds_valid);
    REQUIRE(snap.alert_sounds_count == 2);
    REQUIRE(std::string(snap.alert_sounds[0].id) == "3f1e9c2a-1111");
    REQUIRE(std::string(snap.alert_sounds[0].sha256) == "b5b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1");
    REQUIRE(snap.alert_sounds[0].size_bytes == 48044);
    REQUIRE(snap.alert_sounds[0].volume == 75);
    REQUIRE(std::string(snap.alert_sounds[0].url) == "/device/alert-sounds/3f1e9c2a-1111");
    REQUIRE(snap.alert_sounds[1].volume == 100);
}

TEST_CASE("alert_sounds section absent -> alert_sounds_valid=false", "[alert_sounds]") {
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse("{}", &snap));
    REQUIRE_FALSE(snap.alert_sounds_valid);
    REQUIRE(snap.alert_sounds_count == 0);
}

TEST_CASE("alert_sounds volume missing defaults to 100", "[alert_sounds]") {
    const char *json = R"({"alert_sounds": [{"id": "s1", "sha256": "x", "size_bytes": 1, "url": "/u"}]})";
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(json, &snap));
    REQUIRE(snap.alert_sounds_count == 1);
    REQUIRE(snap.alert_sounds[0].volume == 100);
}

TEST_CASE("alert_sounds volume out of range clamps to 0-100", "[alert_sounds]") {
    const char *json = R"({
      "alert_sounds": [
        {"id": "s1", "sha256": "x", "size_bytes": 1, "url": "/u", "volume": 150},
        {"id": "s2", "sha256": "x", "size_bytes": 1, "url": "/u", "volume": -5}
      ]
    })";
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(json, &snap));
    REQUIRE(snap.alert_sounds[0].volume == 100);
    REQUIRE(snap.alert_sounds[1].volume == 0);
}

TEST_CASE("alert_sounds volume of the wrong type defaults to 100", "[alert_sounds]") {
    const char *json = R"({"alert_sounds": [{"id": "s1", "sha256": "x", "size_bytes": 1, "url": "/u", "volume": "loud"}]})";
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(json, &snap));
    REQUIRE(snap.alert_sounds[0].volume == 100);
}

TEST_CASE("alert_sounds entries beyond the cap are dropped, not overflowed", "[alert_sounds]") {
    std::string json = R"({"alert_sounds": [)";
    for (int i = 0; i < SYNC_MAX_ALERT_SOUNDS + 5; i++) {
        if (i > 0) json += ",";
        json += "{\"id\": \"s" + std::to_string(i) + "\", \"sha256\": \"x\", \"size_bytes\": 1, \"url\": \"/u\"}";
    }
    json += "]}";
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(json.c_str(), &snap));
    REQUIRE(snap.alert_sounds_count == SYNC_MAX_ALERT_SOUNDS);
}

TEST_CASE("reminders alert_sound_id: null degrades to unpinned", "[alert_sounds]") {
    const char *json = R"({"reminders": [{"id": "r1", "message": "m", "due_at": 1, "alert_sound_id": null}]})";
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(json, &snap));
    REQUIRE_FALSE(snap.reminders[0].has_alert_sound_id);
}

TEST_CASE("reminders alert_sound_id key absent degrades to unpinned", "[alert_sounds]") {
    const char *json = R"({"reminders": [{"id": "r1", "message": "m", "due_at": 1}]})";
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(json, &snap));
    REQUIRE_FALSE(snap.reminders[0].has_alert_sound_id);
}

TEST_CASE("sync_find_soonest_reminder resolves a matching pinned alert_sound_id", "[alert_sounds]") {
    const char *json = R"({
      "alert_sounds": [
        {"id": "snd1", "sha256": "x", "size_bytes": 1, "volume": 60, "url": "/u"}
      ],
      "reminders": [
        {"id": "r1", "message": "m", "due_at": 1, "alert_sound_id": "snd1"}
      ]
    })";
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(json, &snap));
    sync_soonest_reminder_t soonest = sync_find_soonest_reminder(&snap);
    REQUIRE(std::string(soonest.alert_sound_id) == "snd1");
    REQUIRE(soonest.alert_sound_volume == 60);
}

TEST_CASE("sync_find_soonest_reminder degrades a pinned id that matches nothing", "[alert_sounds]") {
    const char *json = R"({
      "alert_sounds": [
        {"id": "snd1", "sha256": "x", "size_bytes": 1, "volume": 60, "url": "/u"}
      ],
      "reminders": [
        {"id": "r1", "message": "m", "due_at": 1, "alert_sound_id": "does-not-exist"}
      ]
    })";
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(json, &snap));
    sync_soonest_reminder_t soonest = sync_find_soonest_reminder(&snap);
    REQUIRE(std::string(soonest.alert_sound_id).empty());
    REQUIRE(soonest.alert_sound_volume == 100);
}

TEST_CASE("sync_resolve_alert_sound: unpinned reminder", "[alert_sounds]") {
    const char *json = R"({"reminders": [{"id": "r1", "message": "m", "due_at": 1}]})";
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(json, &snap));
    char id[SYNC_STR_ID_LEN];
    int volume;
    sync_resolve_alert_sound(&snap, &snap.reminders[0], id, sizeof(id), &volume);
    REQUIRE(std::string(id).empty());
    REQUIRE(volume == 100);
}

TEST_CASE("sync_resolve_alert_sound: pinned but alert_sounds section invalid", "[alert_sounds]") {
    const char *json = R"({"reminders": [{"id": "r1", "message": "m", "due_at": 1, "alert_sound_id": "snd1"}]})";
    sync_snapshot_t snap;
    REQUIRE(sync_snapshot_parse(json, &snap));
    REQUIRE_FALSE(snap.alert_sounds_valid);
    char id[SYNC_STR_ID_LEN];
    int volume;
    sync_resolve_alert_sound(&snap, &snap.reminders[0], id, sizeof(id), &volume);
    REQUIRE(std::string(id).empty());
    REQUIRE(volume == 100);
}
