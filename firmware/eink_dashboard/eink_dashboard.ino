/*
  Bedside e-ink dashboard  —  today's agenda, this week, weather
  ---------------------------------------------------------------
  Hardware : Waveshare e-Paper ESP32 Driver Board + Waveshare 7.5" e-Paper V2 (800x480 B/W)
             Board switches: "Display Config" = B, the other one = ON (needed to upload).
  IDE      : Arduino IDE 2.x, board package "esp32" by Espressif 3.3.x
             Tools > Board: "ESP32 Dev Module"
             Tools > Partition Scheme: "Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)"
  Libraries: GxEPD2, Adafruit GFX Library (+ Adafruit BusIO), U8g2_for_Adafruit_GFX, ArduinoJson 7.x

  Every wake-up: WiFi -> (NTP every few hours) -> GET the Apps Script JSON -> (OTA if the
  backend offers a newer build) -> draw -> deep sleep.
  Most refreshes are partial (no black/white flash): the last frame is kept in flash so the
  panel knows which pixels changed. A full refresh runs every FULL_REFRESH_EVERY wakes, on the
  first refresh of the morning, and after any reset.
  If the fetch fails it redraws from the last good copy (kept in flash) with an "Offline" tag.
  Press the RST/EN button on the board to force a (full) refresh.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <LittleFS.h>
#include <SPI.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>

#include "config.h"
#include "model.h"
#include "render.h"
#include "schedule.h"

// Bump for every build you publish for OTA, and set the same number as FW_VERSION in Code.gs.
static const int FW_VERSION = 2;

// Waveshare e-Paper ESP32 Driver Board wiring (fixed on the PCB)
static const int PIN_SCK = 13, PIN_MISO = 12, PIN_MOSI = 14, PIN_CS = 15;
static const int PIN_DC = 27, PIN_RST = 26, PIN_BUSY = 25;

// We drive the panel directly (not through GxEPD2_BW) so the frame lives in our own
// canvas, which we can save and replay as the "previous" image for partial refreshes.
PANEL_DRIVER epd(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY);
static const int W = PANEL_DRIVER::WIDTH, H = PANEL_DRIVER::HEIGHT;
static const size_t FRAME_BYTES = (size_t)W / 8 * H;   // 48000
U8G2_FOR_ADAFRUIT_GFX u8g2;
SPIClass hspi(HSPI);

static Dashboard dash;                    // ~24 KB, kept off the stack
static const char* CACHE_FILE = "/dash.json";
static const char* FRAME_FILE = "/frame.bin";

// Survive deep sleep (reset to these values on power-on / RST)
RTC_DATA_ATTR static uint8_t failStreak = 0;
RTC_DATA_ATTR static uint8_t partialsSinceFull = 0;
RTC_DATA_ATTR static time_t lastNtpSync = 0;
RTC_DATA_ATTR static uint8_t apBssid[6];
RTC_DATA_ATTR static int32_t apChannel = 0;          // 0 = unknown -> scan
RTC_DATA_ATTR static int16_t otaBadVersion = 0;      // stop retrying a build that keeps failing
RTC_DATA_ATTR static uint8_t otaFails = 0;

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

/* ------------------------------- network ------------------------------ */

static String macAddr;

static void wifiBegin(int32_t channel, const uint8_t* bssid) {
#if WIFI_ENTERPRISE
  WiFi.begin(WIFI_SSID, WPA2_AUTH_PEAP, WIFI_EAP_IDENTITY, WIFI_EAP_USERNAME, WIFI_PASSWORD,
             nullptr, nullptr, nullptr, -1, channel, bssid);
#else
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, channel, bssid);
#endif
}

static bool waitConnected(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(50);
  return WiFi.status() == WL_CONNECTED;
}

static bool connectWiFi(uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("eink-dashboard");
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);               // straight from eFuse; WiFi.macAddress() is 0s before the radio starts
  char mb[18];
  snprintf(mb, sizeof(mb), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  macAddr = mb;
  Serial.printf("[wifi] MAC %s  (register this MAC if your network requires it)\n", macAddr.c_str());

  uint32_t t0 = millis();
  bool ok = false;
  if (apChannel > 0) {                             // fast path: same AP + channel as last time, no scan
    wifiBegin(apChannel, apBssid);
    ok = waitConnected(4000);
    if (!ok) {
      Serial.println("[wifi] saved AP didn't answer, scanning");
      WiFi.disconnect();
      apChannel = 0;
      delay(50);
    }
  }
  if (!ok) {
    wifiBegin(0, nullptr);
    uint32_t used = millis() - t0;
    ok = waitConnected(used < timeoutMs ? timeoutMs - used : 1000);
  }
  if (ok) {
    memcpy(apBssid, WiFi.BSSID(), 6);
    apChannel = WiFi.channel();
  }
  Serial.printf("[wifi] %s in %lu ms (ch %ld)\n", ok ? "connected" : "FAILED", (unsigned long)(millis() - t0),
                (long)apChannel);
  return ok;
}

