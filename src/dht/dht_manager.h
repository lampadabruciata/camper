#ifndef DHT_MANAGER_H
#define DHT_MANAGER_H

#include "DHT.h"
#include <stdint.h>

// ============================================================
// MODELLO DATI UNIFICATO DHT
// ============================================================
// Ogni sensore DHT (interno/esterno) è rappresentato da un'unica
// struttura che raggruppa: istanza fisica, abilitazione letture,
// ultime letture, stato di disponibilità e stato quarantena.
// Definita in dht_manager.cpp come array dhtSensors[] e
// accessibile a tutto il progetto tramite il canale DHT_CHANNEL_*.
typedef struct {
  DHT   dht;         // istanza fisica del sensore
  bool  readEnabled; // false = letture fisiche sospese ([DIAG-WDT] DHT interno)

  volatile float temperature; // °C — NAN se mai letto o non valido
  volatile float humidity;    // %  — NAN se mai letto o non valido
  volatile bool  valid;       // false = assente, NAN o in quarantena

  uint32_t failCount;        // errori consecutivi correnti (quarantena)
  uint32_t quarantinedUntil; // millis() di fine quarantena (0 = non in quarantena)
} dhtSensor_t;

// Canali sensori DHT (indici dell'array dhtSensors[])
enum DhtChannel {
  DHT_CHANNEL_EXTERNAL = 0, // AM2301 (DHT21) su GPIO 14
  DHT_CHANNEL_INTERNAL,     // DHT21 su GPIO 4
  DHT_CHANNEL_COUNT
};

// Modello dati unificato dei sensori DHT (esterno + interno)
extern dhtSensor_t dhtSensors[DHT_CHANNEL_COUNT];

// ============================================================
// DIAGNOSTICA DHT
// ============================================================
// Definite in dht_manager.cpp, accessibili a tutto il progetto.
volatile extern uint32_t dhtReads;        // numero totale cicli di lettura tentati
volatile extern uint32_t dhtReadFailures; // cicli falliti (nessun canale valido)
volatile extern uint32_t dhtMaxReadUs;    // durata massima singola lettura (µs)

// Inizializzazione sensori DHT (interno ed esterno)
void initDHT();

// Aggiornamento temperature/umidità di entrambi i canali con
// gestione quarantena. Mantiene la logica !isnan: aggiorna solo
// i valori validi. Eseguito SOLO da DHTTask (mai da SensorLoop).
void updateDHT();

#endif // DHT_MANAGER_H
