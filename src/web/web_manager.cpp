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

// Variabili esterne per handleDataAhLive (definite in main.cpp)
extern int minuteIndex;
extern float ahMinuteDischarge[60];
extern float ahMinuteCharge[60];

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
    DynamicJsonDocument json(4096);

    JsonArray dis = json.createNestedArray("discharge");
    JsonArray chg = json.createNestedArray("charge");
    JsonArray labels = json.createNestedArray("labels");

    for (int i = 0; i < 24; i++) {

        dis.add(ahHourlyToday[i]);
        chg.add(ahChargeHourlyToday[i]);

        char buf[4];
        snprintf(buf, sizeof(buf), "%02d", i);
        labels.add(buf);
    }

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

void handleIndex() {
    sendFile("/index.html", "text/html");
}

void handleChartJs() {
    sendFile("/chart.min.js", "application/javascript");
}
