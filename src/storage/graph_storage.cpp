#include "graph_storage.h"

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