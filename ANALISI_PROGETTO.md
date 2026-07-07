# Report di Analisi - Progetto ESP32 Camper

**Data analisi:** 7 luglio 2026  
**File principale:** `src/main.cpp` (4353 righe)  
**Framework:** Arduino (ESP32)

---

## 1. Moduli Logici Presenti nel Firmware

Il firmware è strutturato in **7 moduli logici principali** (tutto in un unico file `main.cpp`):

| # | Modulo | Descrizione |
|---|--------|-------------|
| 1 | **Configurazione & Hardware Init** | Include, definizioni pin, mutex, hardware initialization |
| 2 | **Gestione Mutex & Thread Safety** | Mutex per I2C, SD, HX711, Preferences + funzione `takeMutexWarn()` |
| 3 | **RTC & Time Management** | RTC DS3231, gestione orario, DST europeo, conversioni epoch |
| 4 | **Sensori Elettrici (ADS1115)** | Lettura corrente batteria, tensione, corrente solare |
| 5 | **Sensori Ambientali** | DHT21 interno/esterno (temperatura/umidità), LIS3DH (livella) |
| 6 | **Gestione Risorse** | Gas (HX711), Acqua (flussometro), Ah integrazione batteria |
| 7 | **Web Server & API** | Web server, endpoint REST, gestione richieste HTTP |

---

## 2. Funzioni Appartenenti a Ciascun Modulo

### Modulo 1: Configurazione & Hardware Init
```
- setup()                           // Inizializzazione completa sistema
- i2cScan()                         // Scansione I2C con riconoscimento componenti
- i2cRecoverBus()                  // Recovery manuale bus I2C
- serialConnected()                 // Controllo seriale connessa
- yesno()                           // Helper boolean to string
```

### Modulo 2: Gestione Mutex & Thread Safety
```
- takeMutexWarn()                   // Wrapper con timeout e logging
- STATE_ENTER() / STATE_EXIT()      // Macro per critical section
```

### Modulo 3: RTC & Time Management
```
- nowEpoch()                        // Epoch corrente da RTC + millis
- updateDST()                       // Calcolo DST europeo
- localToUtc()                      // Conversione orale locale → UTC
- utcToLocal()                      // Conversione UTC → orale locale
- handleSetTime()                   // Endpoint web per impostazione orario
```

### Modulo 4: Sensori Elettrici (ADS1115)
```
- readDifferentialAverage()         // Lettura differenziale con mediana
- readSolarCurrent()                // Lettura corrente pannelli solari
- readBatteryVoltage_singleEnded()    // Lettura tensione batteria
- updateCurrentAndVoltage()         // Aggiornamento corrente e tensione
```

### Modulo 5: Sensori Ambientali
```
- DHTTask()                         // Task lettura DHT interno/esterno
- readTempLiveMinimal()             // Lettura temperatura minima
- readTempInternal()                // Lettura DHT interno
- readTempExternal()                // Lettura DHT esterno
- initLIS3DH()                      // Inizializzazione accelerometro
- updateLevel()                     // Aggiornamento roll/pitch livella
- captureLevelZero()                // Cattura offset livella
- LevelTask()                       // Task dedicato livella
- appendLevelToJson()               // Serializzazione JSON livella
- saveLevelZeroToSD()               // Persistenza offset livella su SD
- loadLevelZeroFromSD()             // Caricamento offset livella da SD
- clearLevelZeroOnSD()              // Cancellazione offset livella
```

### Modulo 6: Gestione Risorse
#### Sottogruppo 6a: Gas (HX711)
```
- scaleSafeGetUnits()               // Wrapper thread-safe HX711
- scaleSafeTare()                   // Tare thread-safe HX711
- saveGasToSD()                     // Salvataggio gas su SD
- loadGasFromSD()                   // Caricamento gas da SD
- restoreGasFromSD()                // Ripristino gas iniziale
- updateGas()                       // Aggiornamento stato bombola
- handleManualGas1()                // Endpoint impostazione gas 1
- handleManualGas2()                // Endpoint impostazione gas 2
```

