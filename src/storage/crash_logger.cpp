#include "crash_logger.h"
#include <WiFi.h>
#include <esp_system.h>
#include <vector>

// Numero massimo di log crash salvati
#define MAX_CRASH_LOGS 20

void logCrash(const char *reason) {

  static volatile bool crashLogging = false;
  if (crashLogging) return;
  crashLogging = true;

  // ----------- TIME STRING ROBUSTO -----------
  char clockBuf[32] = "UNKNOWN_TIME";

  // ✅ 1) PROVA DIRETTA RTC (NON DIPENDE DA timeValid)
  if (rtc.begin()) {
    DateTime now = rtc.now();
    if (now.year() >= 2023 && now.year() < 2100) {
      snprintf(clockBuf, sizeof(clockBuf),
               "%04d-%02d-%02d %02d:%02d",
               now.year(), now.month(), now.day(),
               now.hour(), now.minute());
    }
  }

  // ✅ 2) FALLBACK: usa tempo derivato (se RTC non pronto)
  if (strcmp(clockBuf, "UNKNOWN_TIME") == 0 && timeValid) {
    snprintf(clockBuf, sizeof(clockBuf),
             "%04d-%02d-%02d %02d:%02d",
             currentYear, currentMonth, currentDay,
             currentHour, currentMinute);
  }

  // ✅ 3) FALLBACK FINALE
  if (strcmp(clockBuf, "UNKNOWN_TIME") == 0) {
    snprintf(clockBuf, sizeof(clockBuf),
             "NO_TIME (t=%lu ms)", millis());
  }

  // ----------- SD INIT -----------
  if (!initSD()) {
    crashLogging = false;
    return;
  }

  if (sdMutex == NULL) {
    sdMutex = xSemaphoreCreateMutex();
    if (sdMutex == NULL) { crashLogging = false; return; }
  }

  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(300)) != pdTRUE) {
    crashLogging = false;
    return;
  }

  // ----------- LEGGI FILE ESISTENTE -----------
  String oldLogs = "";
  if (SD.exists("/crashlog.txt")) {
    File r = SD.open("/crashlog.txt", FILE_READ);
    if (r) {
      oldLogs = r.readString();
      r.close();
    }
  }

  // ----------- SPLIT EVENTI -----------
  std::vector<String> events;
  int idx = 0;
  while (true) {
    int start = oldLogs.indexOf("--- CRASH EVENT ---", idx);
    if (start < 0) break;
    int end = oldLogs.indexOf("--------------------", start);
    if (end < 0) break;
    end += strlen("--------------------");

    events.push_back(oldLogs.substring(start, end));
    idx = end;
  }

  // ✅ Mantieni ultimi N
  while (events.size() >= MAX_CRASH_LOGS)
    events.erase(events.begin());

  // ----------- RESET REASON COMPLETO -----------
  esp_reset_reason_t rr = esp_reset_reason();
  const char* rrStr = "UNKNOWN";

  switch (rr) {
    case ESP_RST_POWERON:   rrStr = "POWERON"; break;
    case ESP_RST_EXT:       rrStr = "EXT_RESET (USB/BTN)"; break;
    case ESP_RST_SW:        rrStr = "SW_RESET"; break;
    case ESP_RST_PANIC:     rrStr = "PANIC"; break;
    case ESP_RST_INT_WDT:   rrStr = "INT_WDT"; break;
    case ESP_RST_TASK_WDT:  rrStr = "TASK_WDT"; break;
    case ESP_RST_WDT:       rrStr = "WDT"; break;
    case ESP_RST_DEEPSLEEP: rrStr = "DEEPSLEEP_WAKE"; break;
    case ESP_RST_BROWNOUT:  rrStr = "BROWNOUT"; break;
    case ESP_RST_SDIO:      rrStr = "SDIO"; break;
    default:                rrStr = "UNKNOWN"; break;
  }

  // ----------- INFO SISTEMA -----------
  unsigned long now = millis();

  float socPct = (batteryAh_max > 0.5f)
                   ? (100.0f * batteryAh_now / batteryAh_max)
                   : 0.0f;

  float tempESP = temperatureRead();

  uint32_t heap = ESP.getFreeHeap();

  uint32_t psramFree = 0;
#ifdef BOARD_HAS_PSRAM
  psramFree = ESP.getFreePsram();
