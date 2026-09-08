#include "web_manager.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <TimeLib.h>
#include <esp_system.h>
#include <math.h>
#include "storage/crash_logger.h"
#include "dht/dht_manager.h"
#include <vector>

// WebServer esterno (definito in main.cpp)
extern WebServer server;

// Variabili esterne per handleDataWater (definite in main.cpp)
extern volatile float lastFlowLmin;
extern volatile float totalLitersConsumed;
extern volatile float remainingLiters;
extern float tankCapacity;

// Variabili esterne per handleDataGas (definite in main.cpp)
extern float gasResidual1;
extern float gasResidual2;

// Variabili esterne per handleDataLive (definite in main.cpp)
extern volatile float vBattery;
extern volatile float currentFiltered;
extern float batteryAh_max;
extern float batteryAh_now;
extern int currentHour;
extern int currentMinute;
extern float chargeEfficiency;

// Variabili esterne per handleDataAh24h (definite in main.cpp)
extern float ahHourlyToday[24];
extern float ahChargeHourlyToday[24];
extern float ahHourlyYesterday[24];
extern float ahChargeHourlyYesterday[24];
extern float ahTodayTotal_global;
extern float ahYesterdayTotal_global;
extern float ahChargeTodayTotal_global;
extern float ahChargeYesterdayTotal_global;

// Variabili esterne per handleDataAhLive (definite in main.cpp)
extern int minuteIndex;
extern float ahMinuteDischarge[60];
extern float ahMinuteCharge[60];

// Variabili esterne per handleDataSolar (definite in main.cpp)
extern volatile unsigned long lastWebActivity;
extern int sampleIndex;
extern uint32_t epochMinutes[];
extern float solarCurrentSamples[];
extern char timeLabels[][6];
extern volatile float solarAmps;
extern volatile float solarWatts;
extern volatile float solarAhToday;
extern float solAhHourlyToday[24];
extern float solAhHourlyYesterday[24];

// Variabili esterne per handleDataBattery (definite in main.cpp)
extern float voltageSamples[];
extern float currentSamples[];

// Variabili esterne per handleDataClimate (definite in main.cpp)
extern float climateTempInt[];
extern float climateTempExt[];
extern float climateHumInt[];
extern float climateHumExt[];
extern uint32_t climateEpochMinutes[];
extern int climateSampleIndex;

// Variabili esterni (definite en main.cpp)
extern bool sdOk;
extern volatile bool adsPresent;
extern volatile bool lisPresent;
extern bool timeValid;
extern int8_t tzOffsetHours;
time_t nowEpoch();
time_t utcToLocal(time_t utcEpoch);

void sendFile(const char* path, const char* mime) {
  if (!SPIFFS.exists(path)) {
    server.send(404, "text/plain", "file non trovato");
    return;
  }

  File f = SPIFFS.open(path, "r");
  if (!f) {
    server.send(500, "text/plain", "errore apertura file");
    return;
  }

  // caching: il browser non richiede 100 volte chart.min.js
  server.sendHeader("Cache-Control", "public, max-age=3600");

  server.streamFile(f, mime);
  f.close();
}

void handleDataWater() {
    DynamicJsonDocument json(3000);

    json["flowLmin"]       = lastFlowLmin;
    json["totalLiters"]    = totalLitersConsumed;
    json["remainingLiters"]= remainingLiters;
    json["capacity"]       = tankCapacity;

    json["pct"] = (tankCapacity > 0 ?
                   100.0f * remainingLiters / tankCapacity : 0);

    String out;
    serializeJson(json, out);
    server.send(200, "application/json", out);
}


void handleDataGas() {
    DynamicJsonDocument json(3000);

    json["gas1"] = gasResidual1;
    json["gas2"] = gasResidual2;

    // GAS_MAX_1 e GAS_MAX_2 sono 10.0f (definite in main.cpp)
    json["pct1"] = (10.0f > 0 ? 100.0f * gasResidual1 / 10.0f : 0);
    json["pct2"] = (10.0f > 0 ? 100.0f * gasResidual2 / 10.0f : 0);

    String out;
    serializeJson(json, out);
    server.send(200, "application/json", out);
}

