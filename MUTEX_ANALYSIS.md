# Analisi Mutex del Progetto Camper

## Sommario
Il progetto utilizza **4 mutex** principali per la sincronizzazione tra i task FreeRTOS.

---

## 1. sdMutex (SemaphoreHandle_t)

### Dove viene creato
- **File:** `src/main.cpp`
- **Linea:** 1184 (nella funzione `setup()`)
- **Codice:** `sdMutex = xSemaphoreCreateMutex();`

### Chi lo utilizza
| Task/Modulo | File | Funzione | Linea |
|-------------|------|----------|-------|
| initSD() | main.cpp | initSD() | 360 |
| logCrash() | crash_logger.cpp | logCrash() | 54 |
| saveGasToSD() | gas_manager.cpp | saveGasToSD() | 123 |
| loadGasFromSD() | gas_manager.cpp | loadGasFromSD() | 154 |
| saveLevelZeroToSD() | level_manager.cpp | saveLevelZeroToSD() | 74 |
| loadLevelZeroFromSD() | level_manager.cpp | loadLevelZeroFromSD() | 94 |
| clearLevelZeroOnSD() | level_manager.cpp | clearLevelZeroOnSD() | 119 |
| saveLastLitersToSD() | water_manager.cpp | saveLastLitersToSD() | 25 |
| loadLastLitersFromSD() | water_manager.cpp | loadLastLitersFromSD() | 56 |
| saveGraphToSD() | graph_storage.cpp | saveGraphToSD() | 14 |
| restoreGraphFromSD() | graph_storage.cpp | restoreGraphFromSD() | 92 |
| saveHourlyAhToSD() | main.cpp | saveHourlyAhToSD() | 2339 |
| saveAhToSD() | main.cpp | saveAhToSD() | 2364 |
| restoreHourlyAhFromSD() | main.cpp | restoreHourlyAhFromSD() | 2396 |
| restoreAhDayFromSD() | main.cpp | restoreAhDayFromSD() | 2423 |

### Ordine di acquisizione
- **Sempre singolo:** sdMutex viene acquisito e rilasciato in modo indipendente, mai in combinazione con altri mutex.

---

## 2. i2cMutex (SemaphoreHandle_t)

### Dove viene creato
- **File:** `src/main.cpp`
- **Linea:** 1185 (nella funzione `setup()`)
- **Codice:** `i2cMutex = xSemaphoreCreateMutex();`

### Chi lo utilizza
| Task/Modulo | File | Funzione | Linea |
|-------------|------|----------|-------|
| i2cRecoverBus() | main.cpp | i2cRecoverBus() | 604 |
| readDifferentialAverage() | main.cpp | readDifferentialAverage() | 691 |
| readSolarCurrent() | main.cpp | readSolarCurrent() | 796 |
| updateCurrentAndVoltage() | main.cpp | updateCurrentAndVoltage() | 858, 897 |
| initLIS3DH() | main.cpp | initLIS3DH() | 2658 |
| updateLevel() | main.cpp | updateLevel() | 2692 |
| LevelTask() | main.cpp | LevelTask() | 2141, 2164 |
| setup() | main.cpp | setup() | 1344, 1372 |
| captureLevelZero() | level_manager.cpp | captureLevelZero() | 37 |
| /calSolarZero endpoint | main.cpp | handle calSolarZero | 1586 |

### Ordine di acquisizione
- **Sempre singolo:** i2cMutex viene acquisito e rilasciato in modo indipendente.

---

## 3. hxMutex (SemaphoreHandle_t)

### Dove viene creato
- **File:** `src/main.cpp`
- **Linea:** 1186 (nella funzione `setup()`)
- **Codice:** `hxMutex = xSemaphoreCreateMutex();`

### Chi lo utilizza
| Task/Modulo | File | Funzione | Linea |
|-------------|------|----------|-------|
| scaleSafeGetUnits() | gas_manager.cpp | scaleSafeGetUnits() | 42 |
| scaleSafeTare() | gas_manager.cpp | scaleSafeTare() | 50 |

### Ordine di acquisizione
- **Sempre singolo:** hxMutex viene acquisito e rilasciato in modo indipendente.

---

## 4. stateMux (portMUX_TYPE)

### Dove viene creato
- **File:** `src/main.cpp`
- **Linea:** 38
- **Codice:** `portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;`

### Chi lo utilizza
| Task/Modulo | File | Funzione | Linea |
|-------------|------|----------|-------|
| updateGas() | gas_manager.cpp | updateGas() | 229, 238 |
| integrateAh() | main.cpp | integrateAh() | 996 |
| handleSetAh() | main.cpp | handleSetAh() | 1110 |
| handleSetAhMax() | main.cpp | handleSetAhMax() | 1137 |
| handleSetEff() | main.cpp | handleSetEff() | 1152 |

### Ordine di acquisizione
- **Sempre singolo:** stateMux è un portMUX (critical section) e non può essere combinato con altri mutex.

---

## 5. prefsMutex (SemaphoreHandle_t) - **ORFANO**

