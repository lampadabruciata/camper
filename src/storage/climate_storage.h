#ifndef CLIMATE_STORAGE_H
#define CLIMATE_STORAGE_H

#include <Arduino.h>
#include <SD.h>

// Numero campioni climate 24h (1 campione/minuto per 24h).
// Deve combaciare con climateSamples in main.cpp.
#define CLIMATE_SAMPLES 1440

// Buffer RAM climate (definiti in main.cpp)
extern float climateTempInt[];
extern float climateTempExt[];
extern float climateHumInt[];
extern float climateHumExt[];
extern uint32_t climateEpochMinutes[];
extern int climateSampleIndex;

// Vero solo dopo un restore SD valido (F2)
extern bool climateRestored;

// Mutex SD
extern SemaphoreHandle_t sdMutex;

// Funzione di inizializzazione SD
extern bool initSD();

// Funzione di warning mutex
extern bool takeMutexWarn(SemaphoreHandle_t m, uint32_t timeoutMs, const char *name);

// Header file climate24h.bin (formato approvato)
// Layout: magic 'CLIM' (4), version (2), reserved (2), climateSampleIndex (4)
struct ClimateHeader {
    uint32_t magic;              // 'CLIM'
    uint16_t ver;                // versione formato = 1
    uint16_t rsv;                // riservato (0)
    uint32_t climateSampleIndex; // indice corrente buffer circolare
};
#define CLIMATE_MAGIC 0x4D494C43u  /* 'CLIM' */
#define CLIMATE_VER   1

// Salva il buffer climate su SD in modo atomico (/climate24h.bin)
void saveClimateToSD();

// Ripristina il buffer climate da SD (con pulizia campioni >24h)
void restoreClimateFromSD();

#endif // CLIMATE_STORAGE_H