void handleDataLive() {
    DynamicJsonDocument json(3000);

    json["vBattery"] = vBattery;
    json["amps"] = currentFiltered;
    json["socPct"] = (batteryAh_max > 0 ? 100.0f * batteryAh_now / batteryAh_max : 0);
    // DHT dedicato: se il sensore è assente o in quarantena → null,
    // il frontend mostrerà "--".
    if (dhtSensors[DHT_CHANNEL_INTERNAL].valid) json["tempInt"] = dhtSensors[DHT_CHANNEL_INTERNAL].temperature;
    else                                          json["tempInt"] = nullptr;
    if (dhtSensors[DHT_CHANNEL_EXTERNAL].valid) json["tempExt"] = dhtSensors[DHT_CHANNEL_EXTERNAL].temperature;
    else                                          json["tempExt"] = nullptr;
    if (dhtSensors[DHT_CHANNEL_INTERNAL].valid) json["humInt"]  = dhtSensors[DHT_CHANNEL_INTERNAL].humidity;
    else                                          json["humInt"]  = nullptr;
    if (dhtSensors[DHT_CHANNEL_EXTERNAL].valid) json["humExt"]  = dhtSensors[DHT_CHANNEL_EXTERNAL].humidity;
    else                                          json["humExt"]  = nullptr;

    // Ora
    char buf[6];
    snprintf(buf, 6, "%02d:%02d", currentHour, currentMinute);
    json["time"] = buf;
    json["eff"] = chargeEfficiency;
    
    String out;
    serializeJson(json, out);
    server.send(200, "application/json", out);
}


void handleDataAh24h() {
    DynamicJsonDocument json(6000);

    // Array Ah scaricati (discharge)
    JsonArray ahHourlyTodayArr = json.createNestedArray("ahHourlyToday");
    JsonArray ahHourlyYesterdayArr = json.createNestedArray("ahHourlyYesterday");
    
    // Array Ah caricati (charge)
    JsonArray ahChargeHourlyTodayArr = json.createNestedArray("ahChargeHourlyToday");
    JsonArray ahChargeHourlyYesterdayArr = json.createNestedArray("ahChargeHourlyYesterday");
    
    // Labels ore
    JsonArray labels = json.createNestedArray("labels");

    for (int i = 0; i < 24; i++) {
        ahHourlyTodayArr.add(ahHourlyToday[i]);
        ahHourlyYesterdayArr.add(ahHourlyYesterday[i]);
        ahChargeHourlyTodayArr.add(ahChargeHourlyToday[i]);
        ahChargeHourlyYesterdayArr.add(ahChargeHourlyYesterday[i]);
        
        char buf[4];
        snprintf(buf, sizeof(buf), "%02d", i);
        labels.add(buf);
    }

    // Totali
    json["ahTodayTotal"] = ahTodayTotal_global;
    json["ahYesterdayTotal"] = ahYesterdayTotal_global;
    json["ahChargeTodayTotal"] = ahChargeTodayTotal_global;
    json["ahChargeYesterdayTotal"] = ahChargeYesterdayTotal_global;

    String out;
    serializeJson(json, out);
    server.send(200, "application/json", out);
}


void handleDataAhLive() {

    DynamicJsonDocument json(4096);

    JsonArray dis = json.createNestedArray("discharge");
    JsonArray chg = json.createNestedArray("charge");
    JsonArray labels = json.createNestedArray("labels");

    int idx = minuteIndex;

    for (int i = 0; i < 60; i++) {

        int j = (idx + i) % 60;

        dis.add(ahMinuteDischarge[j]);
        chg.add(ahMinuteCharge[j]);

        char buf[6];
        snprintf(buf, sizeof(buf), "-%d", 59 - i);
        labels.add(buf);
    }

    String out;
    serializeJson(json, out);

    server.send(200, "application/json", out);
}

