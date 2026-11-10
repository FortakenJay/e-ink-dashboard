#pragma once
// Draws the dashboard into any Adafruit_GFX target (the GxEPD2 display on the
// ESP32, or an in-memory canvas on a PC for previews).
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>
#include "model.h"

struct NowInfo {
  int32_t dayNum;   // local date as days since epoch (see daysFromCivil)
  int minutes;      // minutes since local midnight
  int hour;
};

struct RenderOptions {
  bool h24 = false;                 // 24-hour clock
  int tomorrowFrom = 21 * 60 + 30;  // from this minute of the day, the agenda shows tomorrow
  bool stale = false;               // data came from the offline cache
  char status[64] = "";             // footer text, e.g. "Updated 7:42p"
};

void renderDashboard(Adafruit_GFX& gfx, U8G2_FOR_ADAFRUIT_GFX& u8, const Dashboard& d,
                     const NowInfo& now, const RenderOptions& opt);

// Full-screen message for setup problems (no WiFi on first boot, bad token, ...)
void renderMessage(Adafruit_GFX& gfx, U8G2_FOR_ADAFRUIT_GFX& u8, const char* title,
                   const char* detail, const char* hint);
