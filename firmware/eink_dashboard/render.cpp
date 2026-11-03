#include "render.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

// Set to 0 to drop the Chinese font (~200 KB of flash) if you never need it.
#ifndef DASH_CJK
#define DASH_CJK 1
#endif

namespace {

const uint16_t INK = 0x0000;    // == GxEPD_BLACK
const uint16_t PAPER = 0xFFFF;  // == GxEPD_WHITE

// ---- fonts (U8g2; *_tf variants include Latin-1 so accents render) ----
#define F_DAY      u8g2_font_fub30_tr
#define F_DATE     u8g2_font_fur17_tr
#define F_LABEL    u8g2_font_helvB10_tr
#define F_TIME     u8g2_font_helvB12_tr
#define F_TIME_R   u8g2_font_helvR12_tr
#define F_TITLE_B  u8g2_font_helvB14_tf
#define F_TITLE_R  u8g2_font_helvR14_tf
#define F_MED_B    u8g2_font_helvB12_tf
#define F_MED      u8g2_font_helvR12_tf
#define F_SMALL_B  u8g2_font_helvB10_tf
#define F_SMALL    u8g2_font_helvR10_tf
#define F_TINY     u8g2_font_helvR08_tf
#define F_TINY_B   u8g2_font_helvB08_tf
#define F_BIGTEMP  u8g2_font_fub42_tn
#define F_CJK      u8g2_font_wqy14_t_gb2312
#define F_CJK_S    u8g2_font_wqy12_t_gb2312

// ---- layout ----
const int LCOL_X0 = 18, LCOL_X1 = 466;     // left column (agenda)
const int DIV_X = 482;
const int RCOL_X0 = 498, RCOL_X1 = 786;    // right column (weather + week)

Adafruit_GFX* G;
U8G2_FOR_ADAFRUIT_GFX* U;
bool H24 = false;

/* ------------------------------ text ------------------------------ */

void font(const uint8_t* f) { U->setFont(f); }
int tw(const char* s) { return U->getUTF8Width(s); }

void txt(int x, int y, const char* s, uint16_t c = INK) {
  U->setForegroundColor(c);
  U->setBackgroundColor(c == INK ? PAPER : INK);
  U->drawUTF8(x, y, s);
}
void txtR(int xr, int y, const char* s, uint16_t c = INK) { txt(xr - tw(s), y, s, c); }
void txtC(int xc, int y, const char* s, uint16_t c = INK) { txt(xc - tw(s) / 2, y, s, c); }

bool hasCJK(const char* s) {
  for (; *s; s++)
    if ((uint8_t)*s >= 0xE0) return true;   // 3-byte UTF-8 => CJK after server cleaning
  return false;
}

// Truncate `s` to fit maxW pixels in the current font, ending in "...".
const char* fit(const char* s, int maxW, char* buf, size_t cap) {
  if (tw(s) <= maxW) return s;
  uint16_t starts[200];
  int nb = 0;
  for (int i = 0; s[i] && nb < 200 && (size_t)i + 4 < cap; i++)
    if (((uint8_t)s[i] & 0xC0) != 0x80) starts[nb++] = (uint16_t)i;
  auto build = [&](int k) {
    size_t m = starts[k];
    while (m > 0 && s[m - 1] == ' ') m--;
    memcpy(buf, s, m);
    memcpy(buf + m, "...", 4);
  };
  int lo = 0, hi = nb - 1;          // keep `lo` characters (binary search)
  while (lo < hi) {
    int mid = (lo + hi + 1) / 2;
    build(mid);
    if (tw(buf) <= maxW) lo = mid; else hi = mid - 1;
  }
  build(lo);
  return buf;
}

// "NVIDIA Hackathon: SUBMISSION DEADLINE" -> "NVIDIA Hackathon" (for tight spots)
void keyShort(const char* in, char* out, size_t cap) {
  static const char* cut[] = {"submission", "deadline", " due"};
  size_t n = strlen(in), end = n;
  for (const char* w : cut) {
    size_t wl = strlen(w);
    for (size_t i = 0; i + wl <= n; i++) {
      size_t j = 0;
      while (j < wl && (in[i + j] | 0x20) == (w[j] | 0x20)) j++;
      if (j == wl && i < end) { end = i; break; }
    }
  }
  while (end > 0 && strchr(" :-|,", in[end - 1])) end--;
  if (end == 0) end = n;                       // nothing left -> keep original
  if (end >= cap) end = cap - 1;
  while (end > 0 && ((uint8_t)in[end] & 0xC0) == 0x80) end--;
  memcpy(out, in, end);
  out[end] = 0;
}

void titleFont(const char* s, bool bold) {
#if DASH_CJK
  if (hasCJK(s)) { font(F_CJK); return; }
#endif
  font(bold ? F_TITLE_B : F_TITLE_R);
}

void smallFont(const char* s, bool bold = false) {
#if DASH_CJK
  if (hasCJK(s)) { font(F_CJK_S); return; }
#endif
  font(bold ? F_SMALL_B : F_SMALL);
}

void fmtTime(int m, char* b, size_t n, bool compact) {
  m = ((m % 1440) + 1440) % 1440;
  int h = m / 60, mm = m % 60;
  if (H24) { snprintf(b, n, "%d:%02d", h, mm); return; }
  int h12 = (h % 12 == 0) ? 12 : h % 12;
  char ap = h < 12 ? 'a' : 'p';
  if (compact && mm == 0) snprintf(b, n, "%d%c", h12, ap);
  else snprintf(b, n, "%d:%02d%c", h12, mm, ap);
}

void fmtDur(int mins, char* b, size_t n) {
  if (mins < 60) snprintf(b, n, "%dm", mins);
  else if (mins % 60 == 0) snprintf(b, n, "%dh", mins / 60);
  else snprintf(b, n, "%dh%02d", mins / 60, mins % 60);
}

int hhmmToMin(const char* s) {
  int h = 0, m = 0;
  if (sscanf(s, "%d:%d", &h, &m) != 2) return -1;
  return h * 60 + m;
}

// inverted label like [TOMORROW]; returns width
int tag(int x, int baseline, const char* s, const uint8_t* f = F_LABEL) {
  font(f);
  int w = tw(s) + 10;
  int a = U->getFontAscent();
  G->fillRoundRect(x, baseline - a - 4, w, a + 8, 3, INK);
  txt(x + 5, baseline, s, PAPER);
  return w;
}

void dottedH(int x0, int x1, int y) {
  for (int x = x0; x <= x1; x += 3) G->drawPixel(x, y, INK);
}

void thickLine(int x0, int y0, int x1, int y1, int t, uint16_t c = INK) {
  for (int dx = 0; dx < t; dx++)
    for (int dy = 0; dy < t; dy++) G->drawLine(x0 + dx, y0 + dy, x1 + dx, y1 + dy, c);
}

/* ---------------------------- weather ---------------------------- */

enum Wx { WX_CLEAR, WX_PARTLY, WX_CLOUDY, WX_FOG, WX_DRIZZLE, WX_RAIN, WX_FREEZE, WX_SNOW, WX_STORM };

Wx wxCat(int c) {
  if (c == 0) return WX_CLEAR;
  if (c == 1 || c == 2) return WX_PARTLY;
  if (c == 3) return WX_CLOUDY;
  if (c == 45 || c == 48) return WX_FOG;
  if (c >= 51 && c <= 55) return WX_DRIZZLE;
  if (c == 56 || c == 57 || c == 66 || c == 67) return WX_FREEZE;
  if ((c >= 61 && c <= 65) || (c >= 80 && c <= 82)) return WX_RAIN;
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return WX_SNOW;
  if (c >= 95) return WX_STORM;
  return WX_CLOUDY;
}

const char* wxName(int c, bool day) {
  switch (c) {
    case 0: return day ? "Sunny" : "Clear";
    case 1: return day ? "Mostly sunny" : "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorms";
    case 96: case 99: return "Storms + hail";
  }
  return "";
}

void sun(int cx, int cy, int r) {
  G->fillCircle(cx, cy, r * 42 / 100, INK);
  int t = r >= 20 ? 3 : (r >= 10 ? 2 : 1);
  for (int k = 0; k < 8; k++) {
    float a = k * 0.785398f;
    int x0 = cx + (int)(cosf(a) * r * 0.62f), y0 = cy + (int)(sinf(a) * r * 0.62f);
    int x1 = cx + (int)(cosf(a) * r * 0.95f), y1 = cy + (int)(sinf(a) * r * 0.95f);
    thickLine(x0 - t / 2, y0 - t / 2, x1 - t / 2, y1 - t / 2, t);
  }
}

void moon(int cx, int cy, int r) {
  G->fillCircle(cx, cy, r * 55 / 100, INK);
  G->fillCircle(cx + r * 30 / 100, cy - r * 25 / 100, r * 46 / 100, PAPER);
}

void cloudShape(int x, int y, int w, int inset, uint16_t c) {
  int h = w * 62 / 100;
  G->fillCircle(x + w * 30 / 100, y + h * 58 / 100, w * 20 / 100 - inset, c);
  G->fillCircle(x + w * 56 / 100, y + h * 42 / 100, w * 26 / 100 - inset, c);
  G->fillCircle(x + w * 78 / 100, y + h * 64 / 100, w * 17 / 100 - inset, c);
  int left = x + w * 10 / 100 + inset, right = x + w * 95 / 100 - inset;
  int top = y + h * 55 / 100, bottom = y + h * 98 / 100 - inset;
  int rad = h * 22 / 100 - inset;
  if (rad < 1) rad = 1;
  G->fillRoundRect(left, top, right - left, bottom - top, rad, c);
}

void cloud(int x, int y, int w) {
  int t = w >= 48 ? 3 : 2;
  cloudShape(x, y, w, 0, INK);
  cloudShape(x, y, w, t, PAPER);
}

void wxIcon(int cx, int cy, int S, int code, bool day) {
  Wx k = wxCat(code);
  int t = S >= 48 ? 3 : (S >= 24 ? 2 : 1);
  switch (k) {
    case WX_CLEAR:
      if (day) sun(cx, cy, S / 2); else moon(cx, cy, S / 2);
      return;
    case WX_PARTLY:
      if (day) sun(cx - S * 18 / 100, cy - S * 16 / 100, S * 34 / 100);
      else moon(cx - S * 14 / 100, cy - S * 14 / 100, S * 36 / 100);
      cloud(cx - S * 40 / 100, cy - S * 6 / 100, S * 88 / 100);
      return;
    case WX_CLOUDY:
      cloud(cx - S / 2, cy - S * 31 / 100, S);
      return;
    case WX_FOG:
      for (int i = 0; i < 4; i++) {
        int y = cy - S * 30 / 100 + i * S * 20 / 100;
        int inset = (i % 2) ? S * 12 / 100 : 0;
        G->fillRoundRect(cx - S / 2 + inset, y, S - inset - S * 6 / 100, t + 1, 1, INK);
      }
      return;
    default: break;
  }
  // precipitation types: cloud on top, marks below
  cloud(cx - S / 2, cy - S / 2, S);
  int y0 = cy + S * 20 / 100, y1 = cy + S * 47 / 100;
  int xs[3] = {cx - S * 24 / 100, cx, cx + S * 24 / 100};
  int dot = S >= 40 ? 3 : (S >= 24 ? 2 : 1);
  for (int i = 0; i < 3; i++) {
    int x = xs[i];
    switch (k) {
      case WX_RAIN:
        thickLine(x + S * 5 / 100, y0, x - S * 5 / 100, y1, t);
        break;
      case WX_DRIZZLE:
        G->fillCircle(x, y0 + (i % 2) * S * 12 / 100 + 2, dot, INK);
        break;
      case WX_SNOW: {
        int r = S * 9 / 100 + 1, yc = (y0 + y1) / 2 + ((i % 2) ? 2 : -2);
        thickLine(x - r, yc, x + r, yc, t > 1 ? 2 : 1);
        thickLine(x - r * 6 / 10, yc - r * 8 / 10, x + r * 6 / 10, yc + r * 8 / 10, 1);
        thickLine(x - r * 6 / 10, yc + r * 8 / 10, x + r * 6 / 10, yc - r * 8 / 10, 1);
        break;
      }
      case WX_FREEZE:
        if (i == 1) G->fillCircle(x, (y0 + y1) / 2, dot + 1, INK);
        else thickLine(x + S * 5 / 100, y0, x - S * 5 / 100, y1, t);
        break;
      default: break;
    }
  }
  if (k == WX_STORM) {
    G->fillTriangle(cx + S * 10 / 100, cy + S * 10 / 100, cx - S * 14 / 100, cy + S * 36 / 100,
                    cx + S * 4 / 100, cy + S * 33 / 100, INK);
    G->fillTriangle(cx - S * 5 / 100, cy + S * 30 / 100, cx + S * 13 / 100, cy + S * 28 / 100,
                    cx - S * 10 / 100, cy + S * 56 / 100, INK);
  }
}

void degree(int x, int y, int r) {
  G->drawCircle(x, y, r, INK);
  G->drawCircle(x, y, r - 1, INK);
  if (r > 4) G->drawCircle(x, y, r - 2, INK);
}

/* ---------------------------- sections ---------------------------- */

void drawKeysBox(const Dashboard& d, const NowInfo& now, int x0, int y0, int x1, int y1) {
  const KeyItem* list[MAX_KEYS];
  int n = 0;
  for (int i = 0; i < d.nKeys; i++) {
    const KeyItem& k = d.keys[i];
    long away = (long)(k.dayNum - now.dayNum) * 1440 + (k.m >= 0 ? k.m : 0) - now.minutes;
    if (k.dayNum < now.dayNum) continue;
    if (k.m >= 0 && away < -30) continue;           // passed (30 min grace)
    list[n++] = &k;
  }
  if (!n) return;

  int rows = n < 2 ? n : 2;
  y1 = y0 + 22 + rows * 22 + 2;                    // shrink to fit 1 or 2 rows
  G->fillRoundRect(x0, y0, x1 - x0, y1 - y0, 6, INK);
  font(F_TINY_B);
  txt(x0 + 10, y0 + 13, "COMING UP", PAPER);
  if (n > 2) {
    char more[12];
    snprintf(more, sizeof(more), "+%d", n - 2);
    txtR(x1 - 10, y0 + 13, more, PAPER);
  }
  for (int i = 0; i < n && i < 2; i++) {
    const KeyItem& k = *list[i];
    int base = y0 + 34 + i * 22;
    long days = k.dayNum - now.dayNum;
    long away = days * 1440 + (k.m >= 0 ? k.m : 0) - now.minutes;
    char when[16];
    if (k.m >= 0 && away < 0) snprintf(when, sizeof(when), "NOW");
    else if (k.m >= 0 && away < 60) snprintf(when, sizeof(when), "%ldm", away);
    else if (k.m >= 0 && away < 24 * 60) snprintf(when, sizeof(when), "%ldh", (away + 30) / 60);
    else if (days == 0) snprintf(when, sizeof(when), "Today");
    else if (days == 1) snprintf(when, sizeof(when), "Tmrw");
    else snprintf(when, sizeof(when), "%ldd", days);
    font(F_MED_B);
    txt(x0 + 10, base, when, PAPER);
    char buf[96], st[72];
    keyShort(k.title, st, sizeof(st));
    smallFont(st, false);
    if (!hasCJK(st)) font(F_MED);
    txt(x0 + 58, base, fit(st, x1 - x0 - 68, buf, sizeof(buf)), PAPER);
  }
}

void drawHeader(const Dashboard& d, const Day& day, bool tomorrow, const NowInfo& now) {
  font(F_DAY);
  txt(LCOL_X0 - 2, 46, day.dowLong);
  int wDay = tw(day.dowLong);
  font(F_DATE);
  txt(LCOL_X0, 76, day.monthDay);
  int wDate = tw(day.monthDay);
  if (tomorrow) wDate += 12 + tag(LCOL_X0 + wDate + 12, 74, "TOMORROW");
  // the countdown box takes whatever room the date leaves
  int bx = LCOL_X0 + (wDay > wDate ? wDay : wDate) + 18;
  if (bx < LCOL_X0 + 170) bx = LCOL_X0 + 170;
  if (bx > 300) bx = 300;
  drawKeysBox(d, now, bx, 10, LCOL_X1, 84);
}

void drawCheck(int x, int y) {        // small check mark, (x,y) = bottom of the V
  thickLine(x - 5, y - 5, x, y, 2);
  thickLine(x, y, x + 8, y - 10, 2);
}

void drawAgenda(const Day& day, int nowMin, bool tomorrow, int x0, int y0, int x1, int y1) {
  char buf[96], tb[16];
  font(F_LABEL);
  txt(x0, y0 + 12, tomorrow ? "TOMORROW" : "TODAY");
  int remaining = 0;
  for (int i = 0; i < day.nEvents; i++)
    if (nowMin < 0 || day.events[i].e > nowMin) remaining++;
  if (day.nEvents) {
    if (nowMin >= 0) snprintf(buf, sizeof(buf), "%d of %d left", remaining, day.nEvents);
    else snprintf(buf, sizeof(buf), "%d blocks", day.nEvents);
    font(F_SMALL);
    txtR(x1, y0 + 12, buf);
  }
  G->drawFastHLine(x0, y0 + 18, x1 - x0, INK);
  int y = y0 + 26;

  // all-day chips
  if (day.nAllDay) {
    int x = x0;
    int chipH = 22;
    for (int i = 0; i < day.nAllDay; i++) {
      const AllDay& a = day.allDay[i];
      smallFont(a.title, true);
      const char* s = fit(a.title, x1 - x0 - 12, buf, sizeof(buf));
      int w = tw(s) + 14;
      if (x > x0 && x + w > x1) { x = x0; y += chipH + 4; }
      if (y + chipH > y1) break;
      bool key = a.kind == KIND_KEY;
      if (key) G->fillRoundRect(x, y, w, chipH, 5, INK);
      else G->drawRoundRect(x, y, w, chipH, 5, INK);
      txt(x + 7, y + 16, s, key ? PAPER : INK);
      x += w + 6;
    }
    y += chipH + 6;
  }

  int n = day.nEvents;
  if (n == 0) {
    font(F_MED);
    txt(x0 + 74, y + 26, day.nAllDay ? "No timed events." : "Nothing on the calendar.");
    return;
  }

  // decide what fits: finished blocks collapse into one "done" line,
  // anything that still doesn't fit becomes a "+N more" line at the bottom
  const int MIN_ROW = 25, MAX_ROW = 34;
  int avail = y1 - y;
  int maxRows = avail / MIN_ROW;
  int finished = 0;
  if (nowMin >= 0)
    while (finished < n && day.events[finished].e <= nowMin) finished++;
  int first = 0, last = n;
  bool topSum = false, botSum = false;
  if (finished >= 2 || (finished == 1 && n > maxRows)) { first = finished; topSum = true; }
  int rowsForEvents = maxRows - (topSum ? 1 : 0);
  if (last - first > rowsForEvents) { botSum = true; last = first + rowsForEvents - 1; }
  int shown = (last - first) + (topSum ? 1 : 0) + (botSum ? 1 : 0) + (first == n ? 1 : 0);
  int rh = avail / shown;
  if (rh > MAX_ROW) rh = MAX_ROW;
  if (rh < MIN_ROW) rh = MIN_ROW;

  int nextIdx = -1;
  if (nowMin >= 0)
    for (int i = 0; i < n; i++)
      if (day.events[i].s > nowMin) { nextIdx = i; break; }

  font(F_TITLE_B);
  int asc = U->getFontAscent();
  const int timeR = x0 + 62, titleX = x0 + 72;

  if (topSum) {
    int base = y + (rh + asc) / 2 - 1;
    drawCheck(timeR - 12, base - 1);
    font(F_SMALL);
    snprintf(buf, sizeof(buf), "%d block%s done", first, first == 1 ? "" : "s");
    txt(titleX, base - 1, buf);
    y += rh;
  }
  if (first == n) {
    font(F_MED_B);
    txt(titleX, y + (rh + asc) / 2 - 1, "All done for today.");
  }

  for (int i = first; i < last; i++) {
    const Event& e = day.events[i];
    bool past = nowMin >= 0 && e.e <= nowMin && !(e.s == e.e && e.s >= nowMin);
    bool cur = nowMin >= 0 && e.s <= nowMin && nowMin < e.e;
    bool key = e.kind == KIND_KEY;
    uint16_t fg = cur ? PAPER : INK;
    int base = y + (rh + asc) / 2 - 1;

    if (cur) G->fillRoundRect(x0 - 8, y + 1, x1 - x0 + 12, rh - 2, 5, INK);
    if (i == nextIdx && !cur)
      G->fillTriangle(x0 - 14, base - asc + 2, x0 - 14, base, x0 - 7, base - asc / 2 + 1, INK);

    // time column
    if (e.flags & 1) snprintf(tb, sizeof(tb), "...");
    else fmtTime(e.s, tb, sizeof(tb), false);
    font(past ? F_TIME_R : F_TIME);
    txtR(timeR, base, tb, fg);

    // right column
    int rightEdge = x1;
    font(F_SMALL);
    const char* right = nullptr;
    char rb[48];
    if (cur) {
      fmtTime(e.e, tb, sizeof(tb), true);
      snprintf(rb, sizeof(rb), "until %s", tb);
      right = rb;
    } else if (e.loc[0] && !past) {
      right = fit(e.loc, 110, rb, sizeof(rb));
    } else if (!past && e.e > e.s) {
      fmtDur(e.e - e.s, rb, sizeof(rb));
      right = rb;
    }
    if (key && !past) {
      int r = 9, cx = rightEdge - r, cy = base - asc / 2;
      G->fillCircle(cx, cy, r, cur ? PAPER : INK);
      font(F_LABEL);
      txtC(cx + 1, cy + 5, "!", cur ? INK : PAPER);
      rightEdge -= 2 * r + 6;
      font(F_SMALL);
    }
    if (right) {
      smallFont(right);
      txtR(rightEdge, base, right, fg);
      rightEdge -= tw(right) + 10;
    }

    // title
    titleFont(e.title, !past);
    const char* s = fit(e.title, rightEdge - titleX, buf, sizeof(buf));
    txt(titleX, base, s, fg);
    if (past) {
      int sy = base - asc * 4 / 10;
      G->drawFastHLine(titleX - 2, sy, tw(s) + 4, INK);
    }
    y += rh;
  }

  if (botSum) {
    int hidden = n - last;
    int endMin = 0;
    for (int i = last; i < n; i++) if (day.events[i].e > endMin) endMin = day.events[i].e;
    fmtTime(endMin, tb, sizeof(tb), true);
    int base = y + (rh + asc) / 2 - 1;
    font(F_SMALL);
    snprintf(buf, sizeof(buf), "+%d more, until %s", hidden, tb);
    txt(titleX, base - 1, buf);
  }
}

void drawWeather(const Dashboard& d, const Day& mainDay, bool tomorrow, int x0, int y0, int x1) {
  const DayWeather& w = mainDay.wx;
  char b[96], t1[16], t2[16];
  int code, big;
  bool day = true;
  if (!tomorrow && d.cur.valid) { code = d.cur.code; big = d.cur.t; day = d.cur.isDay; }
  else if (w.valid) { code = w.code; big = w.hi; }
  else {
    font(F_MED);
    txt(x0, y0 + 30, "Weather unavailable");
    return;
  }

  wxIcon(x0 + 36, y0 + 38, 68, code, day);

  font(F_BIGTEMP);
  snprintf(b, sizeof(b), "%d", big);
  int tx = x0 + 82;
  txt(tx, y0 + 62, b);
  int tend = tx + tw(b);
  degree(tend + 7, y0 + 25, 5);

  int cx = tend + 22;
  font(F_MED_B);
  char cb[40];
  txt(cx, y0 + 36, fit(wxName(code, day), x1 - cx, cb, sizeof(cb)));
  font(F_SMALL);
  if (tomorrow) snprintf(b, sizeof(b), "Tomorrow's high");
  else snprintf(b, sizeof(b), "Feels like %d°", d.cur.feels);
  txt(cx, y0 + 56, fit(b, x1 - cx, cb, sizeof(cb)));

  // detail lines
  font(F_MED);
  int y = y0 + 88;
  if (w.valid) {
    if (tomorrow) snprintf(b, sizeof(b), "Low %d°  ·  Precip %d%%", w.lo, w.pop);
    else if (d.cur.valid) snprintf(b, sizeof(b), "H %d°  L %d°  ·  Wind %d %s", w.hi, w.lo, d.cur.wind, d.unitW);
    else snprintf(b, sizeof(b), "H %d°  L %d°", w.hi, w.lo);
    txt(x0, y, fit(b, x1 - x0, cb, sizeof(cb)));
  }
  y += 23;
  if (w.valid && w.note[0]) {
    font(F_SMALL_B);
    G->fillRoundRect(x0 - 4, y - 15, x1 - x0 + 8, 21, 5, INK);
    txt(x0 + 4, y, fit(w.note, x1 - x0 - 8, cb, sizeof(cb)), PAPER);
  } else if (w.valid) {
    font(F_SMALL);
    txt(x0, y, w.pop >= 30 ? "Some chance of rain" : "No rain expected");
  }
  y += 21;
  if (w.valid && w.sunrise[0] && w.sunset[0]) {
    fmtTime(hhmmToMin(w.sunrise), t1, sizeof(t1), false);
    fmtTime(hhmmToMin(w.sunset), t2, sizeof(t2), false);
    snprintf(b, sizeof(b), "Sunrise %s  ·  Sunset %s", t1, t2);
    font(F_SMALL);
    txt(x0, y, b);
  }
}

void drawHourly(const Dashboard& d, int todayIdx, bool tomorrow, const NowInfo& now, int x0, int y0, int x1) {
  const int SLOTS = 5;
  int colW = (x1 - x0) / SLOTS;
  char b[16];
  for (int k = 0; k < SLOTS; k++) {
    int H = tomorrow ? 24 + 7 + 3 * k : now.hour + 1 + 3 * k;   // hours from today's midnight
    int di = todayIdx + H / 24, hr = H % 24;
    if (di >= d.nDays || !d.days[di].hasHourly) continue;
    const Day& day = d.days[di];
    const HourWeather& hw = day.hourly[hr];
    int cx = x0 + colW * k + colW / 2;
    int sr = day.wx.valid ? hhmmToMin(day.wx.sunrise) : 7 * 60;
    int ss = day.wx.valid ? hhmmToMin(day.wx.sunset) : 18 * 60;
    bool isDay = hr * 60 + 30 >= sr && hr * 60 + 30 < ss;

    fmtTime(hr * 60, b, sizeof(b), true);
    font(F_SMALL);
    txtC(cx, y0 + 12, b);
    wxIcon(cx, y0 + 30, 24, hw.code, isDay);
    font(F_MED_B);
    snprintf(b, sizeof(b), "%d°", hw.t);
    txtC(cx + 2, y0 + 58, b);
    if (hw.pop >= 20) {
      font(F_TINY);
      snprintf(b, sizeof(b), "%d%%", hw.pop);
      txtC(cx, y0 + 69, b);
    }
  }
}

void drawWeek(const Dashboard& d, int fromIdx, int x0, int y0, int x1, int y1) {
  font(F_LABEL);
  txt(x0, y0 + 12, "THIS WEEK");
  G->drawFastHLine(x0, y0 + 18, x1 - x0, INK);
  const int ROWS = 6;
  int top = y0 + 22;
  int rh = (y1 - top) / ROWS;
  char line[200], buf[96], tb[16], b[32], st[72];

  for (int r = 0; r < ROWS; r++) {
    int di = fromIdx + r;
    if (di >= d.nDays) break;
    const Day& day = d.days[di];
    int yt = top + r * rh;
    bool hasKey = false;
    for (int i = 0; i < day.nAllDay; i++) if (day.allDay[i].kind == KIND_KEY) hasKey = true;
    for (int i = 0; i < day.nEvents; i++) if (day.events[i].kind == KIND_KEY) hasKey = true;

    // line 1: day label (inverted when something important is due) + weather
    const char* dnum = strchr(day.md, ' ');
    snprintf(b, sizeof(b), "%s %s", day.dow, dnum ? dnum + 1 : "");
    if (hasKey) tag(x0, yt + 14, b, F_MED_B);
    else { font(F_MED_B); txt(x0, yt + 14, b); }

    if (day.wx.valid) {
      font(F_SMALL_B);
      snprintf(b, sizeof(b), "%d°/%d°", day.wx.hi, day.wx.lo);
      txtR(x1, yt + 13, b);
      int ix = x1 - tw(b) - 14;
      wxIcon(ix, yt + 8, 18, day.wx.code, true);
      if (day.wx.pop >= 30) {
        font(F_TINY);
        snprintf(b, sizeof(b), "%d%%", day.wx.pop);
        txtR(ix - 13, yt + 13, b);
      }
    }

    // line 2: anchors (one-offs, classes, work, deadlines)
    line[0] = 0;
    size_t L = 0;
    auto add = [&](const char* s) {
      if (L && L + 4 < sizeof(line)) { memcpy(line + L, " · ", 4); L += 4; line[L] = 0; }
      size_t n = strlen(s);
      if (L + n + 1 > sizeof(line)) n = sizeof(line) - L - 1;
      memcpy(line + L, s, n);
      L += n;
      line[L] = 0;
    };
    for (int pass = 0; pass < 2; pass++) {      // key items first, then the rest
      for (int i = 0; i < day.nAllDay; i++) {
        const AllDay& a = day.allDay[i];
        if ((pass == 0) != (a.kind == KIND_KEY)) continue;
        if (a.kind == KIND_KEY) { keyShort(a.title, st, sizeof(st)); snprintf(buf, sizeof(buf), "! %s due", st); }
        else snprintf(buf, sizeof(buf), "%s", a.title);
        add(buf);
      }
      for (int i = 0; i < day.nEvents; i++) {
        const Event& e = day.events[i];
        if (e.kind == KIND_ROUTINE || (pass == 0) != (e.kind == KIND_KEY)) continue;
        fmtTime(e.s, tb, sizeof(tb), true);
        if (e.kind == KIND_KEY) { keyShort(e.title, st, sizeof(st)); snprintf(buf, sizeof(buf), "! %s %s", st, tb); }
        else snprintf(buf, sizeof(buf), "%s %s", e.title, tb);
        add(buf);
      }
    }
    if (!L) snprintf(line, sizeof(line), day.nEvents ? "Routine day · %d blocks" : "Free day", day.nEvents);
    smallFont(line);
    txt(x0, yt + 29, fit(line, x1 - x0, buf, sizeof(buf)));
  }
}

}  // namespace