#endif

  uint32_t stackLoop = uxTaskGetStackHighWaterMark(NULL);
  uint32_t stackSensor = SensorTask ? uxTaskGetStackHighWaterMark(SensorTask) : 0;
  uint32_t stackDisplay = DisplayTaskHandle ? uxTaskGetStackHighWaterMark(DisplayTaskHandle) : 0;

  uint8_t wifiClients = WiFi.softAPgetStationNum();

  // ----------- COSTRUZIONE EVENTO -----------
  String newEvent;
  newEvent.reserve(1200);

  newEvent += "\n--- CRASH EVENT ---\n";
  newEvent += "Time(ms): " + String(now) + "\n";
  newEvent += "Clock: " + String(clockBuf) + "\n";
  newEvent += "Reason: " + String(reason) + "\n";
  newEvent += "ResetReason: " + String(rrStr) + " (" + String((int)rr) + ")\n";

  newEvent += "Page: " + String((int)currentPage) +
              " Dim: " + String(isDimmed ? 1 : 0) +
              " Excl: " + String(levelExclusiveMode ? 1 : 0) + "\n";

  newEvent += "ADS=" + String(adsPresent) +
              " LIS=" + String(lisPresent) +
              " Scale1=" + String(scale1Ready) +
              " Scale2=" + String(scale2Ready) + "\n";

  newEvent += "TempInt=" + String(tempInt,1) +
              " TempExt=" + String(tempExt,1) +
              " TempESP=" + String(tempESP,1) + "\n";

  newEvent += "V=" + String(vBattery,2) +
              " I=" + String(currentFiltered,2) +
              " SolA=" + String(solarAmps,2) + "\n";

  newEvent += "Ah=" + String(batteryAh_now,1) +
              "/" + String(batteryAh_max,1) +
              " SOC=" + String((int)(socPct+0.5f)) + "%\n";

  newEvent += "Gas1=" + String(gasResidual1,2) +
              " Gas2=" + String(gasResidual2,2) + "\n";

  newEvent += "Water=" + String(remainingLiters,1) +
              " Flow=" + String(lastFlowLmin,2) + "\n";

  newEvent += "Heap=" + String(heap) +
              " PSRAM=" + String(psramFree) + "\n";

  newEvent += "Stack: loop=" + String(stackLoop) +
              " sensor=" + String(stackSensor) +
              " display=" + String(stackDisplay) + "\n";

  newEvent += "WiFiClients=" + String(wifiClients) + "\n";

  newEvent += "HB: loop=" + String((unsigned long)hbLoop) +
              " sensor=" + String((unsigned long)hbSensorLoop) +
              " level=" + String((unsigned long)hbLevelTask) +
              " display=" + String((unsigned long)hbDisplayTask) + "\n";

  newEvent += "LastMs: loop=" + String((unsigned long)lastLoopMs) +
              " sensor=" + String((unsigned long)lastSensorLoopMs) +
              " level=" + String((unsigned long)lastLevelTaskMs) +
              " display=" + String((unsigned long)lastDisplayTaskMs) + "\n";

  newEvent += "TaskMaxUs: loop=" + String((unsigned long)loopMaxUs) +
              " sensor=" + String((unsigned long)sensorLoopMaxUs) +
              " level=" + String((unsigned long)levelTaskMaxUs) +
              " display=" + String((unsigned long)displayTaskMaxUs) + "\n";

  newEvent += "BlockMaxUs: updVolt=" + String((unsigned long)updateCurrentAndVoltageMaxUs) +
              " solar=" + String((unsigned long)readSolarCurrentMaxUs) +
              " dht=" + String((unsigned long)updateDHTMaxUs) +
              " gas1=" + String((unsigned long)updateGas1MaxUs) +
              " gas2=" + String((unsigned long)updateGas2MaxUs) +
              " level=" + String((unsigned long)updateLevelMaxUs) +
              " saveAhState=" + String((unsigned long)saveAhStateToSDMaxUs) +
              " saveAh=" + String((unsigned long)saveAhToSDMaxUs) +
              " saveGraph=" + String((unsigned long)saveGraphToSDMaxUs) + "\n";

  newEvent += "DHT: reads=" + String((unsigned long)dhtReads) +
              " failures=" + String((unsigned long)dhtReadFailures) +
              " maxReadUs=" + String((unsigned long)dhtMaxReadUs) + "\n";

  // ----------- LAST DIAG SNAPSHOT (/last_diag.txt) -----------
  // Associe a ogni reboot (INT_WDT) la fotografia del sistema salvata
  // periodicamente per saveDiagSnapshot() prima del crash.
  if (SD.exists("/last_diag.txt")) {
    File d = SD.open("/last_diag.txt", FILE_READ);
    if (d) {
      newEvent += "--- LAST DIAG SNAPSHOT ---\n";
      newEvent += "\n";
      newEvent += d.readString();
      if (newEvent[newEvent.length() - 1] != '\n')
        newEvent += "\n";
      newEvent += "--------------------------\n";
      d.close();
    }
  }

  newEvent += "--------------------\n";

  // ----------- SALVA -----------
  events.push_back(newEvent);

  File w = SD.open("/crashlog.txt", FILE_WRITE);
  if (w) {
    for (auto &e : events)
      w.print(e);
    w.close();
  }

  xSemaphoreGive(sdMutex);
  crashLogging = false;
}

