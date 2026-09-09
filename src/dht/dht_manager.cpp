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
// DIAGNOSTICA DHT
// ============================================================
// Definite qui; dichiarate in dht_manager.h.
volatile uint32_t dhtReads        = 0;
volatile uint32_t dhtReadFailures = 0;
volatile uint32_t dhtMaxReadUs    = 0;

// ============================================================
// MODELLO DATI UNIFICATO DHT
// ============================================================
// Definizione dei due canali sensore come modello dati unico.
// - Esterno: AM2301 (DHT21) su GPIO 14, letture attive.
// - Interno: DHT21 su GPIO 4, letture SOSPESE ([DIAG-WDT] indagine
//   INT_WDT): readEnabled=false → letture sempre non valide → dopo
//   DHT_QUARANTINE_ERRORS errori entra in quarantena → fallback "--".
// Inizializzati nell'ordine dei campi di dhtSensor_t:
// dht, readEnabled, temperature, humidity, valid, failCount, quarantinedUntil,
// reads, failures, quarantineCount, maxReadUs, lastValidMs.
dhtSensor_t dhtSensors[DHT_CHANNEL_COUNT] = {
  { DHT(AM2301_PIN, DHT21),  true,  NAN, NAN, false, 0, 0, 0, 0, 0, 0, 0 },
  { DHT(DHT_INT_PIN, DHT21), true,  NAN, NAN, false, 0, 0, 0, 0, 0, 0, 0 }
};

// ============================================================
// DHT MANAGER
// ============================================================
// Il modulo possiede tutta la logica DHT: inizializzazione,
// lettura periodica e gestione errori (quarantena). L'unico
// percorso di lettura è DHTTask tramite updateDHT(), mai
// SensorLoop (nessuna lettura DHT nel percorso critico).
// ============================================================

// ============================================================
// INIZIALIZZAZIONE DHT
// ============================================================
void initDHT() {
  // ----------- AM2301 esterno -----------
  dhtSensors[DHT_CHANNEL_EXTERNAL].dht.begin();
  Serial.println("AM2301 OK");

  // ----------- DHT interno -----------
  dhtSensors[DHT_CHANNEL_INTERNAL].dht.begin();
  Serial.println("DHT interno OK");
}

// ============================================================
// LETTURA FISICA DI UN SINGOLO CANALE DHT
// ============================================================
// Unifica le precedenti readDHTExternal/readDHTInternal.
// Legge temperatura/umidità dal sensore fisico del canale.
// Imposta i parametri di output con i valori letti (anche NAN)
// e restituisce true se almeno una lettura è valida. La logica
// di aggiornamento (controlli !isnan in updateDhtChannel) resta
// invariata e aggiorna solo i valori validi.
static bool readSensor(dhtSensor_t &sensor, float &temperature, float &humidity) {
  // [DIAG-WDT] DHT interno TEMPORANEAMENTE DISABILITATO per indagine INT_WDT.
  // Nessuna lettura fisica su GPIO 4: updateDHT() scarta i valori tramite
  // i consueti controlli isnan e il canale conserva l'ultimo valore valido
  // (in pratica resta NAN → quarantena → fallback "--").
  // PER RIPRISTINARE: impostare readEnabled = true per DHT_CHANNEL_INTERNAL.
  if (!sensor.readEnabled) {
    temperature = NAN;
    humidity    = NAN;
    return false;
  }

  uint32_t start = micros();

  temperature = sensor.dht.readTemperature();
  humidity    = sensor.dht.readHumidity();

  uint32_t elapsed = micros() - start;
  if (elapsed > dhtMaxReadUs) dhtMaxReadUs = elapsed;
  if (elapsed > sensor.maxReadUs) sensor.maxReadUs = elapsed;

  return !isnan(temperature) || !isnan(humidity);
}

// ============================================================
// QUARANTENA DHT
// ============================================================
// Se un sensore produce letture non valide (NAN) per
// DHT_QUARANTINE_ERRORS cicli consecutivi, le letture fisiche
// vengono sospese per DHT_QUARANTINE_DURATION_MS (backoff).
// Allo scadere viene eseguito un nuovo tentativo.
// Lo stato quarantena è parte del modello dati (dhtSensor_t).
#define DHT_QUARANTINE_ERRORS      5U                     // 5 errori consecutivi
// [DIAG-TEMP] Durata quarantena ridotta a 1 minuto (60.000 ms) per le prove
// sul banco. PER RIPRISTINARE a 10 minuti: (10UL * 60UL * 1000UL).
#define DHT_QUARANTINE_DURATION_MS (1UL * 60UL * 1000UL)  // 1 minuto (60.000 ms)