void handleDataSolar() {
    lastWebActivity = millis();

    // Finestra richiesta dal frontend: 60 (1h) / 360 (6h) / 1440 (24h)
    int n = 360;  // default se parametro assente o non valido
    if (server.hasArg("n")) {
        n = server.arg("n").toInt();
        if (n <= 0 || n > 1440) n = 360;
    }

    // Limite assoluto: MAI piu' di MAX_GRAPH_POINTS punti per grafico.
    // Verifica DynamicJsonDocument(8000) - ArduinoJson v7, slotSize ~8B:
    //   300 punti -> 3*300+73 = 973 slot -> 8 pool da 1KB = 8192B > 8000B => overflow (JSON troncato).
    //   250 punti -> 823 slot        -> 7 pool = 7168B  <= 8000B => sicuro (margine ~10%).
    const int MAX_GRAPH_POINTS = 250;
    // step con arrotondamento all'intero superiore (ceil): copre TUTTA la finestra con <= MAX_GRAPH_POINTS punti
    const int step = (n + MAX_GRAPH_POINTS - 1) / MAX_GRAPH_POINTS;

    const int total = 1440;  // numSamples
    const int start = (sampleIndex - n + total) % total;

    Serial.printf("[MEM] solar JSON: %u\n", ESP.getFreeHeap());

    DynamicJsonDocument json(8000);

    JsonArray solarA = json.createNestedArray("solarA");
    JsonArray labels = json.createNestedArray("labels");
    JsonArray epoch  = json.createNestedArray("epoch");

    time_t nowMin = nowEpoch() / 60;

    int validCount = 0;

    for (int k = 0; k < n && validCount < MAX_GRAPH_POINTS; k += step) {
        int idx = (start + k) % total;

        uint32_t ep = epochMinutes[idx];

        if (ep == 0 || ep < (nowMin - 1440)) continue;

        solarA.add(solarCurrentSamples[idx]);
        labels.add(timeLabels[idx]);
        epoch.add(ep);

        validCount++;
    }

    json["solarNow"] = solarAmps;
    json["solarW"] = solarWatts;
    json["vBattery"] = vBattery;
    json["solAhToday"] = solarAhToday;
    json["solarAhToday"] = solarAhToday;

    // Totali solari giornalieri
    extern float solAhTodayTotal_global;
    extern float solAhYesterdayTotal_global;
    json["solAhTodayTotal"] = solAhTodayTotal_global;
    json["solAhYesterdayTotal"] = solAhYesterdayTotal_global;

    JsonArray solAhTodayArr = json.createNestedArray("solAhHourlyToday");
    for (int i = 0; i < 24; i++) {
        solAhTodayArr.add(solAhHourlyToday[i]);
    }

    JsonArray solAhYesterdayArr = json.createNestedArray("solAhHourlyYesterday");
    for (int i = 0; i < 24; i++) {
        solAhYesterdayArr.add(solAhHourlyYesterday[i]);
    }

    String out;
    serializeJson(json, out);
    server.send(200, "application/json", out);
}

