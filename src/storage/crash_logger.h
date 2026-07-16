#ifndef CRASH_LOGGER_H
#define CRASH_LOGGER_H

#include <Arduino.h>
#include <SD.h>
#include <RTClib.h>
#include <freertos/semphr.h>

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
extern volatile float tempInt;
extern volatile float tempExt;
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

// Funzione di inizializzazione SD
extern bool initSD();

// Log crash su SD
void logCrash(const char *reason);

// Log boot su SD (registrazione sempre)
void logBoot();

#endif // CRASH_LOGGER_H