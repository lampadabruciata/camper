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

// Disponibilità sensori (false = assente, NAN o in quarantena)
volatile bool dhtIntValid = false;
volatile bool dhtExtValid = false;

// ============================================================
// DIAGNOSTICA DHT
// ============================================================
// Definite qui; dichiarate in dht_manager.h.
volatile uint32_t dhtReads        = 0;
volatile uint32_t dhtReadFailures = 0;
volatile uint32_t dhtMaxReadUs    = 0;

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
  uint32_t start = micros();

  temperature = dhtExt.readTemperature();
  humidity    = dhtExt.readHumidity();

  uint32_t elapsed = micros() - start;
  if (elapsed > dhtMaxReadUs) dhtMaxReadUs = elapsed;

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
  // [DIAG-WDT] DHT interno TEMPORANEAMENTE DISABILITATO per indagine INT_WDT.
  // Nessuna lettura fisica su GPIO 4. updateDHT() scarta i valori tramite
  // i consueti controlli isnan: tempInt/humInt conservano l'ultimo valore
  // valido. PER RIPRISTINARE: rimuovere questo blocco e riattivare (#if 1)
  // il corpo originale sottostante.
  temperature = NAN;
  humidity    = NAN;
  return false;

#if 0 // [DIAG-WDT] corpo originale sospeso per il test
  uint32_t start = micros();

  temperature = dhtInt.readTemperature();
  humidity    = dhtInt.readHumidity();

  uint32_t elapsed = micros() - start;
  if (elapsed > dhtMaxReadUs) dhtMaxReadUs = elapsed;

  return !isnan(temperature) || !isnan(humidity);
#endif // [DIAG-WDT] fine corpo originale sospeso
}

// ============================================================
// QUARANTENA DHT
// ============================================================
// Se un sensore produce letture non valide (NAN) per
// DHT_QUARANTINE_ERRORS cicli consecutivi, le letture fisiche
// vengono sospese per DHT_QUARANTINE_DURATION_MS (backoff).
// Allo scadere viene eseguito un nuovo tentativo.
#define DHT_QUARANTINE_ERRORS      5U                     // 5 errori consecutivi
#define DHT_QUARANTINE_DURATION_MS (10UL * 60UL * 1000UL) // 10 minuti

typedef struct {
  uint32_t failCount;        // errori consecutivi correnti
  uint32_t quarantinedUntil; // millis() di fine quarantena (0 = non in quarantena)
} dhtQuarantine_t;

static dhtQuarantine_t qExt = {0, 0};
static dhtQuarantine_t qInt = {0, 0};

// ============================================================
// AGGIORNAMENTO SINGOLO CANALE DHT (con quarantena)
// ============================================================
// - In quarantena: nessuna lettura fisica finché non scade il periodo.
// - Lettura non valida: incrementa gli errori consecutivi; al 5° errore
//   avvia la sospensione (backoff 10 minuti).
// - Lettura valida: azzera gli errori e aggiorna SOLO i valori validi
//   (conserva l'ultimo valore letto correttamente).
static void updateDhtChannel(
    dhtQuarantine_t &q,
    bool (*readFn)(float &, float &),
    volatile float &tempOut,
    volatile float &humOut,
    volatile bool  &validOut,
    uint32_t now)
{
  // --- Quarantena attiva: nessuna lettura fino alla scadenza ---
  if (q.quarantinedUntil != 0) {
    if ((int32_t)(now - q.quarantinedUntil) < 0) return;
    q.quarantinedUntil = 0;   // scaduta: riprova in questo ciclo
    q.failCount        = 0;
  }

  // --- Lettura fisica (solo qui, mai nel percorso critico) ---
  float t, h;
  bool ok = readFn(t, h);

  if (!ok) {
    validOut = false;
    if (++q.failCount >= DHT_QUARANTINE_ERRORS) {
      q.failCount        = 0;
      q.quarantinedUntil = now;   // inizio periodo di sospensione
    }
    return;
  }

  // --- Lettura valida: azzera errori, aggiorna solo valori validi ---
  q.failCount = 0;
  if (!isnan(t)) tempOut = t;
  if (!isnan(h)) humOut = h;
  validOut = true;
}

// ============================================================
// AGGIORNAMENTO TEMPERATURA / UMIDITÀ
// ============================================================
// Eseguito SOLO da DHTTask (task dedicato a bassa priorità,
// periodo 10 s), mai da SensorLoop: nessuna lettura DHT nel
// percorso critico.
// Aggiorna tempExt/humExt/tempInt/humInt mantenendo la logica
// !isnan (ultimo valore valido).
void updateDHT() {
  // Contatore diagnostica: un ciclo completo = 2 letture fisiche
  dhtReads++;
  uint32_t now = millis();

  updateDhtChannel(qExt, readDHTExternal, tempExt, humExt, dhtExtValid, now);
  updateDhtChannel(qInt, readDHTInternal, tempInt, humInt, dhtIntValid, now);

  // Fallimento ciclo = nessun canale con dati utili
  if (!dhtExtValid && !dhtIntValid) {
    dhtReadFailures++;
  }
}