void handleDataBattery() {
    lastWebActivity = millis(); // ✅ segna attività web

    Serial.printf("[MEM] before JSON: %u\n", ESP.getFreeHeap());

    Serial.println(">>> JSON BUILD START <<<");

    // Finestra richiesta dal frontend: 60 (1h) / 360 (6h) / 1440 (24h)
    int n = 360;  // default se parametro assente o non valido
    if (server.hasArg("n")) {
        n = server.arg("n").toInt();
        if (n <= 0 || n > 1440) n = 360;
    }

    // Limite assoluto: MAI piu' di MAX_GRAPH_POINTS punti per grafico.
    // Verifica DynamicJsonDocument(16000) - ArduinoJson v7, slotSize ~8B:
    //   300 punti -> 5*300+137 = 1637 slot -> 13 pool da 1KB = 13312B <= 16000B => OK (margine ~17%).
    const int MAX_GRAPH_POINTS = 300;
    // step con arrotondamento all'intero superiore (ceil): copre TUTTA la finestra con <= MAX_GRAPH_POINTS punti
    const int step = (n + MAX_GRAPH_POINTS - 1) / MAX_GRAPH_POINTS;

    DynamicJsonDocument json(16000);

    JsonArray volt = json.createNestedArray("volt");
    JsonArray amps = json.createNestedArray("amps");
    JsonArray sol  = json.createNestedArray("solar");
    JsonArray labels = json.createNestedArray("labels");
    JsonArray epoch  = json.createNestedArray("epoch");

    const int total = 1440;  // numSamples
    const int start = (sampleIndex - n + total) % total;

    time_t nowMin = nowEpoch() / 60;

    int validCount = 0;

    for (int k = 0; k < n && validCount < MAX_GRAPH_POINTS; k += step) {
        
        
        if ((k % 20) == 0) {
            vTaskDelay(1);
        }

        int idx = (start + k) % total;

        uint32_t ep = epochMinutes[idx];

        if (ep == 0) {
            volt.add(0);
            amps.add(0);
            sol.add(0);
            labels.add("--:--");
            epoch.add(0);
            validCount++;
            continue;
        }

        if (ep < (nowMin - 1440)) continue;

        volt.add(voltageSamples[idx]);
        amps.add(currentSamples[idx]);
        sol.add(solarCurrentSamples[idx]);

        labels.add(timeLabels[idx]);
        epoch.add(ep);

        validCount++;
    }

    json["vBattery"] = vBattery;
    json["ampNow"] = currentFiltered;
    json["solarNow"] = solarAmps;
    json["socPct"] = (batteryAh_max > 0 ? 100.0f * batteryAh_now / batteryAh_max : 0);
    json["ahNow"] = batteryAh_now;
    json["ahMax"] = batteryAh_max;
    json["eff"] = chargeEfficiency;

    // ===== AGGIUNGI DATI AH ORARI E TOTALI (stesso modello di handleDataSolar) =====
    JsonArray ahHourlyTodayArr = json.createNestedArray("ahHourlyToday");
    for (int i = 0; i < 24; i++) {
        ahHourlyTodayArr.add(ahHourlyToday[i]);
    }

    JsonArray ahHourlyYesterdayArr = json.createNestedArray("ahHourlyYesterday");
    for (int i = 0; i < 24; i++) {
        ahHourlyYesterdayArr.add(ahHourlyYesterday[i]);
    }

    JsonArray ahChargeHourlyTodayArr = json.createNestedArray("ahChargeHourlyToday");
    for (int i = 0; i < 24; i++) {
        ahChargeHourlyTodayArr.add(ahChargeHourlyToday[i]);
    }

    JsonArray ahChargeHourlyYesterdayArr = json.createNestedArray("ahChargeHourlyYesterday");
    for (int i = 0; i < 24; i++) {
        ahChargeHourlyYesterdayArr.add(ahChargeHourlyYesterday[i]);
    }

    // Totali Ah
    json["ahTodayTotal"] = ahTodayTotal_global;
    json["ahYesterdayTotal"] = ahYesterdayTotal_global;
    json["ahChargeTodayTotal"] = ahChargeTodayTotal_global;
    json["ahChargeYesterdayTotal"] = ahChargeYesterdayTotal_global;

    String out;
    serializeJson(json, out);

    Serial.println(">>> JSON BUILD DONE <<<");
    
    server.send(200, "application/json", out);
}


