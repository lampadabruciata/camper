#include "gas_manager.h"
#include <SD.h>
#include <Arduino.h>
#include <freertos/semphr.h>

// Dichiarazioni esterne (definite in main.cpp)
extern SemaphoreHandle_t sdMutex;
extern bool initSD();
extern void logCrash(const char *reason);
extern void i2cRecoverBus(void);

// ============================================================
// MUTEX TIMEOUT LOGGER (max 1 messaggio al secondo per mutex)
// ============================================================
bool takeMutexWarn(SemaphoreHandle_t m, uint32_t timeoutMs, const char *name) {
  // Prova a prendere il mutex entro timeoutMs
  if (xSemaphoreTake(m, pdMS_TO_TICKS(timeoutMs)) == pdTRUE)
    return true;

  // Timeout: logga al massimo 1 volta al secondo
  static unsigned long lastPrint = 0;
  unsigned long now = millis();
  if (now - lastPrint >= 1000) {
    Serial.printf("[WARN] MUTEX '%s' TIMEOUT (> %u ms)\n", name ? name : "?", (unsigned)timeoutMs);
    logCrash("Mutex timeout");   // scrive su /crashlog.txt lo stato corrente

    // Se è il bus I2C che va in timeout ripetutamente, tenta recovery
    if (name && strcmp(name, "i2c") == 0) {
      static unsigned long firstErrTs = 0;
      static uint8_t errCount = 0;
      unsigned long nowTs = millis();

      // Finestra di 10s per contare i timeout
      if (firstErrTs == 0 || (nowTs - firstErrTs) > 10000) {
        firstErrTs = nowTs;
        errCount = 0;
      }

      errCount++;
      // 5 timeout in 10 secondi → fai recovery del bus
      if (errCount >= 5) {
        Serial.println("[I2C] troppi timeout: eseguo recovery bus");
        i2cRecoverBus();     // richiede la forward declaration al Passo 1
        errCount = 0;
        firstErrTs = nowTs;
      }
    }

    lastPrint = now;
  }

  return false;  // mutex non ottenuto
}

// ============================================================
// DEBUG SD
// ============================================================
void debugSDWriteResult(const char* file, bool ok, size_t written, uint32_t microsTime) {
  // Forza stampa su seriale durante il debug
  if (!ok) {
    Serial.printf("[SD] ERRORE scrittura su %s\n", file);
    return;
  }
  Serial.printf("[SD] %s: scritto %u byte in %lu us\n",
                file, (unsigned)written, (unsigned long)microsTime);
}

void saveGasToSD(const char* filename, float kg) {
    if (!initSD()) return;
    
    float lastValue = loadGasFromSD(filename);

    // Se non è cambiato niente, inutile scrivere su SD
    if (fabs(lastValue - kg) < 0.01f) {
        return;
    }
    
    if (!takeMutexWarn(sdMutex, 500, "sd"))
        return;

    
    const char* tempname = "/.tmp_gas.txt";
    File tmp = SD.open(tempname, FILE_WRITE);

    if (tmp) {
        tmp.seek(0);

        uint32_t t0 = micros();
        int w = tmp.print(kg, 2);
        tmp.print('\n'); // newline per sicurezza

        debugSDWriteResult(filename, (w > 0), w, micros() - t0);

        tmp.flush();
        tmp.close();

        // Rimuovi il vecchio e rinomina il nuovo
        SD.remove(filename);
        SD.rename(tempname, filename);
    }

    xSemaphoreGive(sdMutex);
}


float loadGasFromSD(const char* filename) {
  if (!initSD()) return 0.0f;

  if (!takeMutexWarn(sdMutex, 500, "sd"))
    return 0.0f;

  if (!SD.exists(filename)) {
    xSemaphoreGive(sdMutex);
    return 0.0f;
  }

  File f = SD.open(filename, FILE_READ);
  if (!f) {
    xSemaphoreGive(sdMutex);
    return 0.0f;
  }

  String s = f.readStringUntil('\n');
  f.close();
  xSemaphoreGive(sdMutex);

  s.trim();
  return s.toFloat();
}