static bool syncClock(uint32_t timeoutMs) {
  configTzTime(TZ_POSIX, "pool.ntp.org", "time.google.com", "time.nist.gov");
  uint32_t t0 = millis();
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && millis() - t0 < timeoutMs) delay(50);
  bool ok = sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
  Serial.printf("[ntp] %s in %lu ms\n", ok ? "synced" : "no reply (using RTC)", (unsigned long)(millis() - t0));
  return ok;
}

static void useCertBundle(WiFiClientSecure& client) {
#if defined(ESP_ARDUINO_VERSION) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)
  client.useBuiltinCACertBundle();       // verify TLS certs with the core's root bundle
#else
  client.setInsecure();                  // older cores: no bundle API
#endif
}

static bool fetchDashboard(String& body, String& why, uint32_t& tookMs) {
  WiFiClientSecure client;
  useCertBundle(client);
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
  tookMs = millis() - t0;
  Serial.printf("[http] %u bytes in %lu ms\n", body.length(), (unsigned long)tookMs);
  if (!body.length()) { why = "empty reply"; return false; }
  return true;
}

// The backend advertises the newest build; flash it if it's newer than us. Reboots on success.
static void maybeOta() {
#if OTA_ENABLED
  if (dash.fwVersion <= FW_VERSION || !dash.fwUrl[0]) return;
  if (otaBadVersion == dash.fwVersion && otaFails >= 3) return;
  Serial.printf("[ota] v%d -> v%d from %s\n", FW_VERSION, dash.fwVersion, dash.fwUrl);
  WiFiClientSecure client;
  useCertBundle(client);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // GitHub release assets redirect
  httpUpdate.rebootOnUpdate(true);
  httpUpdate.update(client, dash.fwUrl);                          // only returns if it didn't work
  if (otaBadVersion != dash.fwVersion) { otaBadVersion = dash.fwVersion; otaFails = 0; }
  otaFails++;
  Serial.printf("[ota] failed (%u/3): %s\n", otaFails, httpUpdate.getLastErrorString().c_str());
#endif
}

/* ------------------------------- storage ------------------------------ */

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

static bool haveFrame() {
  File f = LittleFS.open(FRAME_FILE, "r");
  bool ok = f && f.size() == FRAME_BYTES;
  if (f) f.close();
  return ok;
}

static void saveFrame(const uint8_t* buf) {
  LittleFS.remove(FRAME_FILE);           // remove first: the 128 KB partition can't hold two copies
  File f = LittleFS.open(FRAME_FILE, "w");
  if (!f) return;
  size_t n = f.write(buf, FRAME_BYTES);
  f.close();
  if (n != FRAME_BYTES) LittleFS.remove(FRAME_FILE);   // a partial frame would corrupt the next partial refresh
}

// Stream the last frame into the controller's "old" RAM, in strips (no 48 KB buffer needed).
static bool writePrevFrame() {
  File f = LittleFS.open(FRAME_FILE, "r");
  if (!f || f.size() != FRAME_BYTES) { if (f) f.close(); return false; }
  static uint8_t strip[W / 8 * 40];
  for (int y = 0; y < H; y += 40) {
    int rows = H - y < 40 ? H - y : 40;
    size_t want = (size_t)W / 8 * rows;
    if (f.read(strip, want) != want) { f.close(); return false; }
    epd.writeImageToPrevious(strip, 0, y, W, rows);
  }
  f.close();
  return true;
}

/* ------------------------------- display ------------------------------ */

