#include "level_manager.h"
#include <Arduino.h>
#include <freertos/semphr.h>
#include <Adafruit_LIS3DH.h>
#include <SD.h>

// Dichiarazioni esterne (definite in main.cpp)
extern SemaphoreHandle_t i2cMutex;
extern Adafruit_LIS3DH lis;
extern volatile float lvl_roll_zero;
extern volatile float lvl_pitch_zero;
extern QueueHandle_t prefsQueue;
extern SemaphoreHandle_t sdMutex;

// Dichiarazioni esterne (definite in main.cpp)
extern bool initSD();
extern bool takeMutexWarn(SemaphoreHandle_t m, uint32_t timeoutMs, const char *name);
extern void debugSDWriteResult(const char* file, bool ok, size_t written, uint32_t microsTime);

// Enum locale con gli stessi valori di PrefsCmdType (definito in main.cpp)
enum {
  CMD_SAVE_LEVEL_R0 = 4,
  CMD_SAVE_LEVEL_P0 = 5
};

struct PrefsMessage {
  uint8_t cmd;
  float fvalue;
  int   ivalue;
};

// Legge una volta e salva gli offset zero in NVS (tramite PrefsTask)
void captureLevelZero() {
  sensors_event_t ev;
  bool ok = false;

  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    ok = lis.getEvent(&ev);
    xSemaphoreGive(i2cMutex);
  }
  if (!ok) return;

  float ax = ev.acceleration.x / 9.80665f;
  float ay = ev.acceleration.y / 9.80665f;
  float az = ev.acceleration.z / 9.80665f;

  float roll_raw  = atan2f(ay, az) * 57.2957795f;
  float pitch_raw = atan2f(-ax, sqrtf(ay*ay + az*az)) * 57.2957795f;

  // Aggiorna offset in RAM
  lvl_roll_zero  = roll_raw;
  lvl_pitch_zero = pitch_raw;

  // Persisti con PrefsTask (NVS)
  PrefsMessage mr, mp;
  mr.cmd    = CMD_SAVE_LEVEL_R0;
  mr.fvalue = lvl_roll_zero;
  mr.ivalue = 0;

  mp.cmd    = CMD_SAVE_LEVEL_P0;
  mp.fvalue = lvl_pitch_zero;
  mp.ivalue = 0;

  xQueueSend(prefsQueue, &mr, 0);
  xQueueSend(prefsQueue, &mp, 0);

  // Persisti anche su SD
  saveLevelZeroToSD(lvl_roll_zero, lvl_pitch_zero);
}

// ---------- Livella: SD persistence ----------
bool saveLevelZeroToSD(float roll0, float pitch0) {
  if (!initSD()) return false;
  if (!takeMutexWarn(sdMutex, 500, "sd")) return false;
  
  const char* tmp = "/.tmp_level_zero.txt";
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) { xSemaphoreGive(sdMutex); return false; }
  f.seek(0);
  uint32_t t0 = micros();
  int n = f.printf("%.6f,%.6f\n", roll0, pitch0);
  f.flush();
  f.close();
  debugSDWriteResult("/level_zero.txt", (n > 0), n, micros() - t0);
  SD.remove("/level_zero.txt");
  SD.rename(tmp, "/level_zero.txt");
  xSemaphoreGive(sdMutex);
  return (n > 0);
}
