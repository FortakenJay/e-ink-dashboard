/*
  Bedside e-ink dashboard  —  today's agenda, this week, weather
  ---------------------------------------------------------------
  Hardware : Waveshare e-Paper ESP32 Driver Board + Waveshare 7.5" e-Paper V2 (800x480 B/W)
             Board switches: "Display Config" = B, the other one = ON (needed to upload).
  IDE      : Arduino IDE 2.x, board package "esp32" by Espressif 3.3.x
             Tools > Board: "ESP32 Dev Module"
             Tools > Partition Scheme: "Huge APP (3MB No OTA/1MB SPIFFS)"
  Libraries: GxEPD2, Adafruit GFX Library (+ Adafruit BusIO), U8g2_for_Adafruit_GFX, ArduinoJson 7.x

  Every wake-up: WiFi -> NTP -> GET the Apps Script JSON -> draw -> deep sleep.
  If the fetch fails it redraws from the last good copy (kept in flash) with an "Offline" tag.
  Press the RST/EN button on the board to force a refresh.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <SPI.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <esp_mac.h>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>

#include "config.h"
#include "model.h"
#include "render.h"
#include "schedule.h"

// Waveshare e-Paper ESP32 Driver Board wiring (fixed on the PCB)
static const int PIN_SCK = 13, PIN_MISO = 12, PIN_MOSI = 14, PIN_CS = 15;
static const int PIN_DC = 27, PIN_RST = 26, PIN_BUSY = 25;

GxEPD2_BW<PANEL_DRIVER, PANEL_DRIVER::HEIGHT> display(PANEL_DRIVER(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));
U8G2_FOR_ADAFRUIT_GFX u8g2;
SPIClass hspi(HSPI);

static Dashboard dash;                    // ~24 KB, kept off the stack
RTC_DATA_ATTR static uint8_t failStreak = 0;
static const char* CACHE_FILE = "/dash.json";

/* ------------------------------ helpers ------------------------------ */

static bool clockValid() { return time(nullptr) > 1735689600; }   // after 2025-01-01

static long secOfDay(time_t t) {
  struct tm lt;
  localtime_r(&t, &lt);
  return lt.tm_hour * 3600L + lt.tm_min * 60L + lt.tm_sec;
}

static NowInfo nowInfo() {
  time_t t = time(nullptr);
  t += renderSkewSeconds(secOfDay(t), REFRESH_MINUTES);   // woke early for a slot? draw that slot
  struct tm lt;
  localtime_r(&t, &lt);
  NowInfo n;
  n.dayNum = daysFromCivil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
  n.minutes = lt.tm_hour * 60 + lt.tm_min;
  n.hour = lt.tm_hour;
  return n;
}

// "7:44p" or "Tue 7:44p"
static void fmtClock(time_t t, bool withDay, char* out, size_t n) {
  struct tm lt;
  localtime_r(&t, &lt);
  char day[8] = "";
  if (withDay) strftime(day, sizeof(day), "%a ", &lt);
  if (TIME_24H) {
    snprintf(out, n, "%s%d:%02d", day, lt.tm_hour, lt.tm_min);
  } else {
    int h = lt.tm_hour % 12 == 0 ? 12 : lt.tm_hour % 12;
    snprintf(out, n, "%s%d:%02d%c", day, h, lt.tm_min, lt.tm_hour < 12 ? 'a' : 'p');
  }
}

static String macAddr;

static bool connectWiFi(uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("eink-dashboard");
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);               // straight from eFuse; WiFi.macAddress() is 0s before the radio starts
  char mb[18];
  snprintf(mb, sizeof(mb), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  macAddr = mb;
  Serial.printf("[wifi] MAC %s  (register this on PNW-Media)\n", macAddr.c_str());
#if WIFI_ENTERPRISE
  WiFi.begin(WIFI_SSID, WPA2_AUTH_PEAP, WIFI_EAP_IDENTITY, WIFI_EAP_USERNAME, WIFI_PASSWORD);
#else
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
#endif
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(100);
  bool ok = WiFi.status() == WL_CONNECTED;
  Serial.printf("[wifi] %s in %lu ms\n", ok ? "connected" : "FAILED", (unsigned long)(millis() - t0));
  return ok;
}

static void syncClock(uint32_t timeoutMs) {
  configTzTime(TZ_POSIX, "pool.ntp.org", "time.google.com", "time.nist.gov");
  uint32_t t0 = millis();
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && millis() - t0 < timeoutMs) delay(50);
  Serial.printf("[ntp] %s\n", sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED ? "synced" : "no reply (using RTC)");
}