/* ------------------------------ public ------------------------------ */

void renderMessage(Adafruit_GFX& gfx, U8G2_FOR_ADAFRUIT_GFX& u8, const char* title,
                   const char* detail, const char* hint) {
  G = &gfx; U = &u8;
  U->setFontMode(1);
  U->setFontDirection(0);
  G->fillScreen(PAPER);
  char buf[160];
  font(F_DATE);
  txtC(400, 200, title);
  font(F_MED);
  if (detail && detail[0]) txtC(400, 236, fit(detail, 700, buf, sizeof(buf)));
  font(F_SMALL);
  if (hint && hint[0]) txtC(400, 266, fit(hint, 700, buf, sizeof(buf)));
}

void renderDashboard(Adafruit_GFX& gfx, U8G2_FOR_ADAFRUIT_GFX& u8, const Dashboard& d,
                     const NowInfo& now, const RenderOptions& opt) {
  G = &gfx; U = &u8;
  H24 = opt.h24;
  U->setFontMode(1);
  U->setFontDirection(0);
  G->fillScreen(PAPER);

  int today = -1;
  for (int i = 0; i < d.nDays; i++)
    if (d.days[i].dayNum == now.dayNum) { today = i; break; }
  if (today < 0) {
    renderMessage(gfx, u8, "Calendar data is out of date",
                  opt.status, "Check WiFi, or press RST on the board to retry.");
    return;
  }
  bool tomorrow = now.minutes >= opt.tomorrowFrom && today + 1 < d.nDays;
  int mainIdx = tomorrow ? today + 1 : today;
  const Day& mainDay = d.days[mainIdx];

  // left: header + agenda
  drawHeader(d, mainDay, tomorrow, now);
  G->drawFastHLine(LCOL_X0 - 2, 92, LCOL_X1 - LCOL_X0 + 2, INK);
  drawAgenda(mainDay, tomorrow ? -1 : now.minutes, tomorrow, LCOL_X0, 100, LCOL_X1, 458);

  // divider
  G->drawFastVLine(DIV_X, 12, 458, INK);

  // right: weather, hourly, week
  drawWeather(d, mainDay, tomorrow, RCOL_X0, 6, RCOL_X1);
  dottedH(RCOL_X0, RCOL_X1, 146);
  drawHourly(d, today, tomorrow, now, RCOL_X0, 150, RCOL_X1);
  dottedH(RCOL_X0, RCOL_X1, 224);
  drawWeek(d, mainIdx + 1, RCOL_X0, 228, RCOL_X1, 478);

  // footer
  if (opt.status[0]) {
    if (opt.stale) {
      font(F_TINY_B);
      int w = tw(opt.status) + 10;
      G->fillRoundRect(LCOL_X0 - 4, 464, w, 14, 3, INK);
      txt(LCOL_X0 + 1, 475, opt.status, PAPER);
    } else {
      font(F_TINY);
      txt(LCOL_X0, 475, opt.status);
    }
  }
}