void logBoot() {
  // ----------- TIME STRING ROBUSTO -----------
  char clockBuf[32] = "UNKNOWN";

  // ✅ 1) PROVA DIRETTA RTC (NON DIPENDE DA timeValid)
  if (rtc.begin()) {
    DateTime now = rtc.now();
    if (now.year() >= 2023 && now.year() < 2100) {
      snprintf(clockBuf, sizeof(clockBuf),
               "%04d-%02d-%02d %02d:%02d:%02d",
               now.year(), now.month(), now.day(),
               now.hour(), now.minute(), now.second());
    }
  }

  // ✅ 2) FALLBACK: usa tempo derivato (se RTC non pronto)
  if (strcmp(clockBuf, "UNKNOWN") == 0 && timeValid) {
    snprintf(clockBuf, sizeof(clockBuf),
             "%04d-%02d-%02d %02d:%02d:%02d",
             currentYear, currentMonth, currentDay,
             currentHour, currentMinute, 0);
  }

  // ----------- SD INIT -----------
  if (!initSD()) {
    return;
  }

  if (sdMutex == NULL) {
    sdMutex = xSemaphoreCreateMutex();
    if (sdMutex == NULL) return;
  }

  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(300)) != pdTRUE) {
    return;
  }

  // ----------- RESET REASON COMPLETO -----------
  esp_reset_reason_t rr = esp_reset_reason();
  const char* rrStr = "UNKNOWN";

  switch (rr) {
    case ESP_RST_POWERON:   rrStr = "POWERON"; break;
    case ESP_RST_EXT:       rrStr = "EXT_RESET (USB/BTN)"; break;
    case ESP_RST_SW:        rrStr = "SW_RESET"; break;
    case ESP_RST_PANIC:     rrStr = "PANIC"; break;
    case ESP_RST_INT_WDT:   rrStr = "INT_WDT"; break;
    case ESP_RST_TASK_WDT:  rrStr = "TASK_WDT"; break;
    case ESP_RST_WDT:       rrStr = "WDT"; break;
    case ESP_RST_DEEPSLEEP: rrStr = "DEEPSLEEP_WAKE"; break;
    case ESP_RST_BROWNOUT:  rrStr = "BROWNOUT"; break;
    case ESP_RST_SDIO:      rrStr = "SDIO"; break;
    default:                rrStr = "UNKNOWN"; break;
  }

  // ----------- INFO SISTEMA -----------
  uint32_t heap = ESP.getFreeHeap();

  // ----------- COSTRUZIONE EVENTO (snprintf) -----------
  char lineBuf[128];
  snprintf(lineBuf, sizeof(lineBuf),
           "%s | %s | heap=%u\n",
           clockBuf, rrStr, heap);

  // ----------- SALVA IN APPEND -----------
  File w = SD.open("/bootlog.txt", FILE_APPEND);
  if (w) {
    w.print(lineBuf);
    w.close();
  }

  xSemaphoreGive(sdMutex);
}

// ============================================================
// LEGGI L'ULTIMO EVENTO CRASH DAL LOG (per pagina Diagnostica)
// Lettura limitata alla coda del file per minimizzare l'heap.
// ============================================================
String readLastCrashInfo() {
  if (!initSD()) return String("SD non disponibile");

  if (sdMutex == NULL) sdMutex = xSemaphoreCreateMutex();
  if (sdMutex == NULL) return String("Mutex SD non disponibile");

  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(300)) != pdTRUE)
    return String("SD occupata");

  String result = "Nessun crash registrato";

  if (SD.exists("/crashlog.txt")) {
    File r = SD.open("/crashlog.txt", FILE_READ);
    if (r) {
      size_t size = r.size();
      size_t start = (size > 1600) ? (size - 1600) : 0;
      r.seek(start);
      String tailStr = r.readString();
      r.close();

      int marker = tailStr.lastIndexOf("--- CRASH EVENT ---");
      if (marker >= 0) {
        result = tailStr.substring(marker);
        if (result.length() > 400) result = result.substring(0, 400);
      } else if (tailStr.length() > 0) {
        result = "(nessun evento completo recente nel crash log)";
      }
    } else {
      result = "crashlog.txt non apribile";
    }
  }

  xSemaphoreGive(sdMutex);
  return result;
}
