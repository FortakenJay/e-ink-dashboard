#include "model.h"
#include <ArduinoJson.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// strlcpy that never leaves half a UTF-8 character at the end
static void copyUtf8(char* dst, size_t cap, const char* src) {
  if (!cap) return;
  if (!src) { dst[0] = 0; return; }
  size_t n = strlen(src);
  if (n < cap) { memcpy(dst, src, n + 1); return; }
  n = cap - 1;
  while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) n--;   // back up to a lead byte
  memcpy(dst, src, n);
  dst[n] = 0;
}

int32_t parseDayNum(const char* s) {
  int y, m, d;
  if (!s || sscanf(s, "%d-%d-%d", &y, &m, &d) != 3) return -1000000;
  return daysFromCivil(y, (unsigned)m, (unsigned)d);
}

bool parseDashboard(const char* json, size_t len, Dashboard& d) {
  memset(&d, 0, sizeof(d));
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json, len);
  if (err) {
    snprintf(d.error, sizeof(d.error), "JSON: %s", err.c_str());
    return false;
  }
  if (!(doc["ok"] | false)) {
    copyUtf8(d.error, sizeof(d.error), doc["error"] | "server said not ok");
    return false;
  }

  d.ts = doc["ts"] | 0u;
  copyUtf8(d.unitT, sizeof(d.unitT), doc["units"]["t"] | "F");
  copyUtf8(d.unitW, sizeof(d.unitW), doc["units"]["w"] | "mph");

  JsonObjectConst cur = doc["cur"];
  if (!cur.isNull()) {
    d.cur.valid = true;
    d.cur.t = cur["t"] | 0;
    d.cur.feels = cur["f"] | 0;
    d.cur.code = cur["c"] | 0;
    d.cur.wind = cur["w"] | 0;
    d.cur.gust = cur["g"] | 0;
    d.cur.isDay = (cur["d"] | 1) != 0;
  }

  for (JsonObjectConst jd : doc["days"].as<JsonArrayConst>()) {
    if (d.nDays >= MAX_DAYS) break;
    Day& day = d.days[d.nDays++];
    copyUtf8(day.date, sizeof(day.date), jd["date"] | "");
    day.dayNum = parseDayNum(day.date);
    copyUtf8(day.dow, sizeof(day.dow), jd["dow"] | "");
    copyUtf8(day.dowLong, sizeof(day.dowLong), jd["dl"] | "");
    copyUtf8(day.md, sizeof(day.md), jd["md"] | "");
    copyUtf8(day.monthDay, sizeof(day.monthDay), jd["ml"] | "");

    JsonObjectConst w = jd["w"];
    if (!w.isNull()) {
      day.wx.valid = true;
      day.wx.hi = w["hi"] | 0;
      day.wx.lo = w["lo"] | 0;
      day.wx.code = w["c"] | 0;
      day.wx.pop = w["p"] | 0;
      copyUtf8(day.wx.sunrise, sizeof(day.wx.sunrise), w["sr"] | "");
      copyUtf8(day.wx.sunset, sizeof(day.wx.sunset), w["ss"] | "");
      copyUtf8(day.wx.note, sizeof(day.wx.note), w["n"] | "");
    }

    JsonArrayConst h = jd["h"];
    if (!h.isNull() && h.size() >= 24) {
      day.hasHourly = true;
      for (int i = 0; i < 24; i++) {
        day.hourly[i].t = h[i][0] | 0;
        day.hourly[i].code = h[i][1] | 0;
        day.hourly[i].pop = h[i][2] | 0;
      }
    }

    for (JsonObjectConst je : jd["ev"].as<JsonArrayConst>()) {
      if (day.nEvents >= MAX_EVENTS) break;
      Event& e = day.events[day.nEvents++];
      e.s = je["s"] | 0;
      e.e = je["e"] | 0;
      e.kind = je["k"] | 0;
      e.flags = je["f"] | 0;
      copyUtf8(e.title, sizeof(e.title), je["t"] | "");
      copyUtf8(e.loc, sizeof(e.loc), je["l"] | "");
    }
    for (JsonObjectConst ja : jd["ad"].as<JsonArrayConst>()) {
      if (day.nAllDay >= MAX_ALLDAY) break;
      AllDay& a = day.allDay[day.nAllDay++];
      a.kind = ja["k"] | 1;
      copyUtf8(a.title, sizeof(a.title), ja["t"] | "");
    }
  }

  for (JsonObjectConst jk : doc["keys"].as<JsonArrayConst>()) {
    if (d.nKeys >= MAX_KEYS) break;
    KeyItem& k = d.keys[d.nKeys++];
    k.dayNum = parseDayNum(jk["d"] | "");
    k.m = jk["m"] | -1;
    copyUtf8(k.dow, sizeof(k.dow), jk["w"] | "");
    copyUtf8(k.title, sizeof(k.title), jk["t"] | "");
  }

  copyUtf8(d.aiSummary, sizeof(d.aiSummary), doc["ai"]["s"] | "");
  copyUtf8(d.aiFact, sizeof(d.aiFact), doc["ai"]["f"] | "");

  if (d.nDays == 0) {
    copyUtf8(d.error, sizeof(d.error), "no days in payload");
    return false;
  }
  d.ok = true;
  return true;
}