// ============================================================
// CLIMA 24H — GET /data/climate
// Finestra: 60 (1h) / 360 (6h) / 1440 (24h) via ?n=N.
// JSON: { "tInt":[], "tExt":[], "hInt":[], "hExt":[],
//         "labels":[], "epoch":[] }
// Campioni mancanti (epoch == 0 oppure valore NAN) -> null nel
// JSON: MAI zero, MAI riempimento del buco.
// Stessa filosofia di handleDataBattery()/handleDataSolar():
//   - max 250 punti per grafico, step = ceil(n / MAX)
//   - vTaskDelay(1) ogni 20 iterazioni (nutrizione watchdog)
//   - DynamicJsonDocument a capacità fissa
// ============================================================
void handleDataClimate() {
    lastWebActivity = millis(); // ✅ segna attività web

    // Finestra richiesta dal frontend: 60 (1h) / 360 (6h) / 1440 (24h)
    int n = 360;  // default se parametro assente o non valido
    if (server.hasArg("n")) {
        n = server.arg("n").toInt();
        if (n <= 0 || n > 1440) n = 360;
    }

    // Limite assoluto: MAI piu' di MAX_GRAPH_POINTS punti per grafico.
    // Verifica DynamicJsonDocument(16000) - ArduinoJson v7, slotSize ~8B:
    //   6 array * 250 punti = 1500 slot (+overhead stringhe labels).
    //   Carico pari a handleDataBattery (5 array * 300 = 1500 slot),
    //   quindi lo stesso buffer 16000 resta valido (margine ~17%).
    const int MAX_GRAPH_POINTS = 250;
    // step con arrotondamento all'intero superiore (ceil): copre
    // TUTTA la finestra con <= MAX_GRAPH_POINTS punti
    const int step = (n + MAX_GRAPH_POINTS - 1) / MAX_GRAPH_POINTS;

    DynamicJsonDocument json(16000);

    JsonArray tInt   = json.createNestedArray("tInt");
    JsonArray tExt   = json.createNestedArray("tExt");
    JsonArray hInt   = json.createNestedArray("hInt");
    JsonArray hExt   = json.createNestedArray("hExt");
    JsonArray labels = json.createNestedArray("labels");
    JsonArray epoch  = json.createNestedArray("epoch");

    const int total = 1440;  // climateSamples (main.cpp)
    const int start = (climateSampleIndex - n + total) % total;

    time_t nowMin = nowEpoch() / 60;

    int validCount = 0;

    for (int k = 0; k < n && validCount < MAX_GRAPH_POINTS; k += step) {

        if ((k % 20) == 0) {
            vTaskDelay(1);   // ✅ nutrizione watchdog
        }

        int idx = (start + k) % total;

        uint32_t ep = climateEpochMinutes[idx];

        // campione troppo vecchio (fuori finestra 24h): non emesso
        if (ep != 0 && ep < (nowMin - 1440)) continue;

        // buco reale: epoch 0 (gap) oppure dato NAN (lettura fallita)
        bool missing =
            (ep == 0) ||
            isnan(climateTempInt[idx]) || isnan(climateTempExt[idx]) ||
            isnan(climateHumInt[idx])  || isnan(climateHumExt[idx]);

        // label orario locale (CET/CEST) derivata dall'epoch in minuti
        char buf[8] = "--:--";
        if (ep != 0) {
            time_t tLocal = utcToLocal((time_t)ep * 60);
            tmElements_t tm;
            breakTime(tLocal, tm);
            snprintf(buf, sizeof(buf), "%02d:%02d", tm.Hour, tm.Minute);
        }
        labels.add(buf);

        if (missing) {
            // buco reale: null nel JSON, NIENTE zero, NIENTE fill
            tInt.add(nullptr);
            tExt.add(nullptr);
            hInt.add(nullptr);
            hExt.add(nullptr);
        } else {
            tInt.add(climateTempInt[idx]);
            tExt.add(climateTempExt[idx]);
            hInt.add(climateHumInt[idx]);
            hExt.add(climateHumExt[idx]);
        }

        epoch.add(ep);   // 0 = gap, altrimenti epoch in minuti UTC

        validCount++;
    }

    String out;
    serializeJson(json, out);
    server.send(200, "application/json", out);
}

// ============================================================
// DIAGNOSTICA REBOOT — GET /api/boot
// ============================================================

