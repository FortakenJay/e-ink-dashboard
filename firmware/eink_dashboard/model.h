#pragma once
// Data model for the dashboard + JSON parsing (ArduinoJson 7).
// Kept free of Arduino-only types so the same code also builds on a PC for previews.
#include <stdint.h>
#include <stddef.h>

static const int MAX_DAYS = 7;
static const int MAX_EVENTS = 24;
static const int MAX_ALLDAY = 4;
static const int MAX_KEYS = 4;

enum EventKind : uint8_t { KIND_ROUTINE = 0, KIND_ANCHOR = 1, KIND_KEY = 2 };

struct Event {
  int16_t s, e;          // minutes since local midnight (e may be 1440)
  uint8_t kind;          // EventKind
  uint8_t flags;         // 1 = started yesterday, 2 = continues tomorrow
  char title[72];        // UTF-8, already cleaned server-side
  char loc[36];
};

struct AllDay {
  uint8_t kind;
  char title[72];
};

struct DayWeather {
  bool valid;
  int16_t hi, lo;
  int16_t code;
  uint8_t pop;
  char sunrise[6], sunset[6];   // "07:16"
  char note[64];                // "Rain 1p-7p · Gusts 29 mph"
};

struct HourWeather {
  int16_t t;
  int16_t code;
  uint8_t pop;
};

struct Day {
  int32_t dayNum;               // days since 1970-01-01 (local date)
  char date[11];                // "2026-10-26"
  char dow[4];                  // "Mon"
  char dowLong[10];             // "Monday"
  char md[8];                   // "Oct 26"
  char monthDay[16];            // "October 26"
  DayWeather wx;
  bool hasHourly;
  HourWeather hourly[24];
  uint8_t nEvents;
  Event events[MAX_EVENTS];
  uint8_t nAllDay;
  AllDay allDay[MAX_ALLDAY];
};

struct KeyItem {
  int32_t dayNum;
  int16_t m;                    // start minute, -1 = all-day
  char dow[4];
  char title[72];
};

struct Current {
  bool valid;
  int16_t t, feels, wind, gust, code;
  bool isDay;
};

struct Dashboard {
  bool ok;
  uint32_t ts;                  // server time (unix) when generated
  char unitT[2];                // "F" / "C"
  char unitW[6];                // "mph" / "km/h"
  Current cur;
  uint8_t nDays;
  Day days[MAX_DAYS];
  uint8_t nKeys;
  KeyItem keys[MAX_KEYS];
  char aiSummary[200];          // Gemini's brief of the day ("" = no AI box)
  char aiFact[240];             // fun fact of the day
  int16_t fwVersion;            // newest firmware the backend offers (0 = none)
  char fwUrl[200];              // where to download it (HTTPS .bin)
  char error[64];
};

// Howard Hinnant's days_from_civil: proleptic Gregorian date -> days since epoch
inline int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

// "2026-10-26" -> day number, or INT32_MIN-ish (-1000000) on bad input
int32_t parseDayNum(const char* ymd);

// Parse the backend JSON into `out`. Returns false and fills out.error on failure.
bool parseDashboard(const char* json, size_t len, Dashboard& out);
