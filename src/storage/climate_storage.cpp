#include "climate_storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

// Variabili esterne usate da restoreClimateFromSD
extern bool timeValid;
extern time_t nowEpoch();

// ============================================================
// SALVATAGGIO ATOMICO (pattern saveGraphToSD)
// /climate24h.bin via /.tmp_climate.bin + rename
// ============================================================
void saveClimateToSD() {

  if (!initSD()) return;
  if (!takeMutexWarn(sdMutex, 500, "sd")) return;

  Serial.println("[CLIM] save start");

  // Rimuove un eventuale residuo del file temporaneo
  SD.remove("/.tmp_climate.bin");

  File f = SD.open("/.tmp_climate.bin", FILE_WRITE);

  if (!f) {
    Serial.println("[CLIM] save failed: tmp open");
    xSemaphoreGive(sdMutex);
    return;
  }

  f.seek(0);

  // ================= SCRITTURA =================

  ClimateHeader h{CLIMATE_MAGIC, CLIMATE_VER, 0, (uint32_t)climateSampleIndex};
  f.write((uint8_t*)&h, sizeof(h));

  for (int i = 0; i < CLIMATE_SAMPLES; i++)
    f.write((uint8_t*)&climateTempInt[i], sizeof(float));

  for (int i = 0; i < CLIMATE_SAMPLES; i++)
    f.write((uint8_t*)&climateTempExt[i], sizeof(float));

  for (int i = 0; i < CLIMATE_SAMPLES; i++)
    f.write((uint8_t*)&climateHumInt[i], sizeof(float));

  for (int i = 0; i < CLIMATE_SAMPLES; i++)
    f.write((uint8_t*)&climateHumExt[i], sizeof(float));

  for (int i = 0; i < CLIMATE_SAMPLES; i++)
    f.write((uint8_t*)&climateEpochMinutes[i], sizeof(uint32_t));

  // ================= FINE SCRITTURA =================

  f.flush();
  size_t sz = f.size();
  f.close();

  Serial.printf("[CLIM] save ok: %u bytes\n", (unsigned)sz);

  // 🌀 Atomicità: il file è visibile solo dopo il rename
  SD.remove("/climate24h.bin");

  if (!SD.rename("/.tmp_climate.bin", "/climate24h.bin")) {
    Serial.println("[CLIM] save failed: rename");
  } else {
    Serial.println("[CLIM] save ok");
  }

  xSemaphoreGive(sdMutex);
}

// ============================================================
// RESTORE (pattern restoreGraphFromSD)
// File assente/corrotto → restore annullato, nessun crash.
// Al termine valido: pulizia campioni >24h + climateRestored=true.
// ============================================================
void restoreClimateFromSD() {
  Serial.println("[CLIM] restore start");

  if (!initSD()) {
    Serial.println("[CLIM] restore failed: SD not ready");
    return;
  }

  if (!takeMutexWarn(sdMutex, 500, "sd")) {
    Serial.println("[CLIM] restore failed: sdMutex busy");
    return;
  }

  const char* path = "/climate24h.bin";

  if (!SD.exists(path)) {
    xSemaphoreGive(sdMutex);
    Serial.println("[CLIM] restore: no file");
    return;
  }

  File f = SD.open(path, FILE_READ);
  if (!f) {
    xSemaphoreGive(sdMutex);
    Serial.println("[CLIM] restore failed: open");
    return;
  }

  // ================= DIMENSIONE ATTESA =================
  const size_t expectedSize =
    sizeof(ClimateHeader) +
    CLIMATE_SAMPLES * (4 * sizeof(float) + sizeof(uint32_t));

  size_t fileSize = f.size();
  Serial.printf("[CLIM] file size=%u expected=%u\n",
                (unsigned)fileSize, (unsigned)expectedSize);

  if (fileSize != expectedSize) {
    Serial.println("[CLIM] restore failed: bad size");
    f.close();
    xSemaphoreGive(sdMutex);
    return;
  }

  // ================= LETTURA COMPLETA =================
  uint8_t* buf = (uint8_t*)malloc(fileSize);
  if (!buf) {
    Serial.println("[CLIM] restore failed: malloc");
    f.close();
    xSemaphoreGive(sdMutex);
    return;
  }

  size_t readBytes = f.read(buf, fileSize);
  f.close();
  xSemaphoreGive(sdMutex);

  if (readBytes != fileSize) {
    Serial.println("[CLIM] restore failed: short read");
    free(buf);
    return;
  }

  // ================= VERIFICA HEADER =================
  ClimateHeader* hdr = (ClimateHeader*)buf;

  if (hdr->magic != CLIMATE_MAGIC) {
    Serial.println("[CLIM] restore failed: bad magic");
    free(buf);
    return;
  }

  if (hdr->ver != CLIMATE_VER) {
    Serial.println("[CLIM] restore failed: bad version");
    free(buf);
    return;
  }

  // Indice corrente del buffer circolare (clamp di sicurezza)
  int restoredIndex = (int)hdr->climateSampleIndex;
  if (restoredIndex < 0 || restoredIndex >= CLIMATE_SAMPLES)
    restoredIndex = 0;
  climateSampleIndex = restoredIndex;

  uint8_t* p = buf + sizeof(ClimateHeader);

  // ================= PARSING DATI =================
  for (int i = 0; i < CLIMATE_SAMPLES; i++) {
    if ((i % 100) == 0) vTaskDelay(1);   // ✅ nutrizione watchdog
    memcpy(&climateTempInt[i], p, sizeof(float)); p += sizeof(float);
  }

  for (int i = 0; i < CLIMATE_SAMPLES; i++) {
    if ((i % 100) == 0) vTaskDelay(1);   // ✅ nutrizione watchdog
    memcpy(&climateTempExt[i], p, sizeof(float)); p += sizeof(float);
  }

  for (int i = 0; i < CLIMATE_SAMPLES; i++) {
    if ((i % 100) == 0) vTaskDelay(1);   // ✅ nutrizione watchdog
    memcpy(&climateHumInt[i], p, sizeof(float)); p += sizeof(float);
  }

  for (int i = 0; i < CLIMATE_SAMPLES; i++) {
    if ((i % 100) == 0) vTaskDelay(1);   // ✅ nutrizione watchdog
    memcpy(&climateHumExt[i], p, sizeof(float)); p += sizeof(float);
  }

  for (int i = 0; i < CLIMATE_SAMPLES; i++) {
    if ((i % 100) == 0) vTaskDelay(1);   // ✅ nutrizione watchdog
    memcpy(&climateEpochMinutes[i], p, sizeof(uint32_t)); p += sizeof(uint32_t);
  }

  free(buf);

  // ================= PULIZIA DATI VECCHI (>24h) =================
  if (timeValid) {
    time_t nowMin = nowEpoch() / 60;

    int removed = 0;

    for (int i = 0; i < CLIMATE_SAMPLES; i++) {
      uint32_t ep = climateEpochMinutes[i];
      if (ep == 0) continue;   // gap: niente da rimuovere

      if (ep < (nowMin - 1440)) {
        climateEpochMinutes[i] = 0;
        climateTempInt[i]      = NAN;
        climateTempExt[i]      = NAN;
        climateHumInt[i]       = NAN;
        climateHumExt[i]       = NAN;
        removed++;
      }
    }

    Serial.printf("[CLIM] cleanup removed %d samples\n", removed);
  } else {
    Serial.println("[CLIM] cleanup skipped: time not valid");
  }

  Serial.println("[CLIM] restore completed");

  climateRestored = true;
}