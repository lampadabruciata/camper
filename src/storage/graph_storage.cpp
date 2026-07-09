#include "graph_storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Variabili esterne usate da restoreGraphFromSD
extern bool timeValid;
extern time_t nowEpoch();
extern time_t utcToLocal(time_t t);
extern volatile bool graphRestored;

void saveGraphToSD() {

  if (!initSD()) return;
  if (!takeMutexWarn(sdMutex, 500, "sd")) return;

  Serial.println("[GRAPH] saveGraphToSD chiamato");

  SD.remove("/.tmp_graph.bin");

  File f = SD.open("/.tmp_graph.bin", FILE_WRITE);

  if (!f) {
    Serial.println("[GRAPH] ERRORE apertura file!");
    xSemaphoreGive(sdMutex);
    return;
  }

  f.seek(0);

  // ================= SCRITTURA =================

  GraphHeader h{GRAPH_MAGIC, GRAPH_VER, 0};
  f.write((uint8_t*)&h, sizeof(h));

  for (int i = 0; i < NUM_SAMPLES; i++) {
    f.write((uint8_t*)&voltageSamples[i], sizeof(float));
    f.write((uint8_t*)&currentSamples[i], sizeof(float));
    f.write((uint8_t*)&solarCurrentSamples[i], sizeof(float));
    f.write((uint8_t*)timeLabels[i], 6);
  }

  for (int i = 0; i < NUM_SAMPLES; i++) {
    f.write((uint8_t*)&epochMinutes[i], sizeof(uint32_t));
  }

  f.write((uint8_t*)&sampleIndex, sizeof(int));

  f.write((uint8_t*)ahHourlyToday, sizeof(ahHourlyToday));
  f.write((uint8_t*)ahHourlyYesterday, sizeof(ahHourlyYesterday));
  f.write((uint8_t*)ahChargeHourlyToday, sizeof(ahChargeHourlyToday));
  f.write((uint8_t*)ahChargeHourlyYesterday, sizeof(ahChargeHourlyYesterday));
  f.write((uint8_t*)solAhHourlyToday, sizeof(solAhHourlyToday));
  f.write((uint8_t*)solAhHourlyYesterday, sizeof(solAhHourlyYesterday));

  f.write((uint8_t*)&ahTodayTotal_global, sizeof(float));
  f.write((uint8_t*)&ahYesterdayTotal_global, sizeof(float));
  f.write((uint8_t*)&ahChargeTodayTotal_global, sizeof(float));
  f.write((uint8_t*)&ahChargeYesterdayTotal_global, sizeof(float));
  f.write((uint8_t*)&solAhTodayTotal_global, sizeof(float));
  f.write((uint8_t*)&solAhYesterdayTotal_global, sizeof(float));
  f.write((uint8_t*)&currentHour, sizeof(int));

  // ================= FINE SCRITTURA =================

  f.flush();

  Serial.printf("[GRAPH] dimensione file: %u bytes\n", f.size());

  f.close();

  Serial.println("[GRAPH] scrittura completata");

  SD.remove("/graph24h.bin");
  
  if (!SD.rename("/.tmp_graph.bin", "/graph24h.bin")) {
    Serial.println("[GRAPH] ERRORE rename file!");
  } else {
    Serial.println("[GRAPH] file scritto e rinominato OK");
  }

  xSemaphoreGive(sdMutex);
}