### Dove viene creato
- **File:** `src/main.cpp`
- **Linea:** 1187 (nella funzione `setup()`)
- **Codice:** `prefsMutex = xSemaphoreCreateMutex();`

### Chi lo utilizza
- **NESSUNO** - Il mutex viene creato ma **mai utilizzato** nel codice.

### Problema
- **Mutex orfano:** viene allocato ma non serve a nulla. Potrebbe essere rimosso per risparmiare memoria.

---

## Analisi Casi di Acquisizione Multipla

### Risultato: **NESSUN CASO DI ACQUISIZIONE MULTIPLA DI MUTEX**

Dopo l'analisi dettagliata, **nessun task acquisisce più di un mutex contemporaneamente**. Tutti i mutex sono acquisiti e rilasciati in modo indipendente.

### Esempi di pattern corretto:
```cpp
// Pattern tipico: acquisisci, usa, rilascia
if (xSemaphoreTake(sdMutex, timeout) == pdTRUE) {
    // operazioni SD
    xSemaphoreGive(sdMutex);
}
```

---

## Analisi Possibili Deadlock

### Risultato: **NESSUN DEADLOCK POTENZIALE RILEVATO**

**Motivi:**
1. **Nessun lock nesting:** Nessun codice acquisisce due mutex nello stesso task
2. **Timeout sempre presente:** Tutti gli `xSemaphoreTake` hanno un timeout definito
3. **Ordine coerente:** Non esiste un ordine di acquisizione da rispettare perché non vi è acquisizione multipla
4. **Critical sections brevi:** Le sezioni critiche sono sempre molto brevi

### Attenzione particolare:
- **updateGas()** in `gas_manager.cpp` (linea 229-244):
  - Prima acquisisce `hxMutex` tramite `scaleSafeGetUnits()`
  - Poi acquisisce `stateMux` tramite `STATE_ENTER()`
  - **MA questi sono mutex diversi tipi** (SemaphoreHandle_t vs portMUX_TYPE)
  - **Non c'è rischio di deadlock** perché:
    - `stateMux` è un portMUX (critical section), non un semaforo FreeRTOS
    - I critical section non possono causare deadlock con i semafori
    - Il `hxMutex` viene rilasciato **prima** di acquisire `stateMux`

- **saveGasToSD()** in `gas_manager.cpp` (linea 113-148):
  - Chiama `loadGasFromSD()` **mentre** `sdMutex` è già acquisito
  - **loadGasFromSD() tenta di acquisire di nuovo sdMutex**
  - **QUESTO È UN PROBLEMA!** Vedasi sotto.

---

## Problema Critico Rilevato: **Ritorno a Sezione Critica (Recursive Lock)**

### Descrizione
In `gas_manager.cpp`, la funzione `saveGasToSD()` acquisisce `sdMutex` e poi chiama `loadGasFromSD()` che tenta di acquisirlo di nuovo.

**File:** `src/gas/gas_manager.cpp`

```cpp
void saveGasToSD(const char* filename, float kg) {
    if (!initSD()) return;
    
    float lastValue = loadGasFromSD(filename);  // ← PROBLEMA: chiama funzione che usa sdMutex
    
    if (!takeMutexWarn(sdMutex, 500, "sd"))     // ← sdMutex già acquisito da loadGasFromSD
        return;
    // ...
}
```

### Impatto
- **Non è un deadlock vero e proprio** perché FreeRTOS non supporta i mutex ricorsivi
- Il secondo `xSemaphoreTake(sdMutex, ...)` **fallirà** se il mutex è già stato acquisito dal task stesso
- Questo causa **scrittura su SD anche quando il valore non è cambiato** (comportamento errato)

### Soluzione consigliata
Rifattorizzare `saveGasToSD()` per non chiamare `loadGasFromSD()` mentre il mutex è acquisito, oppure utilizzare un flag per tracciare lo stato del mutex.

---

## Tabella Riepilogativa

| Mutex | Creato in | Utilizzato da | Acquisizione multipla | Rischio deadlock |
|-------|-----------|---------------|----------------------|------------------|
| sdMutex | main.cpp:1184 | 15 funzioni | **NO** (ma c'è ricorsione) | **BASSO** (vedi problema sopra) |
| i2cMutex | main.cpp:1185 | 8 funzioni | NO | Nessuno |
| hxMutex | main.cpp:1186 | 2 funzioni | NO | Nessuno |
| stateMux | main.cpp:38 | 4 funzioni | NO (portMUX) | Nessuno |
| prefsMutex | main.cpp:1187 | **NESSUNO** | NO | Nessuno (orfano) |

---

## Raccomandazioni

1. **Rimuovere prefsMutex** - Non viene mai utilizzato
2. **Correggere saveGasToSD()** - Evitare di chiamare loadGasFromSD() mentre sdMutex è acquisito
3. **Considerare l'uso di mutex ricorsivi** - Se necessario, usare `xSemaphoreCreateRecursiveMutex()`
4. **Aggiungere documentazione** - Commenti su quali variabili sono protette da ogni mutex