#include "level_manager.h"
#include <Arduino.h>
#include <freertos/semphr.h>
#include <Adafruit_LIS3DH.h>

// Dichiarazioni esterne (definite in main.cpp)
extern SemaphoreHandle_t i2cMutex;
extern Adafruit_LIS3DH lis;
extern volatile float lvl_roll_zero;
extern volatile float lvl_pitch_zero;
extern QueueHandle_t prefsQueue;

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

// Dichiarazione esterna (definita in main.cpp)
extern bool saveLevelZeroToSD(float roll0, float pitch0);

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