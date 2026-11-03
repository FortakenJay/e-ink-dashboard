#pragma once
// ======================= EDIT THESE =======================

// --- WiFi ---
#define WIFI_SSID        "Saken"
#define WIFI_PASSWORD    "12345678"

// WPA2-Enterprise (eduroam-style networks: username + password).
// Set to 1 and fill the identity/username; WIFI_PASSWORD is used as the password.
#define WIFI_ENTERPRISE  0
#define WIFI_EAP_IDENTITY "jluoliu@purdue.edu"
#define WIFI_EAP_USERNAME "jluoliu@purdue.edu"

// --- Backend: your Apps Script web-app URL, with the token from Code.gs ---
#define DASH_URL "https://script.google.com/macros/s/AKfycbzK3TQO5uaY9ux2vRnWZ22y-Ecqdcoqehe3VRIXPiBKPCwdqplM1XacHOjV1FvYoQgC8g/exec?token=4846902462"

// --- Time ---
#define TZ_POSIX   "CST6CDT,M3.2.0,M11.1.0"   // America/Chicago (handles DST)
#define TIME_24H   false

// --- Refresh schedule ---
#define REFRESH_MINUTES      30            // refresh on :00 and :30
#define QUIET_FROM_MIN       (0 * 60 + 30) // no refreshes from 00:30 ...
#define QUIET_UNTIL_MIN      (5 * 60 + 30) // ... until 05:30 (fresh before your 6:30 wake-up)
#define TOMORROW_VIEW_FROM   (21 * 60 + 30)// after 9:30 PM the agenda flips to tomorrow

#define NTP_RESYNC_HOURS     6             // between wakes the RTC + backend timestamp keep time

// --- Refresh style ---
// Partial refreshes don't flash black/white; a full one every N wakes clears any ghosting.
// 8 = a full refresh every ~4 hours (plus the first one each morning). 0 = always full.
#define FULL_REFRESH_EVERY   8

// --- Over-the-air updates ---
// When the backend's FW_VERSION is higher than this build's, the board downloads FW_URL and
// reboots into it. Needs the "Minimal SPIFFS (1.9MB APP with OTA)" partition scheme.
#define OTA_ENABLED          1

// --- Panel ---
// Waveshare 7.5" V2 (800x480 B/W). If the picture is faint, blotchy or doesn't
// update, switch to the GDEY075T7 driver (newer "V2 2023" panels).
#define PANEL_DRIVER GxEPD2_750_T7
// #define PANEL_DRIVER GxEPD2_750_GDEY075T7

// =========================================================
