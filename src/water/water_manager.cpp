#include "water_manager.h"
#include <SD.h>
#include <Arduino.h>
#include <WebServer.h>
#include <freertos/semphr.h>

// Dichiarazioni esterne (definite in main.cpp)
extern SemaphoreHandle_t sdMutex;
extern bool initSD();
extern bool takeMutexWarn(SemaphoreHandle_t m, uint32_t timeoutMs, const char *name);
extern void debugSDWriteResult(const char* file, bool ok, size_t written, uint32_t microsTime);
extern WebServer server;

// Variabili globali per l'acqua (definite in main.cpp)
extern volatile float totalLitersConsumed;
extern volatile float remainingLiters;
extern float tankCapacity;

// Riferimenti di persistenza automatica (definiti in main.cpp): servono a far
// partire i trigger A/B gia' "in pari" dopo l'avvio o dopo un /setTank, cosi'
// da non riscrivere subito su SD un file identico.
extern volatile float    lastLitersSavedValue;
extern volatile uint32_t lastLitersSaveMs;

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


// ============================================================
// RIPRISTINA LITRI CONSUMATI DA SD
// ============================================================
void restoreLitersFromSD() {
  float v = loadLastLitersFromSD();

  if (v >= 0)
    totalLitersConsumed = v;
  else {
    totalLitersConsumed = 0.0f;
    saveLastLitersToSD(0.0f); // ha mutex interno
  }

  remainingLiters = tankCapacity - totalLitersConsumed;
  if (remainingLiters < 0) remainingLiters = 0;

  // Allinea i riferimenti di persistenza automatica al valore appena
  // ripristinato, che e' gia' quello contenuto in /last_liters.txt (o 0 se il
  // file non esisteva e l'abbiamo appena creato). Senza questo, il primo giro di
  // SensorLoop vedrebbe TRIGGER A/B veri (millis() - 0 >= 30000) e riscriverebbe
  // un file identico: scrittura inutile e wear su SD.
  lastLitersSavedValue = totalLitersConsumed;
  lastLitersSaveMs     = millis();
}


// ============================================================
// IMPOSTAZIONE DEI LITRI ATUAL PRESENTI NEL SERBATOIO ACQUA
// Endpoint: /setTank?litri=<litri>   (preferito)
//           /setTank?val=<litri>     (compatibilità)
// Il valore inserito rappresenta i litri ATTUALMENTE presenti.
// La capacità (tankCapacity) resta invariata.
// ============================================================
void handleSetTank() {
  float v;

  if (server.hasArg("litri")) {
    v = server.arg("litri").toFloat();
  } else if (server.hasArg("val")) {
    v = server.arg("val").toFloat();
  } else {
    server.send(400, "text/plain", "missing litri (or val)");
    return;
  }

  if (v < 0) v = 0;
  if (v > tankCapacity) v = tankCapacity;

  remainingLiters = v;
  totalLitersConsumed = tankCapacity - remainingLiters;

  // Salva su SD i litri consumati (totalLitersConsumed), non 0
  saveLastLitersToSD(totalLitersConsumed);

  // Questa scrittura e' gia' avvenuta: riallinea i riferimenti di persistenza
  // automatica, altrimenti il prossimo giro di SensorLoop riscriverebbe gli
  // stessi litri. Vale sia per "serbatoio pieno" (consumo azzerato, 0.000
  // gia' su disco) sia per "serbatoio vuoto" (consumo = capacita').

  lastLitersSavedValue = totalLitersConsumed;
  lastLitersSaveMs     = millis();

  server.send(200, "text/plain", "ok");
  Serial.printf("[WATER] Serbatoio: %.1f L / capacita %.1f L (consumati %.1f)\n",
                remainingLiters, tankCapacity, totalLitersConsumed);
}

