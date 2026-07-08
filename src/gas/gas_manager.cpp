#include "gas_manager.h"
#include <SD.h>
#include <Arduino.h>
#include <freertos/semphr.h>
#include "HX711.h"

// Dichiarazioni esterne (definite in main.cpp)
extern SemaphoreHandle_t sdMutex;
extern bool initSD();
extern void logCrash(const char *reason);
extern void i2cRecoverBus(void);
extern bool takeMutexWarn(SemaphoreHandle_t m, uint32_t timeoutMs, const char *name);

// Variabili globali gas (definite in main.cpp)
extern float gasResidual1;
extern float gasResidual2;
extern float gasInitial1;
extern float gasInitial2;
extern float gasConsumed1;
extern float gasConsumed2;
extern float lastScale1Reading;
extern float lastScale2Reading;
extern bool scale1Ready;
extern bool scale2Ready;
extern HX711 scale1;
extern HX711 scale2;

// Mutex per HX711
extern SemaphoreHandle_t hxMutex;

// Mutex per sezione critica stato
extern portMUX_TYPE stateMux;

// Macro per sezione critica (definite in main.cpp)
#define STATE_ENTER() taskENTER_CRITICAL(&stateMux)
#define STATE_EXIT()  taskEXIT_CRITICAL(&stateMux)

// ============================================================
// WRAPPER THREAD-SAFE PER HX711 (usa hxMutex)
// ============================================================
bool scaleSafeGetUnits(HX711 &scale, long samples, float &out) {
  if (!takeMutexWarn(hxMutex, 300, "hx")) return false;
  bool ok = scale.is_ready();
  if (ok) out = scale.get_units(samples);
  xSemaphoreGive(hxMutex);
  return ok;
}

bool scaleSafeTare(HX711 &scale) {
  if (!takeMutexWarn(hxMutex, 600, "hx")) return false;
  bool ok = scale.is_ready();
  if (ok) scale.tare();
  xSemaphoreGive(hxMutex);
  return ok;
}

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

    // Se e il bus I2C che va in timeout ripetutamente, tenta recovery
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

    // Se non e cambiato niente, inutile scrivere su SD
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

// ============================================================
// RIPRISTINO GAS DA SD
// ============================================================
void restoreGasFromSD() {
  gasResidual1 = loadGasFromSD("/last_gas1.txt");
  gasResidual2 = loadGasFromSD("/last_gas2.txt");

  gasInitial1 = gasResidual1;
  gasConsumed1 = 0.0f;

  gasInitial2 = gasResidual2;
  gasConsumed2 = 0.0f;

  // ----------- BOMBOLA 1 -----------
  if (scale1.is_ready()) {
    lastScale1Reading = scale1.get_units(5);
    scale1Ready = true;            // abbiamo una lettura valida
  } else {
    lastScale1Reading = gasResidual1;  // fallback sicuro
    scale1Ready = false;
  }

  // ----------- BOMBOLA 2 -----------
  if (scale2.is_ready()) {
    lastScale2Reading = scale2.get_units(5);
    scale2Ready = true;
  } else {
    lastScale2Reading = gasResidual2;
    scale2Ready = false;
  }
}

// ============================================================
// AGGIORNAMENTO GAS
// ============================================================
void updateGas(HX711 &scale,
               float &gasInitial,
               float &gasConsumed,
               float &gasResidual,
               float &lastReading,
               const char* filename,
               unsigned long &lastSaveTs,
               volatile bool &forceSaveFlag,
               bool &scaleReady)
{
  // 1) Lettura protetta HX711
  float currentReading = 0.0f;
  if (!scaleSafeGetUnits(scale, 5, currentReading)) {
    return; // scala non pronta o mutex non ottenuto
  }

  // 2) Prima lettura valida
  if (!scaleReady) {
    STATE_ENTER();
    lastReading = currentReading;
    scaleReady  = true;
    STATE_EXIT();
    return;
  }

  // 3) Calcoli stato in sezione critica (breve)
  float delta = 0.0f;
  STATE_ENTER();
  delta = lastReading - currentReading;     // diminuzione = consumo
  gasConsumed += delta;
  gasResidual  = gasInitial - gasConsumed;
  if (gasResidual < 0) gasResidual = 0;
  lastReading = currentReading;
  STATE_EXIT();

  // 4) Salvataggio periodico o forzato (fuori dalla sezione critica)
  unsigned long now = millis();
  if (forceSaveFlag || (now - lastSaveTs >= 30000UL)) {
    saveGasToSD(filename, gasResidual);
    lastSaveTs    = now;
    forceSaveFlag = false;
  }
}