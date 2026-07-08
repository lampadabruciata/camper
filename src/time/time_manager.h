#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include <TimeLib.h>

// Variabili esterne (definite in main.cpp)
extern bool timeValid;
extern int8_t tzOffsetHours;
extern bool tzIsDST;

// Funzione esterna (definita in main.cpp)
extern time_t nowEpoch();

// Dichiarazione della funzione DST
void updateDST();

// Conversione UTC → locale
time_t utcToLocal(time_t utcEpoch);

#endif // TIME_MANAGER_H
