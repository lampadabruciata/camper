#ifndef DHT_MANAGER_H
#define DHT_MANAGER_H

// ============================================================
// GESTIONE SENSORI DHT (libreria DHTNEW)
// ============================================================
// Questo modulo pilota i due sensori (AM2301 esterno, DHT interno) con la
// libreria DHTNEW di Rob Tillaart, vendorizzata in lib/DHTNEW/ e adottata
// come libreria di lettura in esercizio al posto del fork Adafruit DHT
// (lib/DHT/, ancora presente e NON modificato come possibile rollback).
//
// Storia: la migrazione a DHTNEW è stata introdotta il 2026-09-17 come prova
// sul campo e si è chiusa con esito positivo (vedi AI_PROJECT_MEMORY.md);
// è oggi la libreria di lettura attiva. Il fork resta disponibile solo come
// riferimento/rollback e non è più percorso dal codice.
//
// INVARIATO rispetto al fork: array dhtSensors[], indice dei canali,
// campi valid/temperature/humidity, contatori reads/failures, quarantena
// (soglia e durata), task e watchdog.
//
// NON alimentato da DHTNEW (i campi restano nel modello dati
// per compatibilità con main.cpp, crash_logger e /api/boot, ma rimangono
// INERTI a 0 perché DHTNEW non li espone): FrameDiag e i contatori di errore
// per categoria. Lo stato grezzo della libreria è comunque esposto da
// dhtLastStatus() / dhtFailureLabel().
// ============================================================

#include "dhtnew.h"
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
  DHTNEW dht;        // istanza fisica del sensore (libreria DHTNEW)
  bool  readEnabled; // false = letture fisiche sospese per questo canale
                     // (guardia riutilizzabile; in esercizio entrambi i canali sono true)

  volatile float temperature; // °C — NAN se mai letto o non valido
  volatile float humidity;    // %  — NAN se mai letto o non valido

  // valid: true = ultimo dato utilizzabile. Un ciclo di lettura fallito NON
  // lo abbassa più immediatamente: temperature/humidity conservano l'ultimo
  // valore valido e il flag resta true entro la finestra
  // DHT_VALIDITY_TIMEOUT_MS (60 s, definita in dht_manager.cpp) dall'ultima
  // lettura valida (lastValidMs); cade solo oltre quella soglia (o subito se
  // lastValidMs == 0, sensore mai letto). Una nuova lettura valida lo
  // ripristina immediatamente.
  volatile bool  valid;

  uint32_t failCount;        // errori consecutivi correnti (quarantena)
  uint32_t quarantinedUntil; // millis() di fine quarantena (0 = non in quarantena)

  // ---- Telemetria diagnostica per-canale (solo osservabilità) ----
  uint32_t reads;           // tentativi di lettura fisica per questo canale
  uint32_t failures;        // letture non valide cumulative per questo canale
  uint32_t quarantineCount; // n° di ingressi in quarantena da boot
  uint32_t maxReadUs;       // durata massima singola lettura (µs)
  uint32_t lastValidMs;     // millis() dell'ultima lettura valida (0 = mai)

  // ---- Distribuzione errori per categoria (solo osservabilità) ----
  // Contatori cumulativi da boot ereditati dal fork, dove erano attribuiti
  // con DHT::lastFailure() nel momento della lettura fisica fallita
  // (etichette di dhtFailureLabel()).
  // Sono pura telemetria per-canale: non influenzano lettura, timeout,
  // retry, quarantena o watchdog.
  // INERTI con DHTNEW: la libreria non espone le categorie di
  // errore del fork, quindi questi contatori restano a 0. Per l'esito grezzo
  // della lettura fisica usare dhtLastStatus()/dhtFailureLabel().
  uint32_t pulseLowTimeouts;  // timeout sul pulse di start LOW (~80 us)
  uint32_t pulseHighTimeouts; // timeout sul pulse di start HIGH (~80 us)
  uint32_t frameTimeouts;     // superato il budget temporale del frame
  uint32_t bitTimeouts;       // timeout su uno dei 40 bit di dati
  uint32_t checksumErrors;    // frame completo ma checksum non coerente

  // ---- Dettaglio in-frame dell'ULTIMO tentativo fisico (solo osservabilità) ----
  // INERTI con DHTNEW: la libreria non espone FrameDiag,
  // questi campi restano a 0 e non vengono più alimentati.
  // Nel fork erano copiati da DHT::lastFrameDiag() a ogni lettura fisica:
  // descrivevano COSA è avvenuto DENTRO il frame, non solo come è stato
  // classificato. Servivano perché FRAME_TIMEOUT e BIT_TIMEOUT possono
  // descrivere lo stesso evento fisico (linea senza transizioni): il primo se
  // il tetto dei 20 ms scatta prima del controllo di fine loop, il secondo se
  // i timeout cadono in coda al frame.
  //   - linea statica/morta  -> molti timeout, pochi pulse misurati;
  //   - bitstream lento/desincronizzato -> 80 pulse, 0 timeout, durate anomale.
  uint16_t lastFramePulses;       // pulse di dato misurati (0-80)
  uint16_t lastFrameLowTimeouts;  // attese LOW scadute (linea letta sempre LOW)
  uint16_t lastFrameHighTimeouts; // attese HIGH scadute (linea letta sempre HIGH)
  uint16_t lastFrameMaxPulseUs;   // durata massima pulse di dato (µs)

  // ---- Cumulativi in-frame da boot (solo osservabilità) ----
  // INERTI con DHTNEW (restano a 0), come i campi sopra.
  // Nel fork erano la somma dei valori precedenti su tutte le letture fisiche.
  uint32_t frameLowTimeoutPulses;  // totale attese LOW scadute (cumulativo)
  uint32_t frameHighTimeoutPulses; // totale attese HIGH scadute (cumulativo)
  uint32_t maxPulseUs;             // massimo storico pulse di dato (µs)
} dhtSensor_t;

