#pragma once
// Refresh scheduling (pure functions so they can be unit-tested on a PC).
// Times are local seconds-of-day; config.h supplies the knobs.

static const long EARLY_WAKE_TOLERANCE_S = 240;   // the ESP32's RTC drifts; waking up to 4 min early counts

inline bool inQuietWindow(long minuteOfDay, long from, long until) {
  if (from == until) return false;
  if (from < until) return minuteOfDay >= from && minuteOfDay < until;
  return minuteOfDay >= from || minuteOfDay < until;          // window wraps past midnight
}

// If we woke a little before a refresh slot, render as if it were that slot
// (so the 9:30 PM flip and midnight rollover happen on the right refresh).
inline long renderSkewSeconds(long secOfDay, long refreshMin) {
  long slot = refreshMin * 60;
  long toSlot = slot - (secOfDay % slot);
  return toSlot <= EARLY_WAKE_TOLERANCE_S ? toSlot : 0;
}

// Seconds from now until the next refresh: the next :00/:30 slot (or the
// tomorrow-view flip if it comes first), pushed past the quiet window.
inline long secondsToNextWake(long secOfDay, long refreshMin, long flipMin, long quietFrom, long quietUntil) {
  long slot = refreshMin * 60;
  long early = secOfDay + EARLY_WAKE_TOLERANCE_S;
  long next = (early / slot + 1) * slot;
  long flip = flipMin * 60;
  if (flip > early && flip < next) next = flip;
  for (int i = 0; i < 1440 && inQuietWindow((next / 60) % 1440, quietFrom, quietUntil); i++) next += 60;
  long wait = next - secOfDay + 5;                             // land just after the boundary
  return wait < 60 ? 60 : wait;
}
