#ifndef DHT_MANAGER_H
#define DHT_MANAGER_H

#include "DHT.h"

// ============================================================
// VARIABILI GLOBALI TEMPERATURA / UMIDITÀ
// ============================================================
// Definite in dht_manager.cpp, accessibili a tutto il progetto.
volatile extern float tempInt;
volatile extern float humInt;
volatile extern float tempExt;
volatile extern float humExt;

// Stato disponibilità sensori: false se il sensore è assente (NAN),
// non ancora letto o in quarantena (5 errori consecutivi → 10 min).
volatile extern bool dhtIntValid;
volatile extern bool dhtExtValid;

// ============================================================
// DIAGNOSTICA DHT
// ============================================================
// Definite in dht_manager.cpp, accessibili a tutto il progetto.
volatile extern uint32_t dhtReads;        // numero totale letture tentate
volatile extern uint32_t dhtReadFailures; // letture fallite (NAN su entrambi i canali)
volatile extern uint32_t dhtMaxReadUs;    // durata massima singola lettura (µs)

// Inizializzazione sensori DHT (interno ed esterno)
void initDHT();

// Lettura temperatura/umidità sensore esterno (AM2301 su GPIO 14)
bool readDHTExternal(float &temperature, float &humidity);

// Lettura temperatura/umidità sensore interno (DHT21 su GPIO 4)
bool readDHTInternal(float &temperature, float &humidity);

// Aggiornamento tempExt/humExt/tempInt/humInt con letture DHT
// Mantiene la logica !isnan: aggiorna solo i valori validi.
void updateDHT();

#endif // DHT_MANAGER_H