// Push the canvas to the panel: partial when we can tell it what was there before.
static void showFrame(const uint8_t* buf, bool full, bool fsOk) {
  hspi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);   // this board swaps SCK/MOSI vs. stock HSPI
  epd.selectSPI(hspi, SPISettings(4000000, MSBFIRST, SPI_MODE0));
  if (!full) {
    epd.init(115200, false, 2, false);               // don't clear: we're about to load the old image
    if (!writePrevFrame()) {
      full = true;
      Serial.println("[epd] no previous frame, doing a full refresh");
    }
  }
  if (full) epd.init(115200, true, 2, false);        // 2 ms reset pulse for Waveshare's reset circuit
  uint32_t t0 = millis();
  epd.writeImage(buf, 0, 0, W, H);
  epd.refresh(!full);
  epd.hibernate();
  Serial.printf("[epd] %s refresh in %lu ms\n", full ? "full" : "partial", (unsigned long)(millis() - t0));
  partialsSinceFull = full ? 0 : partialsSinceFull + 1;
  if (fsOk) saveFrame(buf);
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
  const bool wokeFromSleep = esp_reset_reason() == ESP_RST_DEEPSLEEP;

  String body, why;
  uint32_t fetchMs = 0;
  bool wifiOk = connectWiFi(20000);
  bool fresh = false, ntpNow = false;
  if (wifiOk) {
    // The RTC keeps time through deep sleep and the backend's timestamp corrects drift,
    // so NTP only needs to run on a cold start and every few hours.
    if (!clockValid() || time(nullptr) - lastNtpSync > NTP_RESYNC_HOURS * 3600L) {
      ntpNow = syncClock(6000);
      if (ntpNow) lastNtpSync = time(nullptr);
    }
    fresh = fetchDashboard(body, why, fetchMs);
  } else {
    why = "No WiFi";
  }

  bool fsOk = LittleFS.begin(true);
  bool haveData = false;
  if (fresh) {
    haveData = parseDashboard(body.c_str(), body.length(), dash);
    if (haveData) {
      if (fsOk) saveCache(body);
      maybeOta();                                      // reboots into the new build if there is one
    } else {
      why = dash.error;                                // e.g. "bad token"
      fresh = false;
    }
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  body = String();                                     // free ~7 KB before the 48 KB canvas

  if (!haveData && fsOk) {
    String cached;
    if (loadCache(cached)) haveData = parseDashboard(cached.c_str(), cached.length(), dash);
  }
  if (fresh && dash.ts && !ntpNow) {                   // no NTP this wake: trust the server's clock if we drifted
    time_t server = (time_t)dash.ts + fetchMs / 2000;
    long drift = (long)(time(nullptr) - server);
    if (!clockValid() || drift > 60 || drift < -60) {
      struct timeval tv = {server, 0};
      settimeofday(&tv, nullptr);
      Serial.printf("[time] corrected %ld s from the server\n", drift);
    }
  }
  failStreak = fresh ? 0 : (failStreak < 250 ? failStreak + 1 : failStreak);

  GFXcanvas1 canvas(W, H);
  if (!canvas.getBuffer()) {
    Serial.println("[epd] out of memory for the frame");
    esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
    esp_deep_sleep_start();
  }
  u8g2.begin(canvas);
  bool full = true;
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
    NowInfo now = nowInfo();
    renderDashboard(canvas, u8g2, dash, now, opt);
    full = wantFullRefresh(wokeFromSleep, fsOk && haveFrame(), partialsSinceFull, FULL_REFRESH_EVERY,
                           now.minutes, QUIET_UNTIL_MIN, REFRESH_MINUTES);
  } else if (!wifiOk) {
    String detail = String("Network: " WIFI_SSID "   -   this board's MAC: ") + macAddr;
    renderMessage(canvas, u8g2, "Can't connect to WiFi", detail.c_str(),
                  "Check WIFI_SSID / WIFI_PASSWORD in secrets.h (or register the MAC). Retrying every 10 minutes.");
  } else if (!clockValid()) {
    renderMessage(canvas, u8g2, "Clock not set", "NTP and the backend both failed.",
                  "Retrying in 15 minutes.");
  } else {
    renderMessage(canvas, u8g2, "Can't load the calendar", why.c_str(),
                  "Check DASH_URL + token in secrets.h, and that the Apps Script is deployed for 'Anyone'.");
  }
  showFrame(canvas.getBuffer(), full, fsOk);

  uint32_t secs = secondsUntilNextWake(fresh);
  Serial.printf("[sleep] %lu s  (awake %lu ms)\n", (unsigned long)secs, (unsigned long)millis());
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {}   // never reached
