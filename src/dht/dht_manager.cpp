#include "dht_manager.h"
#include <Arduino.h>

// ============================================================
// PIN SENSORI FISICI DHT
// ============================================================
// Sensore esterno AM2301 (DHT21)
#define AM2301_PIN 14

// Sensore interno DHT21
// DHT interno su GPIO 13 (pin in esercizio dal test di isolamento del
// 2026-09-16: catena GPIO 4 nominale -> GPIO 2 intermedio -> GPIO 13).
// Il pin è stato liberato spostando il pulsante di cambio pagina da GPIO 13
// a GPIO 4 in main.cpp, così il ramo DHT interno resta su un GPIO semplice.
#define DHT_INT_PIN 13

// ============================================================
// TIPO SENSORE PER DHTNEW
// ============================================================
// 22 = famiglia DHT22/DHT33/DHT44: protocollo e formato dati identici a
// DHT21/AM2301 (16 bit umidità + 16 bit temperatura firmata, scala 0.1).
// Il tipo va forzato in initDHT(): con tipo 0 (default) la prima read()
// entrerebbe in autodetect, con attesa bloccante di 1 s e fino a 3 letture.
#define DHTNEW_SENSOR_TYPE 22

// ============================================================
// DIAGNOSTICA DHT
// ============================================================
// Definite qui; dichiarate in dht_manager.h.
volatile uint32_t dhtReads        = 0;
volatile uint32_t dhtReadFailures = 0;
volatile uint32_t dhtMaxReadUs    = 0;

// Ultimo stato restituito da DHTNEW::read() per canale (0 = DHTLIB_OK).
// DHTNEW non espone un getter dello stato: read() lo restituisce una volta
// sola, quindi dht_manager lo conserva qui per la telemetria (/api/boot).
// Valore iniziale DHT_STATUS_NOT_READ = nessuna lettura fisica eseguita.
// Scritto SOLO da DHTTask (readSensor), letto dai task di telemetria: int
// allineato => accesso atomico, nessun lock necessario.
#define DHT_STATUS_NOT_READ 1
static volatile int dhtLastStatusByChannel[DHT_CHANNEL_COUNT] = {
  DHT_STATUS_NOT_READ, DHT_STATUS_NOT_READ
};

