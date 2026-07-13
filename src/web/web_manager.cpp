#include "web_manager.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>

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
extern float vBattery;
extern float currentFiltered;
extern float batteryAh_max;
extern float batteryAh_now;
extern float tempInt;
extern float tempExt;
extern float humInt;
extern float humExt;
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

// Funzione esterna per ora corrente
time_t nowEpoch();

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
    json["tempInt"] = tempInt;
    json["tempExt"] = tempExt;
    json["humInt"]  = humInt;
    json["humExt"]  = humExt;

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

    const int N = 360;  // valore di default
    const int total = 1440;  // numSamples
    const int start = (sampleIndex - N + total) % total;

    Serial.printf("[MEM] solar JSON: %u\n", ESP.getFreeHeap());

    DynamicJsonDocument json(8000);

    JsonArray solarA = json.createNestedArray("solarA");
    JsonArray labels = json.createNestedArray("labels");
    JsonArray epoch  = json.createNestedArray("epoch");

    time_t nowMin = nowEpoch() / 60;

    int validCount = 0;
    const int MAX_WEB_POINTS = 300;

    for (int k = 0; k < N && validCount < MAX_WEB_POINTS; k++) {
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

    const int N = 360;
    int step = 1;
    if (N > 300) {
        step = (N + 300 - 1) / 300;
    }

    DynamicJsonDocument json(16000);

    JsonArray volt = json.createNestedArray("volt");
    JsonArray amps = json.createNestedArray("amps");
    JsonArray sol  = json.createNestedArray("solar");
    JsonArray labels = json.createNestedArray("labels");
    JsonArray epoch  = json.createNestedArray("epoch");

    const int total = 1440;  // numSamples
    const int start = (sampleIndex - N + total) % total;

    time_t nowMin = nowEpoch() / 60;

    int validCount = 0;

    for (int k = 0; k < N && validCount < 300; k += step) {
        
        
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


void handleIndex() {
    sendFile("/index.html", "text/html");
}

void handleChartJs() {
    sendFile("/chart.min.js", "application/javascript");
}
