#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include <TimeLib.h>

// Variabili esterne (definite in main.cpp)
extern bool timeValid;
extern int8_t tzOffsetHours;
extern bool tzIsDST;
extern time_t rtcBaseEpoch;
extern unsigned long rtcBaseMillis;

// Dichiarazione della funzione DST
void updateDST();

// Conversione UTC → locale
time_t utcToLocal(time_t utcEpoch);

// Conversione locale → UTC
time_t localToUtc(time_t localEpoch);

// Ottieni epoch corrente
time_t nowEpoch();

// Handler per impostazione orario via web
void handleSetTime();
#endif // TIME_MANAGER_H