#### Sottogruppo 6b: Acqua (Flussometro)
```
- pulseCounter()                    // Interrupt contatore impulsi
- saveLastLitersToSD()              // Salvataggio litri su SD
- loadLastLitersFromSD()            // Caricamento litri da SD
- restoreLitersFromSD()             // Ripristino litri iniziale
- handleSetTank()                   // Endpoint impostazione serbatoio
- appendWaterDataToJson()           // Serializzazione JSON acqua
```

#### Sottogruppo 6c: Ah Batteria
```
- restoreAhFromPrefs()              // Ripristino Ah da NVS
- saveAhNowToPrefs()                // Salvataggio Ah ora su NVS
- saveAhConfigToPrefs()             // Salvataggio configurazione Ah
- integrateAh()                     // Integrazione Ah (scarica/carica)
- handleSetAh()                     // Endpoint impostazione Ah
- handleSetAhMax()                  // Endpoint impostazione capacità max
- handleSetEff()                    // Endpoint impostazione efficienza
- appendAhToJson()                  // Serializzazione JSON Ah
- saveHourlyAhToSD()                // Salvataggio orario Ah su SD
- saveAhToSD()                      // Salvataggio giornaliero Ah su SD
- restoreHourlyAhFromSD()           // Ripristino orario Ah da SD
- restoreAhDayFromSD()              // Ripristino giornaliero Ah da SD
- saveAhStateToSD()                 // Salvataggio stato Ah su SD
- restoreAhStateFromSD()            // Ripristino stato Ah da SD
```

### Modulo 7: Web Server & API
```
- handleIndex()                     // Pagina HTML principale
- handleChartJs()                   // Chart.js per grafici
- handleDataLive()                  // API dati live (JSON)
- handleDataBattery()               // API dati batteria (JSON)
- handleDataSolar()                 // API dati solare (JSON)
- handleDataWater()                 // API dati acqua (JSON)
- handleDataGas()                   // API dati gas (JSON)
- handleDataAh24h()                 // API dati Ah 24h (JSON)
- handleDataAhLive()                // API dati Ah live (JSON)
- sendFile()                        // Invio file SPIFFS
```

### Modulo 8: Display OLED
```
- DisplayTask()                     // Task rendering display
- drawOLED()                        // Rendering pagina corrente
- iconBattery()                     // Icona batteria
- iconBatteryLevel()                // Icona batteria con riempimento
- iconWater()                       // Icona acqua
- iconGas()                         // Icona gas
- iconTemp()                        // Icona temperatura
- iconClock()                       // Icona orologio
- drawRightText()                   // Testo allineato a destra
- drawRightPrintf()                 // Printf allineato a destra
```

### Modulo 9: Task di Sistema
```
- SensorLoop()                      // Task principale sensori
- PrefsTask()                       // Task scrittura NVS (queue-based)
- LateRestoreTask()                 // Task ripristino grafico posticipato
- SDWatchdogTask()                  // Task monitor SD card
- logCrash()                        // Logging crash su SD
```

### Modulo 10: Grafici 24h
```
- saveGraphToSD()                   // Salvataggio grafico su SD
- restoreGraphFromSD()              // Ripristino grafico da SD
```

---

## 3. Dipendenze tra Moduli

