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