void restoreGraphFromSD() {
  Serial.println("[GRAPH] Avvio restoreGraphFromSD() (SAFE)...");

  if (!initSD()) {
    Serial.println("[GRAPH] SD non pronta");
    return;
  }

  if (!takeMutexWarn(sdMutex, 500, "sd")) {
    Serial.println("[GRAPH] sdMutex non disponibile");
    return;
  }

  const char* path = "/graph24h.bin";

  if (!SD.exists(path)) {
    xSemaphoreGive(sdMutex);
    Serial.println("[GRAPH] nessun file graph24h.bin");
    return;
  }

  File f = SD.open(path, FILE_READ);
  if (!f) {
    xSemaphoreGive(sdMutex);
    Serial.println("[GRAPH] errore apertura file");
    return;
  }

  // ================= DIMENSIONE ATTESA =================
  const size_t bytesPerSample =
    (3 * sizeof(float)) + 6 + sizeof(uint32_t);

  const size_t blockSamples = NUM_SAMPLES * bytesPerSample;
  const size_t blockHeader  = sizeof(GraphHeader);
  const size_t blockIndex   = sizeof(int);

  const size_t blockHours =
    sizeof(ahHourlyToday) +
    sizeof(ahHourlyYesterday) +
    sizeof(ahChargeHourlyToday) +
    sizeof(ahChargeHourlyYesterday) +
    sizeof(solAhHourlyToday) +
    sizeof(solAhHourlyYesterday);

  const size_t blockTotals =
    sizeof(float) * 6 +
    sizeof(int);

  const size_t expectedSize =
    blockHeader + blockSamples + blockIndex + blockHours + blockTotals;

  size_t fileSize = f.size();
  Serial.printf("[GRAPH] File size = %u, expected = %u\n",
    (unsigned)fileSize, (unsigned)expectedSize);

  if (fileSize != expectedSize) {
    Serial.println("[GRAPH] Dimensione NON valida — restore annullato");
    f.close();
    xSemaphoreGive(sdMutex);
    return;
  }

  // ================= LETTURA BUFFER =================
  uint8_t* buf = (uint8_t*)malloc(fileSize);
  if (!buf) {
    Serial.println("[GRAPH] malloc fallita");
    f.close();
    xSemaphoreGive(sdMutex);
    return;
  }

  size_t readBytes = f.read(buf, fileSize);
  f.close();
  xSemaphoreGive(sdMutex);

  if (readBytes != fileSize) {
    Serial.println("[GRAPH] lettura incompleta");
    free(buf);
    return;
  }

  Serial.println("[GRAPH] Lettura OK — parsing...");

  uint8_t* p = buf;

  // ================= HEADER =================
  GraphHeader* hdr = (GraphHeader*)p;
  if (hdr->magic != GRAPH_MAGIC || hdr->ver < 1) {
    Serial.println("[GRAPH] Header non valido");
    free(buf);
    return;
  }

  p += sizeof(GraphHeader);

  // ================= SAMPLES =================
  for (int i = 0; i < NUM_SAMPLES; i++) {

    if ((i % 100) == 0) {
        vTaskDelay(1);   // ✅ nutrizione watchdog
    }

    memcpy(&voltageSamples[i], p, sizeof(float)); p += 4;
    memcpy(&currentSamples[i], p, sizeof(float)); p += 4;
    memcpy(&solarCurrentSamples[i], p, sizeof(float)); p += 4;
    memcpy(timeLabels[i], p, 6);
    timeLabels[i][5] = 0;
    p += 6;
  }

  // ================= EPOCH =================
  if (hdr->ver >= 3) {
    for (int i = 0; i < NUM_SAMPLES; i++) {
        if ((i % 100) == 0) {
            vTaskDelay(1);   // ✅ nutrizione watchdog
        }
        memcpy(&epochMinutes[i], p, sizeof(uint32_t));
        p += sizeof(uint32_t);
    }

  } else {
    for (int i = 0; i < NUM_SAMPLES; i++)
      epochMinutes[i] = 0;
  }

  // ================= INDEX =================
  memcpy(&sampleIndex, p, sizeof(int));
  p += sizeof(int);

  // ================= DATI ORARI =================
  if (hdr->ver >= 2) {
    memcpy(ahHourlyToday, p, sizeof(ahHourlyToday)); p += sizeof(ahHourlyToday);
    memcpy(ahHourlyYesterday, p, sizeof(ahHourlyYesterday)); p += sizeof(ahHourlyYesterday);
    memcpy(ahChargeHourlyToday, p, sizeof(ahChargeHourlyToday)); p += sizeof(ahChargeHourlyToday);
    memcpy(ahChargeHourlyYesterday, p, sizeof(ahChargeHourlyYesterday)); p += sizeof(ahChargeHourlyYesterday);
    memcpy(solAhHourlyToday, p, sizeof(solAhHourlyToday)); p += sizeof(solAhHourlyToday);
    memcpy(solAhHourlyYesterday, p, sizeof(solAhHourlyYesterday)); p += sizeof(solAhHourlyYesterday);

    memcpy(&ahTodayTotal_global, p, sizeof(float)); p += 4;
    memcpy(&ahYesterdayTotal_global, p, sizeof(float)); p += 4;
    memcpy(&ahChargeTodayTotal_global, p, sizeof(float)); p += 4;
    memcpy(&ahChargeYesterdayTotal_global, p, sizeof(float)); p += 4;
    memcpy(&solAhTodayTotal_global, p, sizeof(float)); p += 4;
    memcpy(&solAhYesterdayTotal_global, p, sizeof(float)); p += 4;
    memcpy(&currentHour, p, sizeof(int)); p += 4;
  }

  free(buf);

  // ================= ✅ PULIZIA DATI VECCHI =================
  if (timeValid) {
    time_t nowMin = nowEpoch() / 60;

    int removed = 0;

    for (int i = 0; i < NUM_SAMPLES; i++) {
      uint32_t ep = epochMinutes[i];

      if (ep == 0) continue;

      if (ep < (nowMin - 1440)) {
        epochMinutes[i] = 0;
        voltageSamples[i] = 0;
        currentSamples[i] = 0;
        solarCurrentSamples[i] = 0;
        strcpy(timeLabels[i], "--:--");
        removed++;
      }
    }

    Serial.printf("[GRAPH] Pulizia completata, rimossi %d punti vecchi\n", removed);
  } else {
    Serial.println("[GRAPH] timeValid=false → skip pulizia");
  }

  Serial.printf("[GRAPH] Restore COMPLETATO. sampleIndex=%d\n", sampleIndex);

  graphRestored = true;
}