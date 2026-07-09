#include "time_manager.h"
#include <Arduino.h>
#include <WebServer.h>
#include <RTClib.h>

// WebServer esterno (definito in main.cpp)
extern WebServer server;

// RTC esterno (definito in main.cpp)
extern RTC_DS3231 rtc;

// Variabili esterne per handleSetTime (definite in main.cpp)
extern int currentYear;
extern int currentMonth;
extern int currentDay;
extern int currentHour;
extern int currentMinute;
extern bool timeValid;
extern int8_t tzOffsetHours;
extern bool tzIsDST;
extern time_t rtcBaseEpoch;
extern unsigned long rtcBaseMillis;
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

// Converte epoch UTC in epoch locale (per display e grafici)
time_t utcToLocal(time_t utcEpoch) {
  return utcEpoch + (tzOffsetHours * 3600);
}

// Ottieni epoch corrente (UTC)
time_t nowEpoch() {
  if (!timeValid) return 0;
  return rtcBaseEpoch + (millis() - rtcBaseMillis) / 1000;
}

// Converte epoch locale in UTC
time_t localToUtc(time_t localEpoch) {
  return localEpoch - (tzOffsetHours * 3600);
}

// Handler per impostazione orario via web
// Impostazione orario (HH:MM) via query: /setTime?time=12:34 e DATA
void handleSetTime() {
  if (!server.hasArg("time")) {
    server.send(400, "text/plain", "errore");
    return;
  }

  String t = server.arg("time");

  // FORMAT: YYYY-MM-DD HH:MM
  if (t.length() == 16 &&
      t.charAt(4) == '-' &&
      t.charAt(7) == '-' &&
      t.charAt(10) == ' ') {

    // ✅ 1) PARSING (QUESTO RESTA IDENTICO)
    currentYear   = t.substring(0, 4).toInt();
    currentMonth  = t.substring(5, 7).toInt();
    currentDay    = t.substring(8, 10).toInt();
    currentHour   = t.substring(11, 13).toInt();
    currentMinute = t.substring(14, 16).toInt();

    // ✅ 2) COSTRUISCI EPOCH DAI VALORI GIÀ PARSATI
    tmElements_t tm;
    tm.Year   = currentYear - 1970;
    tm.Month  = currentMonth;
    tm.Day    = currentDay;
    tm.Hour   = currentHour;
    tm.Minute = currentMinute;
    tm.Second = 0;

    // Il telefono manda ORA LOCALE (es. 14:30 a Bergamo)
    // makeTime() produce un epoch come se fosse UTC
    // Quindi dobbiamo sottrarre il fuso orario per ottenere il vero UTC
    time_t localEpoch = makeTime(tm);

    // Prima impostazione: calcola DST dal tempo locale
    // (serve per sapere se è UTC+1 o UTC+2)
    // Usiamo il mese/giorno/ora già parsati per decidere
    bool dstEstimate = false;
    if (currentMonth > 3 && currentMonth < 10) {
      dstEstimate = true;
    } else if (currentMonth == 3 && currentDay >= 25) {
      dstEstimate = true;  // approssimazione: dopo il 25 marzo probabile DST
    } else if (currentMonth == 10 && currentDay < 25) {
      dstEstimate = true;  // approssimazione: prima del 25 ottobre probabile DST
    }
    tzIsDST = dstEstimate;
    tzOffsetHours = dstEstimate ? 2 : 1;

    time_t epoch = localEpoch - (tzOffsetHours * 3600);

    // ✅ 3) ALLINEA RTC + TEMPO BASE (in UTC)
    rtc.adjust(DateTime(epoch));
    rtcBaseEpoch  = epoch;
    rtcBaseMillis = millis();
    timeValid     = true;

    Serial.printf("[RTC] Allineato da telefono: locale=%02d:%02d UTC+%d epoch=%lu\n",
                  currentHour, currentMinute, tzOffsetHours, (unsigned long)epoch);


    // ✅ 4) RISPOSTA AL CLIENT
    server.send(200, "text/plain", "ok");
    return;
  }

  // FORMAT: HH:MM (fallback)
  if (t.length() == 5 && t.charAt(2) == ':') {
    currentHour   = t.substring(0, 2).toInt();
    currentMinute = t.substring(3, 5).toInt();

    server.send(200, "text/plain", "ok");
    return;
  }

  server.send(400, "text/plain", "formato non valido");
}