```
┌─────────────────────┐
│  Config & Hardware  │
└──────────┬──────────┘
           │
           ▼
┌─────────────────────┐
│   Mutex & Safety    │ ◄────────────────────┐
└──────────┬──────────┘                      │
           │                                 │
           ▼                                 │
┌─────────────────────┐                      │
│   RTC & Time Mgmt   │                      │
└──────────┬──────────┘                      │
           │                                 │
           ▼                                 │
┌─────────────────────┐                      │
│  Sensori Elettrici   │                      │
│   (ADS1115)         │                      │
└──────────┬──────────┘                      │
           │                                 │
           ▼                                 │
┌─────────────────────┐                      │
│ Sensori Ambientali  │                      │
│ (DHT + LIS3DH)      │                      │
└──────────┬──────────┘                      │
           │                                 │
           ▼                                 │
┌─────────────────────┐                      │
│  Gestione Risorse   │                      │
│  (Gas + Acqua + Ah) │                      │
└──────────┬──────────┘                      │
           │                                 │
           ▼                                 │
┌─────────────────────┐                      │
│   Web Server & API    │                      │
│                     │                      │
└──────────┬──────────┘                      │
           │                                 │
           ▼                                 │
┌─────────────────────┐                      │
│     Display OLED    │ ◄────────────────────┘
└─────────────────────┘
```

**Dipendenze critiche:**
- **Sensori Elettrici** → dipende da **Mutex & Safety** (I2C)
- **Sensori Ambientali** → dipende da **Mutex & Safety** (I2C)
- **Gestione Risorse** → dipende da **Mutex & Safety** (SD, HX711)
- **Grafici 24h** → dipende da **Mutex & Safety** (SD)
- **Web Server** → legge dati da tutti i moduli
- **Display** → legge dati da tutti i moduli

---

## 4. Variabili Globali Più Utilizzate

### Variabili condivise tra più moduli (alto traffico):

| Variabile | Tipo | Utilizzo | Modulo principale |
|-----------|------|----------|-------------------|
| `vBattery` | `volatile float` | Lettura/Scrittura | Sensori Elettrici, Display, Web |
| `currentFiltered` | `volatile float` | Lettura/Scrittura | Sensori Elettrici, Ah, Display, Web |
| `solarAmps` | `volatile float` | Lettura/Scrittura | Sensori Elettrici, Display, Web |
| `tempInt`, `tempExt` | `volatile float` | Lettura/Scrittura | Sensori Ambientali, Web |
| `gasResidual1`, `gasResidual2` | `float` | Lettura/Scrittura | Gas, Display, Web |
| `remainingLiters` | `volatile float` | Lettura/Scrittura | Acqua, Display, Web |
| `batteryAh_now`, `batteryAh_max` | `float` | Lettura/Scrittura | Ah, Display, Web |
| `currentPage` | `volatile int` | Lettura/Scrittura | Display, Loop (pulsante) |
| `i2cMutex`, `sdMutex`, `hxMutex`, `prefsMutex` | `SemaphoreHandle_t` | Sincronizzazione | Tutti i moduli |

### Variabili critiche per concorrenza:
- `pulseCount` - modificata in interrupt, letta in task
- `lastLevelRequest` - condivisa tra LevelTask, SensorLoop, DisplayTask
- `displayDirty`, `displayNeedUpdate` - flag di sincronizzazione display
- `freezeSensors`, `freezeAhIntegration` - flag di controllo esecuzione

---

## 5. Ordine Consigliato per Estrarre i Moduli in File .h/.cpp

**Ordine di estrazione consigliato (priorità per dipendenze):**

1. **`mutex_utils.h/.cpp`** - Funzioni di utilità mutex, `takeMutexWarn()`, macro STATE_
2. **`time_manager.h/.cpp`** - RTC, DST, conversioni epoch, `nowEpoch()`
3. **`sd_manager.h/.cpp`** - Inizializzazione SD, `initSD()`, `debugSDWriteResult()`
4. **`prefs_task.h/.cpp`** - Task NVS, strutture `PrefsMessage`, `PrefsCmdType`
5. **`sensors_electrical.h/.cpp`** - ADS1115, lettura corrente/tensione, `readDifferentialAverage()`
6. **`sensors_environmental.h/.cpp`** - DHT, LIS3DH, `updateLevel()`, `readTemp*()`
7. **`resource_gas.h/.cpp`** - Gestione bombole HX711, `updateGas()`, `saveGasToSD()`
8. **`resource_water.h/.cpp`** - Flussometro, `pulseCounter()`, `handleSetTank()`
9. **`resource_ah.h/.cpp`** - Integrazione Ah, `integrateAh()`, `saveAh*()`
10. **`display_oled.h/.cpp`** - Rendering display, `drawOLED()`, icone
11. **`web_server.h/.cpp`** - Endpoint web, `handleData*()`, routing
12. **`graph_storage.h/.cpp`** - Grafici 24h, `saveGraphToSD()`, `restoreGraphFromSD()`
13. **`main.cpp`** - Solo `setup()`, `loop()`, task creation, variabili globali