static const char* resetReasonString(esp_reset_reason_t rr) {
  switch (rr) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT_RESET (USB/BTN)";
    case ESP_RST_SW:        return "SW_RESET";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP_WAKE";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

// Endpoint de diagnostica reboot. Devuelve la info de base del sistema.
// El contador derivado (crashCount) se basa unicamente en /crashlog.txt
// (sin bootCount NVS).
void handleDataBoot() {
  DynamicJsonDocument json(4096);

  json["crashCount"]   = countCrashEvents();
  json["resetReason"] = resetReasonString(esp_reset_reason());
  json["uptime"]      = (unsigned long)(millis() / 1000);
  json["heap"]        = ESP.getFreeHeap();

  // Data e ora attuale (ora locale CET/CEST, se RTC valido)
  char dtBuf[24] = "--";
  if (timeValid) {
    time_t nowUtc = nowEpoch();
    tmElements_t tm;
    breakTime(nowUtc + (tzOffsetHours * 3600), tm);
    snprintf(dtBuf, sizeof(dtBuf), "%04d-%02d-%02d %02d:%02d:%02d",
             tm.Year + 1970, tm.Month, tm.Day, tm.Hour, tm.Minute, tm.Second);
  }
  json["dateTime"]    = dtBuf;
  json["timeValid"]   = timeValid;

  json["sdOk"]        = sdOk;
  json["wifiClients"] = WiFi.softAPgetStationNum();
  json["adsPresent"]  = adsPresent;
  json["lisPresent"]  = lisPresent;

  // ---- TELEMETRIA DHT PER-CANALE (solo diagnostica) ----
  // Ogni canale espone readEnabled/valid/reads/failures/failCount/
  // quarantineCount/quarantinedUntil/maxReadUs/lastValidMs/ageSec.
  // NON modifica il formato di /data/live né quello del frontend.
  JsonObject dht = json.createNestedObject("dht");
  for (int ch = 0; ch < DHT_CHANNEL_COUNT; ch++) {
    const dhtSensor_t &s = dhtSensors[ch];
    JsonObject co = dht.createNestedObject(dhtChannelLabel((DhtChannel)ch));
    co["readEnabled"]      = s.readEnabled;
    co["valid"]            = (bool)s.valid;
    co["reads"]            = s.reads;
    co["failures"]         = s.failures;
    co["failCount"]        = s.failCount;
    co["quarantineCount"]  = s.quarantineCount;
    co["quarantinedUntil"] = s.quarantinedUntil;
    co["maxReadUs"]        = s.maxReadUs;
    co["lastValidMs"]      = s.lastValidMs;
    co["ageSec"]           = dhtAgeSec(s);
  }

  String out;
  serializeJson(json, out);
  server.send(200, "application/json", out);
}

// ============================================================
// CRASH LOG — GET /api/crash
// Lista ordinata di tutti gli eventi in /crashlog.txt, dal più
// recente al più vecchio. Array JSON di {id, timestamp, reason,
// resetReason}. L'id (0 = più recente) si usa in /api/crashdetail.
// ============================================================
void handleDataCrashList() {
  std::vector<CrashEventInfo> events;
  readAllCrashEvents(events);

  DynamicJsonDocument json((size_t)events.size() * 96 + 256);
  JsonArray arr = json.to<JsonArray>();
  int id = 0;
  for (auto &e : events) {
    JsonObject o = arr.createNestedObject();
    o["id"]          = id++;
    o["timestamp"]   = e.timestamp;
    o["reason"]      = e.reason;
    o["resetReason"] = e.resetReason;
  }

  String out;
  serializeJson(json, out);
  events.clear();
  server.send(200, "application/json", out);
}

// ============================================================
// CRASH DETAIL — GET /api/crashdetail?id=N
// Devuelve el bloque completo (detalle) de un evento crash.
// id 0 = más reciente, 1 = siguiente, etc.
// ============================================================
void handleDataCrashDetail() {
  int id = server.hasArg("id") ? server.arg("id").toInt() : -1;
  String raw = readCrashEventRaw(id);

  DynamicJsonDocument json((size_t)raw.length() * 2 + 128);
  json["detail"] = raw;
  String out;
  serializeJson(json, out);
  server.send(200, "application/json", out);
}
void handleIndex() {
    sendFile("/index.html", "text/html");
}

void handleChartJs() {
    sendFile("/chart.min.js", "application/javascript");
}
