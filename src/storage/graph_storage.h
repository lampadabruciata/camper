#ifndef GRAPH_STORAGE_H
#define GRAPH_STORAGE_H

#include <Arduino.h>
#include <SD.h>

// Numero campioni grafico 24h
#define NUM_SAMPLES 1440

// Array per i grafici tensione/corrente
extern float voltageSamples[];
extern float currentSamples[];
extern float solarCurrentSamples[];
extern char timeLabels[][6];
extern uint32_t epochMinutes[];
extern int sampleIndex;

// Array orari Ah
extern float ahHourlyToday[24];
extern float ahHourlyYesterday[24];
extern float ahChargeHourlyToday[24];
extern float ahChargeHourlyYesterday[24];
extern float solAhHourlyToday[24];
extern float solAhHourlyYesterday[24];

// Totali giornalieri
extern float ahTodayTotal_global;
extern float ahYesterdayTotal_global;
extern float ahChargeTodayTotal_global;
extern float ahChargeYesterdayTotal_global;
extern float solAhTodayTotal_global;
extern float solAhYesterdayTotal_global;

// Ora corrente
extern int currentHour;

// Mutex SD
extern SemaphoreHandle_t sdMutex;

// Funzione di inizializzazione SD
extern bool initSD();

// Funzione di warning mutex
extern bool takeMutexWarn(SemaphoreHandle_t m, uint32_t timeoutMs, const char *name);

// Header per file grafico (retro-compatibile)
struct GraphHeader { uint32_t magic; uint16_t ver; uint16_t rsv; };
#define GRAPH_MAGIC 0x47524648u /* 'GRFH' */
#define GRAPH_VER   3

// Salva i grafici su SD
void saveGraphToSD();

// Ripristina i grafici da SD
void restoreGraphFromSD();

#endif // GRAPH_STORAGE_H