---

## 6. Moduli a Basso Rischio

| Modulo | Motivazione |
|--------|-------------|
| **Display OLED** | Codice puramente grafico, nessuna logica di business, nessuna operazione su bus condiviso |
| **Web Server (API)** | Solo lettura dati esposti, nessuna logica complessa, operazioni non bloccanti |
| **RTC & Time** | Funzioni di utilità, poca logica, nessuna condivisione dati critica |
| **Grafici 24h** | Operazioni su SD già protette da mutex, logica di salvataggio stabile |

---

## 7. Moduli ad Alto Rischio

| Modulo | Motivazione |
|--------|-------------|
| **Sensori Elettrici (ADS1115)** | **Massima priorità di refactoring** - Accesso I2C condiviso, logica di mediana complessa, budget timing, gestione errori I2C |
| **Gestione Risorse (Gas)** | **Alta priorità** - Accesso HX711 condiviso, logica di calibrazione, salvataggio su SD, variabili cumulative |
| **Gestione Risorse (Acqua)** | **Media priorità** - Interrupt in ISR, accesso a `pulseCount` non protetto da mutex |
| **Gestione Risorse (Ah)** | **Alta priorità** - Integrazione Ah in sezione critica, accumulo orario, salvataggio su SD e NVS |
| **Sensori Ambientali (LIS3DH)** | **Media priorità** - Concorrente con ADS1115 sullo stesso bus I2C, gestione sleep/wake |
| **PrefsTask** | **Media priorità** - Gestione queue, rate-limiting, anti-tempesta, scrittura su NVS |

### Rischi specifici identificati:

1. **Race condition su `pulseCount`**: L'interrupt `pulseCounter()` modifica la variabile senza protezione, mentre `SensorLoop()` la legge. Attualmente gestita con `detachInterrupt/attachInterrupt` (soluzione accettabile ma non ideale).

2. **Concorrenza I2C**: ADS1115, LIS3DH, RTC, EEPROM condividono lo stesso bus I2C. Il mutex `i2cMutex` protegge le operazioni ma potrebbe causare deadlock se non gestito correttamente.

3. **Logica di integrazione Ah**: L'accumulo `ahMinuteDischargeAccum` e `ahMinuteChargeAccum` avviene in sezione critica breve, ma la logica di reset minuto potrebbe avere race condition.

4. **Salvataggio SD multiplo**: Più task (`SensorLoop`, `PrefsTask`, `LateRestoreTask`) accedono alla SD con mutex, ma non c'è protezione contro accessi concorrenti prolungati.

5. **DisplayTask e LevelTask**: Entrambi su Core 1, competono per risorse I2C quando la livella è attiva.

---

## Note Aggiuntive

- **Architettura multitasking**: 6 task principali (`SensorLoop`, `DisplayTask`, `LevelTask`, `DHTTask`, `PrefsTask`, `SDWatchdogTask`)
- **Core affinity**: Sensori e DHT su Core 0, Display e Livella su Core 1
- **Watchdog**: Tutti i task hanno `esp_task_wdt_add(NULL)` per prevenire lockup
- **Memory**: Uso di `volatile` per variabili condivise, allocazione dinamica in `restoreGraphFromSD()` con `malloc()`