#ifndef GAS_MANAGER_H
#define GAS_MANAGER_H

#include <stdint.h>
#include "HX711.h"

// ============================================================
// TELEMETRIA TIMEOUT MUTEX (solo osservabilita diagnostica)
// ============================================================
// Contatori in RAM statica aggiornati da takeMutexWarn() nel solo percorso di
// timeout: nessuna allocazione dinamica, nessuna I/O, nessuna attesa. E' il
// sostituto di logCrash("Mutex timeout"), che allocava heap (String/std::vector)
// proprio mentre il bus era occupato e poteva fallire con std::bad_alloc ->
// std::terminate -> abort (PANIC).
//
// Uno slot per mutex: hx (HX711), sd (SD), i2c (bus I2C) e other (qualunque
// altro nome passato dai chiamanti, NULL compreso).
//
// Campi per slot:
//   count                timeout cumulativi da boot
//   consecutive          timeout consecutivi (azzerato a ogni presa riuscita)
//   maxConsec            massimo storico di consecutive
//   lastMs               millis() dell'ultimo timeout (0 = mai avvenuto)
//   countDuringGasChange timeout caduti nella finestra di cambio bombola
//
// Esposizione: saveDiagSnapshot() in /last_diag.txt (righe hx_to,
// hx_to_maxConsec, hx_to_ageSec, hx_to_duringGasChange, sd_to, sd_to_ageSec,
// i2c_to, other_to).
enum MutexSlot {
  MUTEX_SLOT_HX = 0, // mutex HX711   ("hx")
  MUTEX_SLOT_SD,     // mutex SD      ("sd")
  MUTEX_SLOT_I2C,    // mutex bus I2C ("i2c")
  MUTEX_SLOT_OTHER,  // qualsiasi altro nome (o NULL)
  MUTEX_SLOT_COUNT
};

typedef struct {
  uint32_t count;                // timeout cumulativi da boot
  uint32_t consecutive;          // timeout consecutivi correnti
  uint32_t maxConsec;            // massimo storico di timeout consecutivi
  uint32_t lastMs;               // millis() dell'ultimo timeout (0 = mai)
  uint32_t countDuringGasChange; // timeout nella finestra di cambio bombola
} mutexTimeoutStats_t;

// Un elemento per slot (indice = enum MutexSlot). Definita in gas_manager.cpp.
extern mutexTimeoutStats_t mutexTimeoutStats[MUTEX_SLOT_COUNT];

// Secondi trascorsi dall'ultimo timeout dello slot; -1 se mai avvenuto.
// Solo aritmetica su millis(): nessuna allocazione, nessuna I/O.
long mutexTimeoutAgeSec(uint8_t slot);

// ---- Finestra "cambio bombola" ----
// handleManualGas1()/handleManualGas2() (main.cpp) impostano la scadenza a
// millis() + GAS_MANUAL_CHANGE_WINDOW_MS: copre tare + get_units(10)
// (~2 s di mutex HX711 occupato) e i cicli di lettura immediatamente
// successivi. takeMutexWarn() usa la finestra solo per classificare i timeout
// (countDuringGasChange): non cambia timeout, retry o recovery.
#define GAS_MANUAL_CHANGE_WINDOW_MS 10000UL
extern unsigned long gasManualChangeUntilMs; // 0 = finestra mai aperta

// Salvataggio gas su SD
void saveGasToSD(const char* filename, float kg);

// Caricamento gas da SD
float loadGasFromSD(const char* filename);

// Ripristino gas da SD
void restoreGasFromSD();

// Wrapper thread-safe per HX711
bool scaleSafeGetUnits(HX711 &scale, long samples, float &out);
bool scaleSafeTare(HX711 &scale);

// Aggiornamento gas
void updateGas(HX711 &scale,
               float &gasInitial,
               float &gasConsumed,
               float &gasResidual,
               float &lastReading,
               const char* filename,
               unsigned long &lastSaveTs,
               volatile bool &forceSaveFlag,
               bool &scaleReady);

#endif // GAS_MANAGER_H