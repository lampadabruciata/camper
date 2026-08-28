#ifndef CRASH_LOGGER_H
#define CRASH_LOGGER_H

#include <Arduino.h>
#include <SD.h>
#include <RTClib.h>
#include <freertos/semphr.h>
#include <vector>

// Modello dati unificato DHT (accesso a dhtSensors[] nel crash log)
#include "../dht/dht_manager.h"

// Informazioni brevi di un evento crash (para la lista de la página Diagnóstica)
struct CrashEventInfo {
  String timestamp;   // Clock del evento (p.ej. "2026-08-24 00:57")
  String reason;      // Causa breve (p. ej. "RESET: INT_WDT")
  String resetReason; // ResetReason completo (p. ej. "INT_WDT (2)")
};

// Variabili esterne necessarie per logCrash
extern RTC_DS3231 rtc;
extern bool timeValid;
extern int8_t tzOffsetHours;
extern bool tzIsDST;
extern int currentYear;
extern int currentMonth;
extern int currentDay;
extern int currentHour;
extern int currentMinute;
extern volatile int currentPage;
extern volatile bool isDimmed;
extern volatile bool levelExclusiveMode;
extern volatile bool adsPresent;
extern volatile bool lisPresent;
extern bool scale1Ready;
extern bool scale2Ready;
extern volatile float vBattery;
extern volatile float currentFiltered;
extern volatile float solarAmps;
extern float batteryAh_max;
extern float batteryAh_now;
extern float gasResidual1;
extern float gasResidual2;
extern volatile float remainingLiters;
extern volatile float lastFlowLmin;
extern TaskHandle_t SensorTask;
extern TaskHandle_t DisplayTaskHandle;
extern SemaphoreHandle_t sdMutex;

// Heartbeat diagnostici (per individuare task bloccati prima di INT_WDT)
extern volatile uint32_t hbSensorLoop;
extern volatile uint32_t hbLevelTask;
extern volatile uint32_t hbDisplayTask;
extern volatile uint32_t hbLoop;

// Timestamp ultimo ciclo task (ms)
extern volatile unsigned long lastSensorLoopMs;
extern volatile unsigned long lastLevelTaskMs;
extern volatile unsigned long lastDisplayTaskMs;
extern volatile unsigned long lastLoopMs;

// Tempi massimi di esecuzione task (µs)
extern volatile uint32_t sensorLoopMaxUs;
extern volatile uint32_t levelTaskMaxUs;
extern volatile uint32_t displayTaskMaxUs;
extern volatile uint32_t loopMaxUs;

// Tempi massimi di esecuzione blocchi SensorLoop (µs)
extern volatile uint32_t updateCurrentAndVoltageMaxUs;
extern volatile uint32_t readSolarCurrentMaxUs;
extern volatile uint32_t updateDHTMaxUs;
extern volatile uint32_t updateGas1MaxUs;
extern volatile uint32_t updateGas2MaxUs;
extern volatile uint32_t updateLevelMaxUs;
extern volatile uint32_t saveAhStateToSDMaxUs;
extern volatile uint32_t saveAhToSDMaxUs;
extern volatile uint32_t saveGraphToSDMaxUs;

// Diagnostica DHT
extern volatile uint32_t dhtReads;
extern volatile uint32_t dhtReadFailures;
extern volatile uint32_t dhtMaxReadUs;

// Funzione di inizializzazione SD
extern bool initSD();

// Log crash su SD
void logCrash(const char *reason);

// Log boot su SD (registrazione sempre)
void logBoot();

// ----- API diagnostica crash (basadas en crashlog.txt, sin bootCount NVS) -----

// Cuenta cuántos eventos hay en /crash_log.txt (0 si el archivo no existe o vacío)
int countCrashEvents();

// Devuelve todos los eventos del crash log (más reciente primero) en `out`
void readAllCrashEvents(std::vector<CrashEventInfo>& out);

// Devuelve el bloque completo sin procesar del evento `index` (0 = más reciente).
// Cadena vacía si `index` no existe.
String readCrashEventRaw(int index);

#endif // CRASH_LOGGER_H