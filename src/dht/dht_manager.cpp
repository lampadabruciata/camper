#include "dht_manager.h"
#include <Arduino.h>

// ============================================================
// PIN SENSORI FISICI DHT
// ============================================================
// Sensore esterno AM2301 (DHT21)
#define AM2301_PIN 14

// Sensore interno DHT21
#define DHT_INT_PIN 4

// ============================================================
// VARIABILI GLOBALI TEMPERATURA / UMIDITÀ
// ============================================================
// Definite qui; dichiarate in dht_manager.h.
volatile float tempInt = NAN;
volatile float humInt  = NAN;
volatile float tempExt = NAN;
volatile float humExt  = NAN;

// ============================================================
// ISTANZE SENSORI FISICI DHT
// ============================================================
// Il modulo DHT è proprietario dei sensori fisici.
// Dichiarate extern in dht_manager.h.
DHT dhtExt(AM2301_PIN, DHT21);   // AM2301 (DHT21) esterno su GPIO 14
DHT dhtInt(DHT_INT_PIN, DHT21);  // DHT21 interno su GPIO 4

// ============================================================
// DHT MANAGER
// ============================================================
// Questo modulo ospiterà in futuro tutta la logica DHT
// (inizializzazione, lettura periodica, gestione errori).
// Attualmente contiene solo la struttura base: le funzioni
// dichiarate in dht_manager.h verranno implementate qui.
// ============================================================

// ============================================================
// INIZIALIZZAZIONE DHT
// ============================================================
void initDHT() {
  // ----------- AM2301 esterno -----------
  dhtExt.begin();
  Serial.println("AM2301 OK");

  // ----------- DHT interno -----------
  dhtInt.begin();
  Serial.println("DHT interno OK");
}

// ============================================================
// LETTURA DHT ESTERNO (AM2301 su GPIO 14)
// ============================================================
// Legge temperatura e umidità dal sensore esterno.
// Imposta i parametri di output con i valori letti
// (anche NAN) e restituisce true se almeno una lettura
// è valida. La logica di aggiornamento (controlli !isnan
// in DHTTask) resta invariata e aggiorna solo i valori validi.
bool readDHTExternal(float &temperature, float &humidity) {
  temperature = dhtExt.readTemperature();
  humidity    = dhtExt.readHumidity();

  return !isnan(temperature) || !isnan(humidity);
}

// ============================================================
// LETTURA DHT INTERNO (DHT21 su GPIO 4)
// ============================================================
// Legge temperatura e umidità dal sensore interno.
// Imposta i parametri di output con i valori letti
// (anche NAN) e restituisce true se almeno una lettura
// è valida. La logica di aggiornamento (controlli !isnan
// in DHTTask) resta invariata e aggiorna solo i valori validi.
bool readDHTInternal(float &temperature, float &humidity) {
  temperature = dhtInt.readTemperature();
  humidity    = dhtInt.readHumidity();

  return !isnan(temperature) || !isnan(humidity);
}

// ============================================================
// AGGIORNAMENTO TEMPERATURA / UMIDITÀ
// ============================================================
// Legge i sensori DHT esterno ed interno e aggiorna le
// variabili globali tempExt/humExt/tempInt/humInt.
// Mantiene la logica !isnan: aggiorna solo i valori validi.
void updateDHT() {
  float tExt, hExt;
  readDHTExternal(tExt, hExt);

  float tInt, hInt;
  readDHTInternal(tInt, hInt);

  if (!isnan(tExt)) tempExt = tExt;
  if (!isnan(hExt)) humExt = hExt;
  if (!isnan(tInt)) tempInt = tInt;
  if (!isnan(hInt)) humInt = hInt;
}

