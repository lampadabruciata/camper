#include "water_manager.h"
#include <SD.h>
#include <Arduino.h>
#include <freertos/semphr.h>

// Dichiarazioni esterne (definite in main.cpp)
extern SemaphoreHandle_t sdMutex;
extern bool initSD();
extern bool takeMutexWarn(SemaphoreHandle_t m, uint32_t timeoutMs, const char *name);
extern void debugSDWriteResult(const char* file, bool ok, size_t written, uint32_t microsTime);

// ============================================================
// SALVATAGGIO LITRI ACQUA SU SD
// ============================================================
void saveLastLitersToSD(float liters) {
  if (!initSD()) return;

  if (!takeMutexWarn(sdMutex, 500, "sd"))
    return;

  const char* tempname = "/.tmp_liters.txt";
  File tmp = SD.open(tempname, FILE_WRITE);
  if (!tmp) {
    xSemaphoreGive(sdMutex);
    return;
  }

  tmp.seek(0);
  uint32_t t0 = micros();
  int w = tmp.print(liters, 3);
  tmp.print('\n');
  debugSDWriteResult("/last_liters.txt", (w > 0), w, micros() - t0);
  tmp.flush();
  tmp.close();

  SD.remove("/last_liters.txt");
  SD.rename(tempname, "/last_liters.txt");

  xSemaphoreGive(sdMutex);
}


// ============================================================
// CARICAMENTO LITRI ACQUA DA SD
// ============================================================
float loadLastLitersFromSD() {
  if (!initSD()) return -1;

  if (!takeMutexWarn(sdMutex, 500, "sd"))
    return -1;

  if (!SD.exists("/last_liters.txt")) {
    xSemaphoreGive(sdMutex);
    return -1;
  }

  File f = SD.open("/last_liters.txt", FILE_READ);
  if (!f) {
    xSemaphoreGive(sdMutex);
    return -1;
  }

  String s = f.readStringUntil('\n');
  f.close();
  xSemaphoreGive(sdMutex);

  s.trim();
  return s.toFloat();
}