// ============================================================
// MODELLO DATI UNIFICATO DHT
// ============================================================
// Definizione dei due canali sensore come modello dati unico.
// - Esterno: AM2301 (DHT21) su GPIO 14, letture attive (readEnabled=true).
// - Interno: DHT21 su GPIO 13 (pin operativo dal test di isolamento del 2026-09-16;
//   il pin nominale precedente era GPIO 4), letture attive (readEnabled=true).
// Inizializzati nell'ordine dei campi di dhtSensor_t:
// dht, readEnabled, temperature, humidity, valid, failCount, quarantinedUntil,
// reads, failures, quarantineCount, maxReadUs, lastValidMs,
// pulseLowTimeouts, pulseHighTimeouts, frameTimeouts, bitTimeouts, checksumErrors,
// lastFramePulses, lastFrameLowTimeouts, lastFrameHighTimeouts, lastFrameMaxPulseUs,
// frameLowTimeoutPulses, frameHighTimeoutPulses, maxPulseUs.
//
// Il costruttore DHTNEW(pin) memorizza il pin e, come effetto
// collaterale documentato dalla libreria (reset()), esegue pinMode(OUTPUT) +
// digitalWrite(HIGH) sulla linea dati durante la costruzione statica (prima
// di setup()). La cosa è innocua qui: la linea dati è comunque idle-high e
// ogni read() riconfigura il pin da sé (OUTPUT/LOW → INPUT_PULLUP). Il tipo
// sensore è impostato in initDHT() con setType(DHTNEW_SENSOR_TYPE).
dhtSensor_t dhtSensors[DHT_CHANNEL_COUNT] = {
  { DHTNEW(AM2301_PIN),  true,  NAN, NAN, false, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
  { DHTNEW(DHT_INT_PIN), true,  NAN, NAN, false, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
};

// ============================================================
// DHT MANAGER
// ============================================================
// Il modulo possiede tutta la logica DHT: inizializzazione,
// lettura periodica e gestione errori (quarantena). L'unico
// percorso di lettura è DHTTask tramite updateDHT(), mai
// SensorLoop (nessuna lettura DHT nel percorso critico).
// La transazione fisica sul bus è affidata alla
// libreria DHTNEW (lib/DHTNEW/) al posto del fork DHT.
// ============================================================

// ============================================================
// INIZIALIZZAZIONE DHT
// ============================================================
void initDHT() {
  // ----------- AM2301 esterno (GPIO 14) -----------
  // DHTNEW non ha begin(): si forza il tipo sensore (evita l'autodetect
  // bloccante alla prima read()) e si lascia la linea dati in pull-up, la
  // stessa condizione di riposo creata da DHT::begin() del fork.
  dhtSensors[DHT_CHANNEL_EXTERNAL].dht.setType(DHTNEW_SENSOR_TYPE);
  // Non bloccante come il fork: se la read() arriva prima del readDelay la
  // libreria restituisce DHTLIB_WAITING_FOR_READ invece di attendere in loop
  // (DHTTask non deve mai bloccarsi più del tempo della singola transazione).
  dhtSensors[DHT_CHANNEL_EXTERNAL].dht.setWaitForReading(false);
  pinMode(AM2301_PIN, INPUT_PULLUP);
  Serial.println("AM2301 OK (DHTNEW)");

  // ----------- DHT interno (GPIO 13) -----------
  dhtSensors[DHT_CHANNEL_INTERNAL].dht.setType(DHTNEW_SENSOR_TYPE);
  dhtSensors[DHT_CHANNEL_INTERNAL].dht.setWaitForReading(false);
  pinMode(DHT_INT_PIN, INPUT_PULLUP);
  Serial.println("DHT interno OK (DHTNEW)");
}

// ============================================================
// CONTEGGIO ERRORI DHT PER CATEGORIA (solo telemetria)
// ============================================================
// Rimosso countDhtFailure() del fork: DHTNEW non espone le
// categorie DHT::DhtFailure, quindi i contatori per categoria
// (pulseLowTimeouts/pulseHighTimeouts/frameTimeouts/bitTimeouts/
// checksumErrors) restano INERTI a 0. L'esito grezzo dell'ultima lettura
// fisica resta comunque leggibile per canale con dhtLastStatus() e
// dhtFailureLabel(). Nessun'altra logica (lettura, timeout, retry,
// quarantena, watchdog) dipende da quei contatori.

// ============================================================
// LETTURA FISICA DI UN SINGOLO CANALE DHT (libreria DHTNEW)
// ============================================================
// Unifica le precedenti readDHTExternal/readDHTInternal.
// Una sola transazione DHTNEW::read() per canale: la libreria restituisce lo
// stato della lettura fisica, poi i valori si prelevano con
// getTemperature()/getHumidity(). I valori vengono prelevati SOLO se lo stato
// è DHTLIB_OK, perché in caso di errore DHTNEW li valorizza a
// DHTLIB_INVALID_VALUE (-999) e non a NAN: così il contratto !isnan usato da
// updateDhtChannel() resta identico a quello del fork.
// Imposta i parametri di output con i valori letti (anche NAN)
// e restituisce true se almeno una lettura è valida. La logica
// di aggiornamento (controlli !isnan in updateDhtChannel) resta
// invariata e aggiorna solo i valori validi.
static bool readSensor(dhtSensor_t &sensor, float &temperature, float &humidity) {
  // Guardia generica per canale (non più usata per disattivare il DHT interno):
  // con readEnabled=false non si accede al bus fisico e i parametri di output
  // restano NAN, quindi updateDHT() scarta i valori tramite i consueti
  // controlli isnan e il canale conserva l'ultimo valore valido (in pratica
  // entra in quarantena → fallback "--"). Entrambi i canali hanno attualmente
  // readEnabled=true; la guardia resta come protezione riutilizzabile.
  if (!sensor.readEnabled) {
    temperature = NAN;
    humidity    = NAN;
    return false;
  }

  const DhtChannel ch = (DhtChannel)(&sensor - dhtSensors);

  uint32_t start = micros();

  // --- Lettura fisica (una sola transazione DHTNEW per canale) ---
  // DHTNEW::read() restituisce un codice DHTLIB_*:
  //   0 (DHTLIB_OK)                  lettura completa e checksum coerente
  //   DHTLIB_WAITING_FOR_READ (-8)   nessuna lettura fisica: è trascorso meno
  //                                  del readDelay (2000 ms) dalla precedente
  //   < 0                            errore fisico (timeout/checksum/bit shift)
  // Col periodo del DHTTask (10 s) il caso WAITING_FOR_READ può capitare solo
  // al primissimo ciclo se il boot è più rapido di 2 s: viene trattato come
  // lettura non valida, come già accadeva con un NAN del fork.
  const int status = sensor.dht.read();
  dhtLastStatusByChannel[ch] = status;   // telemetria read-only

  if (status == DHTLIB_OK) {
    temperature = sensor.dht.getTemperature();
    humidity    = sensor.dht.getHumidity();
  } else {
    // Nessun dato utile: NAN preserva la semantica !isnan del chiamante
    // (l'ultimo valore valido resta in sensor.temperature/humidity).
    temperature = NAN;
    humidity    = NAN;
  }

  // Telemetria: durata della lettura fisica (invariata rispetto al fork).
  uint32_t elapsed = micros() - start;
  if (elapsed > dhtMaxReadUs) dhtMaxReadUs = elapsed;
  if (elapsed > sensor.maxReadUs) sensor.maxReadUs = elapsed;

  // Non vengono alimentati i contatori per categoria né il
  // dettaglio in-frame (DHTNEW non espone FrameDiag): restano a 0.

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
// Durata quarantena: 1 minuto (60.000 ms), valore in esercizio. È stato
// adottato in origine per le prove sul banco (il valore precedente era
// 10 minuti: (10UL * 60UL * 1000UL)).
#define DHT_QUARANTINE_DURATION_MS (1UL * 60UL * 1000UL)  // 1 minuto (60.000 ms)

// ============================================================
// FINESTRA DI VALIDITÀ DHT (ultimo valore valido)
// ============================================================
// Un ciclo di lettura fallito non invalida più immediatamente il canale:
// temperature/humidity conservano l'ultimo valore valido e il flag valid
// resta true finché l'età dell'ultimo dato valido (now - lastValidMs) è
// inferiore a questa soglia. Oltre la soglia il canale torna non valido
// (backend /data/live -> null, frontend e OLED "--") finché una nuova
// lettura valida non lo ripristina immediatamente.
// - lastValidMs == 0 (sensore mai letto) -> nessuna finestra: resta false.
// - Vale anche in quarantena (nessuna lettura fisica): la scadenza viene
//   valutata comunque a ogni ciclo del DHTTask (periodo 10 s), quindi il
//   flag cade al primo ciclo con età >= DHT_VALIDITY_TIMEOUT_MS.
// Non modifica quarantena, failCount, reads, failures, lastValidMs,
// temperature e humidity: è solo la condizione di validità del canale.
#define DHT_VALIDITY_TIMEOUT_MS 60000UL  // 60 s

// Validità del canale in funzione dell'età dell'ultimo dato valido.
// Con lastValidMs == 0 (mai letta) restituisce false: nessuna finestra.
// Sottrazione senza segno: corretta anche al wraparound di millis().
static bool dhtValidByAge(uint32_t now, uint32_t lastValidMs) {
  if (lastValidMs == 0) return false;
  return (uint32_t)(now - lastValidMs) < DHT_VALIDITY_TIMEOUT_MS;
}

// ============================================================
// AGGIORNAMENTO SINGOLO CANALE DHT (con quarantena)
// ============================================================
// - In quarantena: nessuna lettura fisica finché non scade il periodo.
// - Lettura non valida: incrementa gli errori consecutivi; al 5° errore
//   avvia la sospensione (backoff 1 minuto, DHT_QUARANTINE_DURATION_MS).
//   Il flag valid NON cade più qui: resta true entro DHT_VALIDITY_TIMEOUT_MS
//   (60 s) dall'ultima lettura valida, anche durante la quarantena.
// - Lettura valida: azzera gli errori e aggiorna SOLO i valori validi
//   (conserva l'ultimo valore letto correttamente).
static void updateDhtChannel(dhtSensor_t &sensor, uint32_t now) {
  // Canale corrente (indice dell'array dhtSensors[]) per i log diagnostici
  // di ingresso/uscita quarantena. DHT_CHANNEL_EXTERNAL="DHT_EXT",
  // DHT_CHANNEL_INTERNAL="DHT_INT" (vedi dhtChannelLabel()).
  const DhtChannel ch = (DhtChannel)(&sensor - dhtSensors);

  // --- Finestra di validità: valutata a ogni ciclo, prima di ogni uscita ---
  // Il flag dipende SOLO dall'età dell'ultimo dato valido: un ciclo fallito
  // non lo abbassa più immediatamente e la scadenza viene applicata anche
  // durante la quarantena, dove non si legge dal bus. Con lastValidMs == 0
  // (sensore mai letto) resta false.
  sensor.valid = dhtValidByAge(now, sensor.lastValidMs);

  // --- Quarantena attiva: nessuna lettura fino alla scadenza ---
  if (sensor.quarantinedUntil != 0) {
    if ((int32_t)(now - sensor.quarantinedUntil) < 0) return;
    sensor.quarantinedUntil = 0;   // scaduta: riprova in questo ciclo
    sensor.failCount        = 0;
    // Log uscita quarantena: il canale viene riprovato.
    Serial.print("[");
    Serial.print(dhtChannelLabel(ch));
    Serial.println("] leaving quarantine");
  }

  // --- Lettura fisica (solo qui, mai nel percorso critico) ---
  sensor.reads++;   // telemetria: tentativo di lettura per questo canale
  float t, h;
  bool ok = readSensor(sensor, t, h);

  if (!ok) {
    // Nessun dato nuovo: temperature/humidity conservano l'ultimo valore
    // valido e valid NON viene abbassato qui — è già stato calcolato per età
    // all'inizio del ciclo e cade solo oltre DHT_VALIDITY_TIMEOUT_MS.
    sensor.failures++;   // telemetria: lettura non valida per questo canale
    if (++sensor.failCount >= DHT_QUARANTINE_ERRORS) {
      sensor.failCount        = 0;
      sensor.quarantinedUntil = now;   // inizio periodo di sospensione
      sensor.quarantineCount++;        // telemetria: ingresso in quarantena
      // Log ingresso quarantena (durata attuale del backoff).
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
  sensor.valid       = true;   // dato fresco: valido subito, nessuna attesa
  sensor.lastValidMs = now;    // timestamp ultima lettura valida (finestra)
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

// Stato dell'ultima lettura fisica DHT in forma leggibile (codici DHTNEW).
// Telemetria read-only: legge solo la copia memorizzata da readSensor(),
// senza toccare lettura, timeout, retry, quarantena o watchdog.
// I nomi mantengono la continuità con la telemetria del fork dove la
// semantica coincide (es. TIMEOUT_A = attesa del pulse di start LOW).
const char* dhtFailureLabel(int status) {
  switch (status) {
    case DHTLIB_OK:                       return "NONE";
    case DHTLIB_ERROR_CHECKSUM:           return "CHECKSUM_ERROR";
    case DHTLIB_ERROR_TIMEOUT_A:          return "PULSE_LOW_TIMEOUT";
    case DHTLIB_ERROR_TIMEOUT_B:          return "PULSE_HIGH_TIMEOUT";
    case DHTLIB_ERROR_TIMEOUT_C:          return "BIT_TIMEOUT";
    case DHTLIB_ERROR_TIMEOUT_D:          return "BIT_TIMEOUT";
    case DHTLIB_ERROR_BIT_SHIFT:          return "BIT_SHIFT_ERROR";
    case DHTLIB_ERROR_SENSOR_NOT_READY:   return "SENSOR_NOT_READY";
    case DHTLIB_WAITING_FOR_READ:         return "WAITING_FOR_READ";
    case DHTLIB_HUMIDITY_OUT_OF_RANGE:    return "HUMIDITY_OUT_OF_RANGE";
    case DHTLIB_TEMPERATURE_OUT_OF_RANGE: return "TEMPERATURE_OUT_OF_RANGE";
    case DHT_STATUS_NOT_READ:             return "NOT_READ";
    default:                              return "UNKNOWN";
  }
}

// Ultimo stato DHTNEW::read() del canale (0 = DHTLIB_OK).
// Telemetria read-only: nessun accesso al bus, nessuna modifica di stato.
int dhtLastStatus(DhtChannel ch) {
  if (ch >= DHT_CHANNEL_COUNT) return DHT_STATUS_NOT_READ;
  return dhtLastStatusByChannel[ch];
}

int32_t dhtAgeSec(const dhtSensor_t &s) {
  if (!s.valid || s.lastValidMs == 0) return -1;
  return (int32_t)((millis() - s.lastValidMs) / 1000UL);
}
