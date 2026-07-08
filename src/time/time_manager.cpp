#include "time_manager.h"

// ---- FUSO ORARIO: calcolo DST europeo ----
// Regola UE: ultima domenica di marzo alle 02:00 → 03:00 (DST on)
//            ultima domenica di ottobre alle 03:00 → 02:00 (DST off)
void updateDST() {
  if (!timeValid) return;

  time_t utcNow = nowEpoch();
  tmElements_t tmUtc;
  breakTime(utcNow, tmUtc);

  int year  = tmUtc.Year + 1970;
  int month = tmUtc.Month;
  int day   = tmUtc.Day;
  int hour  = tmUtc.Hour;

  // Ultima domenica di marzo
  int marchLastSun = 31 - ((5 * year / 4 + 4) % 7);
  // Ultima domenica di ottobre
  int octLastSun = 31 - ((5 * year / 4 + 1) % 7);

  bool dst = false;

  if (month > 3 && month < 10) {
    dst = true;  // Aprile-Settembre: sicuramente DST
  } else if (month == 3) {
    if (day > marchLastSun) dst = true;
    else if (day == marchLastSun && hour >= 2) dst = true;
  } else if (month == 10) {
    if (day < octLastSun) dst = true;
    else if (day == octLastSun && hour < 1) dst = true;
  }

  tzIsDST = dst;
  tzOffsetHours = dst ? 2 : 1;
}