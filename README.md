# Bedside e-ink dashboard

Waveshare 7.5" e-Paper V2 (800×480) + Waveshare e-Paper ESP32 Driver Board.
Shows today's time blocks, a countdown to deadlines, the weather that matters when you walk
everywhere, the week ahead, and a Gemini-written brief of the day with a fun fact — pulled from **jihbin.luo@gmail.com**. Read it instead of your phone.

```
┌──────────────── left: TODAY ────────────────┬──────── right ─────────┐
│ Tuesday            ┌ COMING UP ───────────┐  │ ☁ 47°  Overcast         │
│ October 27         │ 3d  NVIDIA Hackathon │  │ H 51° L 39° · Wind 12   │
│─────────────────────└──────────────────────┘  │ [Rain 1p-7p · Gusts 29] │
│ TODAY                         11 of 13 left  │ 8a  11a  2p  5p  8p     │
│   ✓ 2 blocks done                            │─────────────────────────│
│ ███ 7:15a Homework (Graphics+Stats) until 9:15│ THIS WEEK               │
│ ▶  9:30a Computer Graphics          1h15     │ Wed 28   ☁ 46°/34°      │
│   11:00a AV Work                       4h    │  Physics 8a · NLP 9:30a │
│   ...                                        │ [Fri 30] ! NVIDIA due   │
│ ╭ Graphics, AV Work, Stats back to back -  ╮ │                         │
│ │ rain 1p-7p, pack the umbrella.           │ │                         │
│ ╰ [FUN FACT] Octopuses have three hearts...╯ │                         │
└──────────────────────────────────────────────┴─────────────────────────┘
```

## How it works

```
ESP32 wakes ──WiFi──► Apps Script web app (runs as you) ──► Google Calendar
   │                          ├──────────────────────────► Open-Meteo (weather, no key)
   │                          └──────────────────────────► Gemini (brief + fun fact, cached)
   │◄──────── ~7 KB JSON ─────┘
   └─ draws, saves a copy to flash, deep-sleeps until the next :00 / :30
```

The backend does the heavy lifting (recurring events, time zones, emoji stripping, rain
windows, deadline detection), so the firmware only draws.

**Behaviour**
- Refreshes every 30 min, sleeps 00:30–05:30, so it's fresh before your 6:30 wake-up.
- After **9:30 PM** the agenda flips to **tomorrow** — what you see before bed is what you wake up to.
- Finished blocks collapse into "✓ N blocks done"; the current block is inverted; ▶ marks the next one.
- **COMING UP** box: anything titled *deadline / due / submission / exam / quiz / midterm / interview / presentation*
  in the next 14 days, counted down in hours when it's within a day.
- **THIS WEEK** shows only anchors (classes = Blueberry color, Work / AV Work, one-off events,
  all-day events, deadlines) so the daily routine doesn't drown it. Days with a deadline get an inverted label.
- Weather note bar only appears when it matters: rain/snow windows during 7a–10p, gusts ≥ 25 mph, feels-like ≤ 32°F.
- **Brief + fun fact** box at the bottom of the agenda, written by Gemini: a one/two-sentence heads-up
  about the day (tomorrow after 9:30 PM) and a fun fact that changes daily and doesn't repeat for 20 days.
  Gemini only runs when the day's calendar/forecast changes (cached), so it's a few calls a day — free tier
  is plenty. No API key, or Gemini down → the box disappears and the agenda gets the room back.
- No WiFi? It redraws from the last good copy with an **Offline** tag and retries in 10 min.
- Press **RST** on the board to force a refresh.
- Spanish accents and GB2312 Chinese render; emoji in titles are stripped.

## 1. Hardware