// ============================================================
// AGGIORNAMENTO SINGOLO CANALE DHT (con quarantena)
// ============================================================
// - In quarantena: nessuna lettura fisica finché non scade il periodo.
// - Lettura non valida: incrementa gli errori consecutivi; al 5° errore
//   avvia la sospensione (backoff 1 minuto, valore temporaneo di diagnostica).
// - Lettura valida: azzera gli errori e aggiorna SOLO i valori validi
//   (conserva l'ultimo valore letto correttamente).
static void updateDhtChannel(dhtSensor_t &sensor, uint32_t now) {
  // Canale corrente (indice dell'array dhtSensors[]) per i log diagnostici
  // di ingresso/uscita quarantena. DHT_CHANNEL_EXTERNAL="DHT_EXT",
  // DHT_CHANNEL_INTERNAL="DHT_INT" (vedi dhtChannelLabel()).
  const DhtChannel ch = (DhtChannel)(&sensor - dhtSensors);

  // --- Quarantena attiva: nessuna lettura fino alla scadenza ---
  if (sensor.quarantinedUntil != 0) {
    if ((int32_t)(now - sensor.quarantinedUntil) < 0) return;
    sensor.quarantinedUntil = 0;   // scaduta: riprova in questo ciclo
    sensor.failCount        = 0;
    // [DIAG-TEMP] Log uscita quarantena: il canale viene riprovato.
    Serial.print("[");
    Serial.print(dhtChannelLabel(ch));
    Serial.println("] leaving quarantine");
  }

  // --- Lettura fisica (solo qui, mai nel percorso critico) ---
  sensor.reads++;   // telemetria: tentativo di lettura per questo canale
  float t, h;
  bool ok = readSensor(sensor, t, h);

  if (!ok) {
    sensor.valid = false;
    sensor.failures++;   // telemetria: lettura non valida per questo canale
    if (++sensor.failCount >= DHT_QUARANTINE_ERRORS) {
      sensor.failCount        = 0;
      sensor.quarantinedUntil = now;   // inizio periodo di sospensione
      sensor.quarantineCount++;        // telemetria: ingresso in quarantena
      // [DIAG-TEMP] Log ingresso quarantena (durata attuale del backoff).
      Serial.print("[");
      Serial.print(dhtChannelLabel(ch));
      Serial.print("] entering quarantine for ");
      Serial.print(DHT_QUARANTINE_DURATION_MS / 1000UL);
      Serial.println(" s");
    }
    return;
  }

  // --- Lettura valida: azzera errori, aggiorna solo valori validi ---
  sensor.failCount = 0;
  if (!isnan(t)) sensor.temperature = t;
  if (!isnan(h)) sensor.humidity    = h;
  sensor.valid      = true;
  sensor.lastValidMs = now;   // telemetria: timestamp ultima lettura valida
}

// ============================================================
// AGGIORNAMENTO TEMPERATURA / UMIDITÀ
// ============================================================
// Eseguito SOLO da DHTTask (task dedicato a bassa priorità,
// periodo 10 s), mai da SensorLoop: nessuna lettura DHT nel
// percorso critico.
// Aggiorna temperature/umidità di entrambi i canali mantenendo
// la logica !isnan (ultimo valore valido).
void updateDHT() {
  // Contatore diagnostica: un ciclo completo = 2 letture fisiche
  dhtReads++;
  uint32_t now = millis();

  updateDhtChannel(dhtSensors[DHT_CHANNEL_EXTERNAL], now);
  updateDhtChannel(dhtSensors[DHT_CHANNEL_INTERNAL], now);

  // Fallimento ciclo = nessun canale con dati utili
  if (!dhtSensors[DHT_CHANNEL_EXTERNAL].valid &&
      !dhtSensors[DHT_CHANNEL_INTERNAL].valid) {
    dhtReadFailures++;
  }
}

// ============================================================
// HELPER TELEMETRIA DHT
// ============================================================
// Logica condivisa tra /api/boot, saveDiagSnapshot() e logCrash()
// per evitare divergenze nel nome e nei campi derivati dei canali.

const char* dhtChannelLabel(DhtChannel ch) {
  switch (ch) {
    case DHT_CHANNEL_INTERNAL: return "DHT_INT";
    case DHT_CHANNEL_EXTERNAL: return "DHT_EXT";
    default:                   return "DHT_UNKNOWN";
  }
}

int32_t dhtAgeSec(const dhtSensor_t &s) {
  if (!s.valid || s.lastValidMs == 0) return -1;
  return (int32_t)((millis() - s.lastValidMs) / 1000UL);
}
