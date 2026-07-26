#include <catch2/catch_test_macros.hpp>
#include <cstring>

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
