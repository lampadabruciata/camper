#include "crash_logger.h"
#include <WiFi.h>
#include <esp_system.h>
#include <vector>

// ---- TEMPO: usa le stesse funzioni di time_manager.cpp ----
// (utcToLocal/nowEpoch/updateDST) per esprimere l'ora in LOCALE (CET/CEST),
// esattamente come fanno rtc= in last_diag.txt e dateTime della UI.
#include <TimeLib.h>
extern time_t nowEpoch();
extern time_t utcToLocal(time_t utcEpoch);
extern void updateDST();

// Numero massimo di log crash salvati
#define MAX_CRASH_LOGS 20

// Caché crash (definita più avanti in questo file) — forward declaration
void crashCacheInvalidate();

void logCrash(const char *reason) {

  static volatile bool crashLogging = false;
  if (crashLogging) return;
  crashLogging = true;

  // ----------- TIME STRING ROBUSTO -----------
  char clockBuf[32] = "UNKNOWN_TIME";

  // Ora LOCALE (CET/CEST): stessa conversione di rtc= (last_diag) e
  // dateTime (UI), cioe' utcToLocal() su un epoch UTC.
  updateDST();   // garantisce tzOffsetHours corretto (anche subito al boot)

  // ✅ 1) Fonte primaria: RTC diretto convertito in locale (non dipende da timeValid)
  if (rtc.begin()) {
    DateTime now = rtc.now();
    if (now.year() >= 2023 && now.year() < 2100) {
      time_t loc = utcToLocal((time_t)now.unixtime());
      tmElements_t tm;
      breakTime(loc, tm);
      snprintf(clockBuf, sizeof(clockBuf),
               "%04d-%02d-%02d %02d:%02d",
               tm.Year + 1970, tm.Month, tm.Day, tm.Hour, tm.Minute);
    }
  }

  // ✅ 2) FALLBACK: tempo derivato (se RTC non pronto)
  if (strcmp(clockBuf, "UNKNOWN_TIME") == 0 && timeValid) {
    time_t loc = utcToLocal(nowEpoch());
    tmElements_t tm;
    breakTime(loc, tm);
    snprintf(clockBuf, sizeof(clockBuf),
             "%04d-%02d-%02d %02d:%02d",
             tm.Year + 1970, tm.Month, tm.Day, tm.Hour, tm.Minute);
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

  newEvent += "TempInt=" + String(dhtSensors[DHT_CHANNEL_INTERNAL].temperature,1) +
              " TempExt=" + String(dhtSensors[DHT_CHANNEL_EXTERNAL].temperature,1) +
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

  // Telemetria DHT per-canale (solo diagnostica): readEnabled/valid/reads/
  // failures/failCount/quarantineCount/quarantinedUntil/maxReadUs/lastValidMs/ageSec.
  {
    const uint32_t nowMs = now;
    for (int ch = 0; ch < DHT_CHANNEL_COUNT; ch++) {
      const dhtSensor_t &s = dhtSensors[ch];
      newEvent += String(dhtChannelLabel((DhtChannel)ch)) +
                  ": readEnabled=" + String(s.readEnabled ? 1 : 0) +
                  " valid=" + String(s.valid ? 1 : 0) +
                  " reads=" + String((unsigned long)s.reads) +
                  " failures=" + String((unsigned long)s.failures) +
                  " failCount=" + String((unsigned long)s.failCount) +
                  " quarantineCount=" + String((unsigned long)s.quarantineCount) +
                  " quarantinedUntil=" + String((unsigned long)s.quarantinedUntil) +
                  " maxReadUs=" + String((unsigned long)s.maxReadUs) +
                  " lastValidMs=" + String((unsigned long)s.lastValidMs) +
                  " ageSec=" + String((long)dhtAgeSec(s)) +
                  // Cumulativi in-frame: INERTI a 0 con DHTNEW. Nel fork
                  // sopravvivevano al reboot e discriminavano
                  // linea statica (molte attese scadute) vs bitstream lento
                  // (zero attese, maxPulseUs molto sopra il nominale).
                  " frameLowTimeoutPulses=" + String((unsigned long)s.frameLowTimeoutPulses) +
                  " frameHighTimeoutPulses=" + String((unsigned long)s.frameHighTimeoutPulses) +
                  " maxPulseUs=" + String((unsigned long)s.maxPulseUs) + "\n";
    }
  }

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

  // Invalida la cache crash (il file è cambiato)
  crashCacheInvalidate();

  xSemaphoreGive(sdMutex);
  crashLogging = false;
}

void logBoot() {
  // ----------- TIME STRING ROBUSTO -----------
  char clockBuf[32] = "UNKNOWN";

  // ✅ 0) Calcola fuso/DST: a boot updateDST() non è ancora stato eseguito
  //        da SensorLoop → lo forziamo qui per esprimere l'ora in LOCALE
  updateDST();

  // Ora LOCALE (CET/CEST): stessa conversione di rtc= e dateTime (UI).
  // 1) Fonte primaria: RTC diretto convertito in locale
  if (rtc.begin()) {
    DateTime now = rtc.now();
    if (now.year() >= 2023 && now.year() < 2100) {
      time_t loc = utcToLocal((time_t)now.unixtime());
      tmElements_t tm;
      breakTime(loc, tm);
      snprintf(clockBuf, sizeof(clockBuf),
               "%04d-%02d-%02d %02d:%02d:%02d",
               tm.Year + 1970, tm.Month, tm.Day, tm.Hour, tm.Minute, tm.Second);
    }
  }

  // ✅ 2) FALLBACK: tempo derivato (se RTC non pronto)
  if (strcmp(clockBuf, "UNKNOWN") == 0 && timeValid) {
    time_t loc = utcToLocal(nowEpoch());
    tmElements_t tm;
    breakTime(loc, tm);
    snprintf(clockBuf, sizeof(clockBuf),
             "%04d-%02d-%02d %02d:%02d:%02d",
             tm.Year + 1970, tm.Month, tm.Day, tm.Hour, tm.Minute, tm.Second);
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

// ============================================================
// API DIAGNOSTICA CRASH — toda la cronología de /crashlog.txt
// (sin usar bootCount NVS). La página Diagnóstica muestra estos datos.
// ============================================================

// Lee el archivo completo /crashlog.txt en `out` bajo mutex SD.
// Devuelve false si SD no disponible/ocupada.
static bool readCrashLogFile(String &out) {
  if (!initSD()) return false;
  if (sdMutex == NULL) sdMutex = xSemaphoreCreateMutex();
  if (sdMutex == NULL) return false;
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(300)) != pdTRUE) return false;

  out = "";
  if (SD.exists("/crashlog.txt")) {
    File r = SD.open("/crashlog.txt", FILE_READ);
    if (r) {
      out = r.readString();
      r.close();
    }
  }
  xSemaphoreGive(sdMutex);
  return true;
}

// ============================================================
// CACHÉ CRASH (livello leggero)
// ============================================================
// La pagina Diagnostica fa polling ~1s su /api/boot: senza cache si
// rileggerebbe /crashlog.txt a ogni richiesta (40KB peggiori casi).
// Con un TTL di pochi secondi l'I/O SD si riduce drasticamente.
// La cache si invalida automaticamente a ogni nuovo logCrash().
static uint32_t crashCacheTtlMs = 0;
static int      crashCacheCount = 0;
static std::vector<CrashEventInfo> crashCacheEvents;

static bool crashCacheIsValid() {
  return crashCacheTtlMs != 0 && (millis() - crashCacheTtlMs) < 60000;
}

void crashCacheInvalidate() {
  crashCacheTtlMs = 0;
  crashCacheCount = 0;
  crashCacheEvents.clear();
}

// Legge /crashlog.txt (una volta sola) e riempie conteggio + lista nella cache.
// Ritorna false se la lettura SD non è riuscita.
static bool crashCacheRefresh() {
  String all;
  if (!readCrashLogFile(all)) return false;

  std::vector<int> starts, ends;
  int idx = 0;
  while (true) {
    int start = all.indexOf("--- CRASH EVENT ---", idx);
    if (start < 0) break;
    int end = all.indexOf("--------------------", start);
    if (end < 0) end = (int)all.length();
    else end += strlen("--------------------");
    starts.push_back(start);
    ends.push_back(end);
    idx = end;
  }

  crashCacheCount = (int)starts.size();
  crashCacheEvents.clear();

  // Reconstruir en orden inverso: out[0] = más reciente
  for (size_t i = starts.size(); i-- > 0; ) {
    String ev = all.substring(starts[i], ends[i]);
    CrashEventInfo info;
    int p, e;
    p = ev.indexOf("Clock: ");
    if (p >= 0) {
      e = ev.indexOf('\n', p);
      info.timestamp = ev.substring(p + 7, (e < 0) ? ev.length() : e);
    }
    p = ev.indexOf("Reason: ");
    if (p >= 0) {
      e = ev.indexOf('\n', p);
      info.reason = ev.substring(p + 8, (e < 0) ? ev.length() : e);
    }
    p = ev.indexOf("ResetReason: ");
    if (p >= 0) {
      e = ev.indexOf('\n', p);
      info.resetReason = ev.substring(p + 13, (e < 0) ? ev.length() : e);
    }
    info.timestamp.trim();  // quita espacios residuales
    info.reason.trim();
    info.resetReason.trim();
    crashCacheEvents.push_back(info);
  }

  crashCacheTtlMs = millis();
  return true;
}

// Cuenta eventos presentes en /crashlog.txt (0 = archivo ausente o vacío).
int countCrashEvents() {
  if (!crashCacheIsValid()) crashCacheRefresh();
  return crashCacheCount;
}

// Rellena `out` con los eventos de la cache (más reciente primero).
// Cada elemento incluye timestamp, causa breve y reset reason.
// No incluye el texto completo (para minimizar RAM): el detalle se
// obtiene bajo demanda con readCrashEventRaw().
void readAllCrashEvents(std::vector<CrashEventInfo>& out) {
  out.clear();
  if (!crashCacheIsValid()) crashCacheRefresh();
  out = crashCacheEvents;
}

// Bloque completo sin procesar del evento `index` (0 = más reciente).
// Cadena vacía si el índice no existe. Este es el detalle completo del crash
// logger (mismo contenido que se escribe en /crashlog.txt).
String readCrashEventRaw(int index) {
  String all;
  if (!readCrashLogFile(all)) return String("");
  if (index < 0) return String("");

  std::vector<int> starts, ends;
  int idx = 0;
  while (true) {
    int start = all.indexOf("--- CRASH EVENT ---", idx);
    if (start < 0) break;
    int end = all.indexOf("--------------------", start);
    if (end < 0) end = (int)all.length();
    else end += strlen("--------------------");
    starts.push_back(start);
    ends.push_back(end);
    idx = end;
  }

  int n = (int)starts.size();
  if (index >= n) return String("");
  int pos = n - 1 - index;  // índice invertido: más reciente = 0
  return all.substring(starts[pos], ends[pos]);
}