static bool fetchDashboard(String& body, String& why) {
  WiFiClientSecure client;
#if defined(ESP_ARDUINO_VERSION) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)
  client.useBuiltinCACertBundle();       // verify Google's TLS cert with the core's root bundle
#else
  client.setInsecure();                  // older cores: no bundle API
#endif
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // Apps Script replies 302 -> googleusercontent
  http.setConnectTimeout(10000);
  http.setTimeout(25000);                                   // cold Apps Script starts can take a few s
  if (!http.begin(client, DASH_URL)) { why = "bad DASH_URL"; return false; }
  uint32_t t0 = millis();
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    why = code < 0 ? http.errorToString(code) : String("HTTP ") + code;
    http.end();
    Serial.printf("[http] failed: %s\n", why.c_str());
    return false;
  }
  body = http.getString();
  http.end();
  Serial.printf("[http] %u bytes in %lu ms\n", body.length(), (unsigned long)(millis() - t0));
  if (!body.length()) { why = "empty reply"; return false; }
  return true;
}

static bool saveCache(const String& body) {
  File f = LittleFS.open(CACHE_FILE, "w");
  if (!f) return false;
  size_t n = f.print(body);
  f.close();
  return n == body.length();
}

static bool loadCache(String& body) {
  File f = LittleFS.open(CACHE_FILE, "r");
  if (!f) return false;
  body = f.readString();
  f.close();
  return body.length() > 0;
}

static void initDisplay() {
  hspi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);   // this board swaps SCK/MOSI vs. stock HSPI
  display.epd2.selectSPI(hspi, SPISettings(4000000, MSBFIRST, SPI_MODE0));
  display.init(115200, true, 2, false);             // 2 ms reset pulse for Waveshare's reset circuit
  display.setRotation(0);
  display.setFullWindow();
  u8g2.begin(display);
}

static uint32_t secondsUntilNextWake(bool ok) {
  if (!clockValid()) return 15 * 60;
  if (!ok && failStreak <= 3) return 10 * 60;          // quick retries after a failure
  return (uint32_t)secondsToNextWake(secOfDay(time(nullptr)), REFRESH_MINUTES, TOMORROW_VIEW_FROM,
                                     QUIET_FROM_MIN, QUIET_UNTIL_MIN);
}

/* ------------------------------- main -------------------------------- */

void setup() {
  Serial.begin(115200);
  delay(20);
  setenv("TZ", TZ_POSIX, 1);
  tzset();

  String body, why;
  bool wifiOk = connectWiFi(20000);
  bool fresh = false;
  if (wifiOk) {
    syncClock(6000);
    fresh = fetchDashboard(body, why);
  } else {
    why = "No WiFi";
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  bool fsOk = LittleFS.begin(true);
  bool haveData = false;
  if (fresh) {
    haveData = parseDashboard(body.c_str(), body.length(), dash);
    if (haveData) {
      if (fsOk) saveCache(body);
    } else {
      why = dash.error;                                // e.g. "bad token"
      fresh = false;
    }
  }
  if (!haveData && fsOk) {
    String cached;
    if (loadCache(cached)) haveData = parseDashboard(cached.c_str(), cached.length(), dash);
  }
  if (!clockValid() && fresh && dash.ts) {             // NTP blocked? trust the server's clock
    struct timeval tv = {(time_t)dash.ts, 0};
    settimeofday(&tv, nullptr);
  }
  failStreak = fresh ? 0 : (failStreak < 250 ? failStreak + 1 : failStreak);

  initDisplay();
  if (haveData && clockValid()) {
    RenderOptions opt;
    opt.h24 = TIME_24H;
    opt.tomorrowFrom = TOMORROW_VIEW_FROM;
    opt.stale = !fresh;
    char t[24];
    if (fresh) {
      fmtClock(time(nullptr), false, t, sizeof(t));
      snprintf(opt.status, sizeof(opt.status), "Updated %s", t);
    } else {
      fmtClock((time_t)dash.ts, true, t, sizeof(t));
      snprintf(opt.status, sizeof(opt.status), "Offline (%s) - last sync %s", why.c_str(), t);
    }
    renderDashboard(display, u8g2, dash, nowInfo(), opt);
  } else if (!wifiOk) {
    String detail = String("Network: " WIFI_SSID "   -   this board's MAC: ") + macAddr;
    renderMessage(display, u8g2, "Can't connect to WiFi", detail.c_str(),
                  "Check WIFI_SSID / WIFI_PASSWORD in config.h (or register the MAC). Retrying every 10 minutes.");
  } else if (!clockValid()) {
    renderMessage(display, u8g2, "Clock not set", "NTP and the backend both failed.",
                  "Retrying in 15 minutes.");
  } else {
    renderMessage(display, u8g2, "Can't load the calendar", why.c_str(),
                  "Check DASH_URL + token in config.h, and that the Apps Script is deployed for 'Anyone'.");
  }
  display.display(false);   // full refresh (clears ghosting)
  display.hibernate();

  uint32_t secs = secondsUntilNextWake(fresh);
  Serial.printf("[sleep] %lu s\n", (unsigned long)secs);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {}   // never reached
