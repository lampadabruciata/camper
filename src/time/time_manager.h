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

#endif // TIME_MANAGER_H