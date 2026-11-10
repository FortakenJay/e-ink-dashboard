// g++ -std=c++17 -I../firmware/eink_dashboard test_schedule.cpp && ./a.out
#include <cstdio>
#include <cassert>
#include "schedule.h"
static long hms(int h, int m, int s = 0) { return h * 3600L + m * 60L + s; }
static void show(long t) { printf("%02ld:%02ld:%02ld", (t / 3600) % 24, (t / 60) % 60, t % 60); }
int main() {
  const long R = 30, FLIP = 21 * 60 + 30, QF = 30, QU = 5 * 60 + 30;
  struct { long now, expectWake; const char* why; } cases[] = {
    {hms(19, 44, 10), hms(20, 0, 5), "normal: next :00"},
    {hms(7, 58, 0), hms(8, 30, 5), "woke 2 min early for 8:00 -> treat as 8:00, next 8:30"},
    {hms(21, 10, 0), hms(21, 30, 5), "flip slot"},
    {hms(23, 31, 0), hms(24, 0, 5), "midnight rollover refresh"},
    {hms(23, 57, 0), hms(24 + 5, 30, 5), "early for midnight -> skip quiet to 5:30"},
    {hms(0, 1, 0), hms(5, 30, 5), "after midnight -> 5:30"},
    {hms(5, 31, 0), hms(6, 0, 5), "morning cadence"},
  };
  for (auto& c : cases) {
    long wake = c.now + secondsToNextWake(c.now, R, FLIP, QF, QU);
    printf("now "); show(c.now); printf(" -> wake "); show(wake);
    printf("  skew=%lds  %s %s\n", renderSkewSeconds(c.now, R), wake == c.expectWake ? "OK " : "BAD", c.why);
    assert(wake == c.expectWake);
  }
  assert(renderSkewSeconds(hms(21, 27, 30), R) == 150);   // 9:27:30 renders as 9:30 -> tomorrow view
  assert(renderSkewSeconds(hms(21, 20, 0), R) == 0);
  // wrap-around quiet window 23:30-05:30
  assert(inQuietWindow(23 * 60 + 45, 23 * 60 + 30, 5 * 60 + 30) && !inQuietWindow(12 * 60, 23 * 60 + 30, 5 * 60 + 30));
  // full vs partial refresh
  assert(wantFullRefresh(false, true, 0, 8, 12 * 60, QU, R));        // power-on / RST
  assert(wantFullRefresh(true, false, 0, 8, 12 * 60, QU, R));        // no saved frame
  assert(!wantFullRefresh(true, true, 3, 8, 12 * 60, QU, R));        // normal -> partial
  assert(wantFullRefresh(true, true, 8, 8, 12 * 60, QU, R));         // every 8th
  assert(wantFullRefresh(true, true, 1, 8, 5 * 60 + 30, QU, R));     // first of the morning
  assert(!wantFullRefresh(true, true, 1, 8, 6 * 60, QU, R));
  assert(wantFullRefresh(true, true, 0, 0, 12 * 60, QU, R));         // 0 = always full
  puts("schedule tests passed");
}