1. On the driver board set **Display Config = B** (0.47 Ω — the setting for the 7.5" panel) and the
   other switch to **ON** (it must be ON to upload).
2. Open the connector latch, slide the panel's ribbon fully in, close the latch. If the panel never
   changes at all, check the ribbon orientation against the photo on Waveshare's wiki
   (e-Paper ESP32 Driver Board page).
3. Power from any USB 5 V supply via micro-USB.

## 2. Backend (Google Apps Script) — 5 minutes

1. Go to script.google.com while signed in as **jihbin.luo@gmail.com** → *New project*.
2. Paste `backend/Code.gs` into `Code.gs`.
3. *Project Settings* → tick **Show "appsscript.json"** → replace its contents with `backend/appsscript.json`.
4. In `Code.gs`, change `TOKEN` to a long random string (e.g. 32 random letters/digits).
5. Run `testPayload` once from the editor → approve the permissions (read-only calendar + external
   requests) → check the log shows your events.
6. *Deploy → New deployment → Web app*: **Execute as: Me**, **Who has access: Anyone** → Deploy.
   Copy the `/exec` URL.
7. Test in a browser: `<exec URL>?token=<your token>` should return JSON starting with `{"ok":true`.

8. **Gemini (optional):** get a free API key at aistudio.google.com → *Get API key*. In Apps Script,
   *Project Settings → Script properties → Add*: `GEMINI_API_KEY` = your key. Run `testAi` from the
   editor to check the log shows a brief and a fact. (The key stays in Google; it's never on the board.)

When you edit `Code.gs` later: *Deploy → Manage deployments → ✏️ → Version: New version*, so the
URL stays the same.

## 3. Firmware (Arduino IDE 2.x)

1. Boards Manager: install **esp32 by Espressif Systems** (3.3.x).
2. Library Manager: install **GxEPD2**, **Adafruit GFX Library** (say yes to Adafruit BusIO),
   **U8g2_for_Adafruit_GFX**, **ArduinoJson** (7.x).
3. Open `firmware/eink_dashboard/eink_dashboard.ino` and edit `config.h`:
   WiFi name/password and `DASH_URL` = the exec URL + `?token=...`.
4. *Tools*: Board **ESP32 Dev Module**, Partition Scheme **Huge APP (3MB No OTA/1MB SPIFFS)**
   (the sketch is ~1.7 MB because of the Chinese font).
5. Upload. Open Serial Monitor at 115200 to watch `[wifi] [ntp] [http] [sleep]` lines.

Compiled clean against esp32 core 3.3.12, GxEPD2 1.6.9, ArduinoJson 7.4.2:
1.73 MB flash, 123 KB RAM.

## Customizing

| What | Where |
|---|---|
| Refresh rate, quiet hours, 9:30 PM flip, 24 h clock | `config.h` |
| Panel driver (if the image is faint/blotchy) | `config.h` → `GxEPD2_750_GDEY075T7` |
| eduroam-style WiFi (username + password) | `config.h` → `WIFI_ENTERPRISE 1` |
| Which calendars, °C, location, deadline keywords, week-view anchor colors | `Code.gs` → `CONFIG` |
| Gemini model, how many past facts to avoid repeating | `Code.gs` → `CONFIG.AI_MODEL`, `AI_FACT_HISTORY` |
| Tone/length of the brief and what kind of facts | `Code.gs` → prompt in `buildAi_` |
| Drop the Chinese font (sketch then fits the default partition) | add `#define DASH_CJK 0` at the top of `render.cpp` |

**Going home to Costa Rica for break:** in `Code.gs` set `TZ: 'America/Costa_Rica'`,
`LAT: 9.93, LON: -84.09`, `METRIC: true` (if you want °C); in `config.h` set
`TZ_POSIX "CST6"` (no DST). Redeploy + re-upload.

## Troubleshooting

| Screen / serial says | Fix |
|---|---|
| "Can't connect to WiFi" | ESP32 is 2.4 GHz only. Check SSID/password. Campus networks may need `WIFI_ENTERPRISE` or device registration. |
| "Can't load the calendar — bad token" | `TOKEN` in Code.gs ≠ `token=` in `DASH_URL`. |
| HTTP 401/403, or an HTML page | Deployment access isn't **Anyone**, or you're using a `/dev` URL instead of `/exec`. |
| `connection refused` / `-1` | Usually transient; it retries in 10 min. If it persists, re-check the URL. |
| Image faint, blotchy, partly drawn | Flip the Display Config switch, or try the `GxEPD2_750_GDEY075T7` driver. |
| Times off by an hour | `TZ_POSIX` in config.h doesn't match `TZ` in Code.gs. |

## Files

```
backend/Code.gs, appsscript.json      Apps Script web app
firmware/eink_dashboard/
  eink_dashboard.ino                  WiFi, fetch, cache, deep sleep
  config.h                            your settings
  model.h/.cpp                        JSON → structs (ArduinoJson)
  render.h/.cpp                       all drawing (fonts, layout, weather icons)
  schedule.h                          refresh timing
tools/                                PC-side tests: backend under Node with a mocked
                                      Calendar, scheduler unit test, and a pixel-exact
                                      preview renderer (preview/build_and_render.sh)
docs/                                 rendered previews
```