// Canali sensori DHT (indici dell'array dhtSensors[])
enum DhtChannel {
  DHT_CHANNEL_EXTERNAL = 0, // AM2301 (DHT21) su GPIO 14
  DHT_CHANNEL_INTERNAL,     // DHT21 su GPIO 13
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

// Etichetta diagnostica del canale ("DHT_INT" / "DHT_EXT")
const char* dhtChannelLabel(DhtChannel ch);

// Etichetta leggibile dello stato restituito da DHTNEW::read() (codici DHTLIB_*).
// Mappa i codici della libreria sui nomi già usati dalla telemetria:
// "NONE" (DHTLIB_OK), "CHECKSUM_ERROR", "PULSE_LOW_TIMEOUT" (TIMEOUT_A),
// "PULSE_HIGH_TIMEOUT" (TIMEOUT_B), "BIT_TIMEOUT" (TIMEOUT_C/D),
// "BIT_SHIFT_ERROR", "SENSOR_NOT_READY", "WAITING_FOR_READ",
// "HUMIDITY_OUT_OF_RANGE", "TEMPERATURE_OUT_OF_RANGE", "NOT_READ", "UNKNOWN".
// Telemetria read-only: non influenza la logica di lettura.
const char* dhtFailureLabel(int status);

// Ultimo stato restituito da DHTNEW::read() per il canale (0 = DHTLIB_OK).
// DHTNEW non espone un getter dello stato (read() lo restituisce una volta
// sola): dht_manager.cpp lo memorizza al momento della lettura fisica.
// Usato da /api/boot (chiave "lastStatus"). Telemetria read-only.
int dhtLastStatus(DhtChannel ch);

// Telemetria: età dell'ultimo dato valido in secondi (-1 se il canale è non
// valido: mai letto oppure oltre la finestra di validità). Dentro la finestra
// riporta l'età reale (0..60 s), anche dopo letture fallite o in quarantena:
// è la stessa grandezza usata per far scadere il flag valid.
// Campo derivato, non memorizzato nel modello dati.
int32_t dhtAgeSec(const dhtSensor_t &s);

// Inizializzazione sensori DHT (interno ed esterno)
void initDHT();

// Aggiornamento temperature/umidità di entrambi i canali con
// gestione quarantena. Mantiene la logica !isnan: aggiorna solo
// i valori validi. Eseguito SOLO da DHTTask (mai da SensorLoop).
// Il flag valid di ogni canale segue la finestra di validità:
// resta true fino a DHT_VALIDITY_TIMEOUT_MS (60 s) dall'ultima
// lettura valida, anche in caso di errori o durante la quarantena.
void updateDHT();

#endif // DHT_MANAGER_H
