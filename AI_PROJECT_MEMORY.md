# AI Project Memory - ESP32 Camper Monitoring System

> **Documento generato per:** Nuova AI che deve comprendere il progetto senza leggere tutto il codice  
> **Ultimo aggiornamento:** 10 luglio 2026  
> **Versione firmware:** main.cpp (3168 righe)

---

## # Scopo del progetto

### Descrizione generale
Sistema di monitoraggio completo per un camper basato su ESP32 che gestisce:
- **Energia elettrica**: monitoraggio batteria (tensione, corrente, Ah residui), produzione solare
- **Risorse idriche**: serbatoio acqua con flussometro, livello residuo
- **Risorse GPL**: due bombole gas con bilance HX711, livello residuo
- **Ambiente**: temperatura/umidità interna ed esterna
- **Livellamento**: accelerometro LIS3DH per livellare il camper
- **Persistenza dati**: grafici 24h, istogrammi Ah/solari su SD

### Obiettivi
1. Monitorare in tempo reale lo stato di carica della batteria e produzione solare
2. Tracciare il consumo acqua e gas nel tempo
3. Fornire interfaccia web responsive per visualizzare dati e grafici 24h
4. Persistenza dati su SD card e NVS (Preferences) per non perdere lo stato
5. Display OLED per visualizzazione locale con gestione luminosità
6. Operare come Access Point WiFi autonomo (niente rete esterna necessaria)

---

## # Hardware

### Modello ESP32
- **Board**: ESP32 Dev Module (`esp32dev`)
- **Framework**: Arduino
- **Filesystem**: SPIFFS
- **Platform**: espressif32@6.9.0

### Sensori
| Sensore | Tipo | Pin | Descrizione |
|---------|------|-----|-------------|
| ADS1115 (x2) | ADC differenziale | I2C (0x48, 0x49) | Corrente batteria (shunt) + corrente solare |
| HX711 (x2) | ADC carico | GPIO 32/33, 25/26 | Bilancia per bombole gas |
| DHT21 (AM2301) | Temp/Hum | GPIO 14 (esterno), GPIO 4 (interno) | Temperatura/umidità ambiente |
| LIS3DH | Accelerometro | I2C (0x19) | Livellamento camper (roll/pitch) |
| YF-S402C | Flussometro | GPIO 27 | Misuratore portata acqua (impulsi) |

### Attuatori
- **LED integrato**: GPIO 2 (indicator)
- **Pulsante**: GPIO 13 (cambio pagina display)

### Display
- **Modello**: OLED SH1107 128x128
- **Interface**: SPI HW (4-wire)
- **Pin**: CS=GPIO 5, DC=GPIO 16, RST=GPIO 17
- **Libreria**: U8g2

### Moduli esterni
- **RTC DS3231**: Orologio tempo reale con backup batteria
- **SD Card**: Memoria per dati storici e crash log
- **DNSServer**: Captive portal per configurazione WiFi

### Bus di comunicazione
- **I2C**: SDA=GPIO 21, SCL=GPIO 22, 50kHz (cavi lunghi)
- **SPI**: VSPI per SD (MOSI=23, MISO=19, SCK=18, CS=15)
- **SPI Display**: 4MHz bus clock

---

## # Architettura software

### Moduli principali
```
src/
├── main.cpp              # Entry point, task creation, variabili globali
├── gas/
│   ├── gas_manager.h     # API gas
│   └── gas_manager.cpp   # Logica bombole HX711
├── water/
│   ├── water_manager.h   # API acqua
│   └── water_manager.cpp # Logica flussometro
├── level/
│   ├── level_manager.h   # API livella
│   └── level_manager.cpp # Logica LIS3DH
├── time/
│   ├── time_manager.h    # API tempo
│   └── time_manager.cpp  # RTC, DST, conversioni
├── storage/
│   ├── graph_storage.h   # API grafici
│   ├── graph_storage.cpp # Persistenza grafici 24h
│   └── crash_logger.h    # API crash log
│   └── crash_logger.cpp  # Logging crash su SD
└── web/
    ├── web_manager.h     # API web
    └── web_manager.cpp   # Handler endpoint HTTP
```

### Responsabilità di ogni modulo

| Modulo | Responsabilità |
|--------|----------------|
| **main.cpp** | Coordinamento sistema, creazione task, variabili globali, logica di integrazione Ah, salvataggio/ripristino SD |
| **gas_manager** | Lettura bilance HX711, calcolo consumo gas, salvataggio/ripristino SD |
| **water_manager** | Gestione flussometro, contatore litri, capacità serbatoio |
| **level_manager** | Livellamento camper, calibrazione zero, gestione sleep LIS3DH |
| **time_manager** | RTC, fuso orario, DST europeo, conversioni epoch |
| **graph_storage** | Persistenza grafici 24h (tensione, corrente, solare) su SD |
| **crash_logger** | Logging crash/reset su SD con stato sistema |
| **web_manager** | Endpoint HTTP, API JSON, invio file SPIFFS |

### Relazioni tra i moduli
- **main.cpp** è il modulo centrale che include tutti gli altri
- **Mutex** (`i2cMutex`, `sdMutex`, `hxMutex`, `prefsMutex`) sincronizzano l'accesso alle risorse
- **Queue** (`prefsQueue`) permette la scrittura asincrona su NVS
- **Web server** legge dati da variabili globali esposte
- **Display** riceve notifiche dal loop principale per ridisegnare

---

## # Flusso di avvio

### Sequenza di boot
1. `setup()` inizializza seriale, mutex, task PrefsTask
2. Log del reset reason su SD (crash logging)
3. Recovery I2C hardware (9 clock pulses)
4. Inizializzazione bus I2C (50kHz)
5. Scansione I2C per rilevare sensori
6. Inizializzazione RTC DS3231
7. Inizializzazione display OLED
8. Inizializzazione DHT interno/esterno
9. Configurazione interrupt flussometro
10. Inizializzazione bilance HX711 (gas)
11. Ripristino valori gas da SD
12. Inizializzazione LIS3DH (livella)
13. Inizializzazione ADS1115 (x2) per corrente/tensione
14. Configurazione WiFi Access Point
15. Inizializzazione SPIFFS
16. Ripristino livella zero da SD/NVS
17. Ripristino litri acqua da SD
18. Ripristino Ah da SD o NVS
19. Ripristino dati orari da SD
20. Ripristino dati giornalieri Ah da SD (ah_day.bin)
21. Configurazione routing web server
22. Creazione task: LateRestore, SDWatchdog, SensorLoop, DisplayTask, LevelTask, DHTTask
23. Primo redraw display

### Inizializzazione servizi
- **Mutex**: Creati immediatamente in setup (sdMutex, i2cMutex, hxMutex, prefsMutex)
- **Queue**: prefsQueue per comunicazione con PrefsTask
- **Watchdog**: Tutti i task registrati con `esp_task_wdt_add(NULL)`

### Inizializzazione hardware
- **I2C**: Wire.begin() con timeout 50ms, clock 50kHz
- **SD**: 20 tentativi di inizializzazione con retry
- **Display**: Bus clock 4MHz per stabilità
- **RTC**: Verifica anno plausibile (>= 2023)

---

## # Task FreeRTOS

| Task | Priorità | Stack | Core | Responsabilità | Frequenza |
|------|----------|-------|------|----------------|-----------|
| **SensorLoop** | 2 | 12288 | 0 | Lettura sensori, integrazione Ah, gestione grafici, salvataggio orario | 100ms (vTaskDelay) |
| **DisplayTask** | 3 | 4096 | 1 | Rendering display OLED | Su notifica (max 5Hz) |
| **LevelTask** | 4 | 4096 | 1 | Aggiornamento livella LIS3DH | 40ms (30Hz) o 100ms (10Hz) |
| **DHTTask** | 1 | 4096 | 0 | Lettura DHT interno/esterno | 2 secondi |
| **PrefsTask** | 1 | 4096 | 0 | Scrittura asincrona su NVS | Su messaggi (debounce 30s) |
| **SDWatchdog** | 1 | 2048 | 0 | Monitor stato SD card | 5 secondi |
| **LateRestore** | 1 | 4096 | 0 | Ripristino grafico post-avvio | Esegue una volta (dopo 8s) |

---

## # Timer

| Nome | Scopo |
|------|-------|
| `rtcBaseEpoch` + `rtcBaseMillis` | Base per calcolo epoch corrente |
| `nextDimAt` | Timeout per riduzione luminosità display |
| `lastPulseMicros` | Debounce interrupt flussometro |
| `lastLevelRequest` | Rileva attività pagina livella |
| `lastSampleMinute` | Tracciamento minuti per grafici |
| `lastAhUpdateMillis` | Timestamp per integrazione Ah |

---

## # Queue, Mutex e Semaphore

### Mutex
| Mutex | Utilizzo |
|-------|----------|
| `sdMutex` | Protezione accessi SD card (crash log, gas, acqua, grafici, ah_day) |
| `i2cMutex` | Protezione bus I2C (ADS1115, LIS3DH, RTC) |
| `hxMutex` | Protezione accessi HX711 (bilance gas) |
| `prefsMutex` | Protezione accessi Preferences (NVS) |

### PortMUX
| Variabile | Utilizzo |
|-----------|----------|
| `stateMux` | Sezione critica per variabili Ah (batteryAh_now, batteryAh_max) |

### Queue
| Queue | Struttura | Utilizzo |
|-------|-----------|----------|
| `prefsQueue` | `PrefsMessage` | Comunicazione asincrona per scrittura su NVS |

### Struttura PrefsMessage
```cpp
enum PrefsCmdType {
  CMD_SAVE_AH_NOW,
  CMD_SAVE_AH_MAX,
  CMD_SAVE_EFF,
  CMD_SAVE_SOLAR_OFFSET,
  CMD_SAVE_LEVEL_R0,
  CMD_SAVE_LEVEL_P0
};

struct PrefsMessage {
  PrefsCmdType cmd;
  float fvalue;
  int ivalue;
};
```

---

## # Comunicazioni

### WiFi
- **Modalità**: Access Point (AP)
- **SSID**: `camper_lamini`
- **Password**: `12365478`
- **IP**: 192.168.4.1
- **Canale**: 6
- **Potenza TX**: 15dBm
- **Hostname**: `camper`

### BLE
- **Non utilizzato** nel progetto

### MQTT
- **Non utilizzato** nel progetto

### HTTP
- **Porta**: 80
- **Server**: WebServer (Arduino)
- **DNS Captive**: DNSServer per redirect richieste

### UART
- **Seriale**: 115200 baud per debug
- **Debug**: Stampa condizionale con `serialConnected()`

### I2C
- **Bus**: SDA=GPIO 21, SCL=GPIO 22
- **Clock**: 50kHz (cavi lunghi)
- **Timeout**: 50ms
- **Device**: ADS1115 (0x48, 0x49), RTC DS3231 (0x68), LIS3DH (0x19)

### SPI
- **Bus**: VSPI per SD card
- **Display**: SPI HW 4-wire separato

### CAN / RS485
- **Non utilizzati** nel progetto

---

## # Storage

### SD
- **Pin CS**: GPIO 15
- **File principali**:
  - `/graph24h.bin` - Grafici tensione/corrente/solare
  - `/ah_state.bin` - Stato Ah corrente
  - `/ah_hours.bin` - Dati orari Ah
  - `/ah_day.bin` - Dati giornalieri Ah (con header data)
  - `/last_gas1.txt`, `/last_gas2.txt` - Livello gas
  - `/last_liters.txt` - Litri consumati
  - `/level_zero.txt` - Offset livella
  - `/crashlog.txt` - Log crash/reset

### Struttura file `/ah_day.bin` (396 byte totali)
```
struct AhDayHeader {
  uint16_t year;   // 2 byte
  uint8_t  month;  // 1 byte
  uint8_t  day;    // 1 byte
};                 // 4 byte totali

float ahHourlyToday[24];         // 96 byte
float ahChargeHourlyToday[24];   // 96 byte
float solAhHourlyToday[24];      // 96 byte
float solAhHourlyYesterday[24];  // 96 byte
float solAhTodayTotal_global;    // 4 byte
float solAhYesterdayTotal_global; // 4 byte
                                 // 396 byte totali
```

### SPIFFS
- `/index.html` - Pagina web principale
- `/chart.min.js` - Libreria grafici Chart.js

### LittleFS
- **Non utilizzato** (usa SPIFFS)

### Preferences/NVS
- **Namespace "energy"**: `ah_max`, `ah_now`, `eff`
- **Namespace "solar"**: `offV` (offset solare)
- **Namespace "level"**: `r0`, `p0` (offset livella)
- **Namespace "shunt"**: `cal`, `off` (calibrazione shunt)

---

## # Configurazione

### Dove sono memorizzate le impostazioni
- **NVS (Preferences)**: Configurazioni persistenti (Ah, efficienza, offset)
- **SD Card**: Dati storici, grafici, log
- **RAM**: Valori runtime, offset livella

### Come vengono caricate
1. **Setup**: Lettura offset shunt da NVS
2. **Setup**: Lettura offset solare da NVS
3. **Setup**: Lettura offset livella da NVS, poi tentativo da SD
4. **LateRestoreTask** (8s dopo boot): Ripristino grafici da SD
5. **Setup**: Ripristino Ah da SD (`/ah_state.bin`) o NVS fallback
6. **Setup**: Ripristino dati orari da SD
7. **Setup**: Ripristino dati giornalieri Ah da SD (`/ah_day.bin`)

---

## # Web Server

### Endpoint
| Endpoint | Metodo | Descrizione |
|----------|--------|-------------|
| `/` | GET | Pagina HTML principale |
| `/chart.min.js` | GET | Libreria Chart.js |
| `/data/live` | GET | Dati live (JSON) |
| `/data/battery` | GET | Grafico batteria 24h (JSON) |
| `/data/solar` | GET | Dati solare (JSON) |
| `/data/water` | GET | Dati acqua (JSON) |
| `/data/gas` | GET | Dati gas (JSON) |
| `/data/ahLive` | GET | Ah ultimi 60 minuti (JSON) |
| `/setTime` | GET | Imposta orologio RTC |
| `/setAh` | GET | Imposta capacità batteria Ah |
| `/setAhMax` | GET | Imposta Ah massimi |
| `/setEff` | GET | Imposta efficienza carica |
| `/setTank` | GET | Imposta capacità serbatoio acqua |
| `/manualGas1` | GET | Imposta livello gas bombola 1 |
| `/manualGas2` | GET | Imposta livello gas bombola 2 |
| `/level` | GET | Dati livella (roll/pitch) |
| `/levelZero` | GET | Salva offset livella corrente |
| `/levelZeroLoad` | GET | Ripristina offset livella da SD |
| `/levelZeroClear` | GET | Cancella offset livella |
| `/calSolarZero` | GET | Calibra offset solare |

### API
- Tutte le API `/data/*` restituiscono JSON con dati numerici
- Le API di configurazione accettano parametri query (`?val=10.5`)
- Risposte: 200 OK, 400 Bad Request, 404 Not Found, 500 Server Error

### Gestione richieste
- **Non bloccante**: `server.handleClient()` nel loop
- **DNS captive**: Risponde a richieste di connectivity check
- **Cache**: `Cache-Control: public, max-age=3600` per chart.min.js

---

## # Classi principali

### U8G2 (display)
- **Scopo**: Gestione display OLED SH1107
- **File**: main.cpp (dichiarazione globale)
- **Dipendenze**: U8g2lib.h

### Adafruit_ADS1115
- **Scopo**: Lettura ADC differenziale per corrente/tensione
- **File**: main.cpp (ads, adsSolar)
- **Dipendenze**: Adafruit ADS1X15 library

### Adafruit_LIS3DH
- **Scopo**: Accelerometro per livellamento
- **File**: main.cpp (lis)
- **Dipendenze**: Adafruit LIS3DH library

### HX711
- **Scopo**: Bilancia per bombole gas
- **File**: main.cpp (scale1, scale2)
- **Dipendenze**: HX711 library

### DHT
- **Scopo**: Sensore temperatura/umidità
- **File**: main.cpp (dhtInt, dhtExt)
- **Dipendenze**: DHT sensor library

### WebServer
- **Scopo**: Server HTTP per API
- **File**: main.cpp
- **Dipendenze**: WebServer.h (Arduino)

### Preferences
- **Scopo**: Memoria non volatile NVS
- **File**: main.cpp (prefs, prefsAh, prefsSolar, prefsLevel)
- **Dipendenze**: Preferences.h (Arduino)

### RTC_DS3231
- **Scopo**: Orologio tempo reale
- **File**: main.cpp (rtc)
- **Dipendenze**: RTClib

---

## # File critici

| File | Motivo |
|------|--------|
| `src/main.cpp` | Entry point, logica di sistema, task, variabili globali, salvataggio Ah |
| `src/gas/gas_manager.cpp` | Gestione bombole gas, logica di consumo cumulativo |
| `src/water/water_manager.cpp` | Gestione acqua, interrupt flussometro |
| `src/level/level_manager.cpp` | Livellamento camper, calibrazione |
| `src/time/time_manager.cpp` | RTC, DST, conversioni tempo |
| `src/storage/graph_storage.cpp` | Persistenza grafici 24h su SD |
| `src/storage/crash_logger.cpp` | Logging crash per debug |
| `src/web/web_manager.cpp` | API web, endpoint HTTP |
| `platformio.ini` | Configurazione build, dipendenze librerie |
| `data/index.html` | Interfaccia web utente |

---

## # Bug conosciuti

| Problema | Gravità | Workaround |
|----------|---------|------------|
| Race condition su `pulseCount` | Media | Usato detach/attach interrupt (accettabile ma non ideale) |
| Concorrenza I2C tra ADS1115 e LIS3DH | Alta | Mutex con timeout, recovery bus automatico |
| Memory leak in `restoreGraphFromSD()` | Media | malloc() potrebbe fallire, ma gestito con controlli |
| DisplayTask potrebbe saturare CPU in livella | Media | Throttling a 5Hz quando livella attiva |
| SD card potrebbe rimanere in stato inconsistente | Alta | SDWatchdogTask tenta reinizializzazione ogni 5s |
| Cambio manuale orario indietro (prevHour > currentHour) | Alta | Nessun workaround - rileverà falso rollover giornaliero |
| Variabile lastHourlyDay dichiarata ma non utilizzata | Media | Nessun workaround - salti temporali >24h non gestiti |

---

## # Debito tecnico

| Refactoring | Priorità | Note |
|-------------|----------|------|
| Estrarre moduli in file .h/.cpp separati | Alta | main.cpp è troppo grande (3168 righe) |
| Usare `portENTER_CRITICAL` invece di `STATE_ENTER` macro | Media | Per consistenza con FreeRTOS |
| Aggiungere task per gestione pulsante | Bassa | Attualmente nel loop principale |
| Implementare queue per dati display | Media | Per ridurre race condition sui flag |
| Separare logica di integrazione Ah | Alta | In file dedicato `resource_ah.cpp` |
| Aggiungere test unitari | Media | Per verificare logica di integrazione |

---

## # Stato progetto

### Aggiornamenti recenti
- **Refactoring pagina Batteria**:
  - Eliminato endpoint `/data/ah24h`
  - Integrati dati Ah in `/data/battery`
- **Fix rollover giornaliero**:
  - Sostituito `prevHour > currentHour`
  - Con confronto day epoch RTC
- **Fix restore RTC**:
  - Inizializzazione `currentYear/currentMonth/currentDay`
  - Da RTC al boot
- **Diagnosi watchdog**:
  - DHTTask disabilitato per test
  - Uptime > 12.000.000 ms senza reboot

---

## # Problemi ancora aperti

- **Confermare definitivamente correlazione DHT ↔ INT_WDT**
- **Valutare sostituzione libreria DHT**

---

## # Test stabilità DHT - Luglio 2026

- Test con ENABLE_DHT_TASK = false
- Uptime raggiunto: >156.000.000 ms (~43,5 ore)
- Nessun reboot osservato
- Nessun INT_WDT osservato
- Heap stabile (~130-136 KB)

- Test successivo con ENABLE_DHT_TASK = true
- Reboot osservato durante il test
- Indagine ancora aperta

Conclusione provvisoria:
- Forte correlazione tra DHTTask attivo e reboot.
- DHT rimane il principale sospettato.
- Necessaria ulteriore conferma tramite bootlog e crashlog.

 
## # Stabilità ESP32
 
### Potenziali memory leak
- `restoreGraphFromSD()`: malloc() per buffer file (controllato, free() chiamato)
- `String` in ArduinoJson: usato `DynamicJsonDocument` con dimensioni fisse
- Allocamenti in `crash_logger.cpp`: vettore di stringhe (limitato a 20 eventi)
 
### Potenziali race condition
```
- `pulseCount`: modificata in ISR, letta in task (gestita con detach/attach)
- Variabili Ah: protette con `stateMux` critical section
- Accesso I2C: protetto con `i2cMutex`
- Accesso SD: protetto con `sdMutex`
```

### Rischi watchdog
- Tutti i task hanno `esp_task_wdt_add(NULL)`
- `vTaskDelay(1)` nei loop pesanti per nutrire watchdog
- Timeout mutex a 200ms per evitare blocchi

### Uso heap
- Monitorato ogni 30s in `monitorTaskStacks()`
- `Serial.printf("[HB] ... Heap=%u\n", ESP.getFreeHeap())`
- DynamicJsonDocument usa buffer fissi (3000-16000 byte)

### Uso String
- Evitato dove possibile
- Usato `String` in crash_logger per costruzione messaggi
- `ArduinoJson` usa `String` per output JSON (necessario)

---

## # Cronologia decisioni

| Data | Decisione | Motivazione |
|------|-----------|-------------|
| 2024 | Architettura multitasking | Separare sensori, display, livella per stabilità |
| 2024 | Core affinity | Sensori su Core 0, Display su Core 1 per ridurre conflitti |
| 2024 | Queue per NVS | Evitare scritture bloccanti su NVS da task multipli |
| 2024 | Mutex I2C con timeout | Prevenire deadlock in caso di sensori non rispondenti |
| 2024 | Recovery I2C hardware | Cavi lunghi possono causare errori di comunicazione |
| 2024 | Filtro IIR su sensori | Ridurre rumore su letture corrente/tensione |
| 2024 | Mediana a blocchi | Robustezza contro outlier nei sensori analogici |
| 2024 | SD con retry | SD card può essere lenta ad avviare |
| 2024 | SPIFFS invece di LittleFS | Compatibilità con piattaforma |
| 2024 | Access Point WiFi | Nessuna rete esterna necessaria, più semplice |
| 2026 | Header data in ah_day.bin | Consente ripristino automatico solo se giorno corrente |
| 2026 | Istogrammi solare in ah_day.bin | Persistenza totale Ah solare per oggi/ieri |

---

## # Come lavorare sul progetto

### Regole da seguire
1. **Non modificare mai main.cpp senza test hardware** - È il cuore del sistema
2. **Usare sempre i mutex esistenti** per accesso a risorse condivise
3. **Non aggiungere delay() nei task** - Usare `vTaskDelay()` invece
4. **Registrare tutti i task con watchdog** - `esp_task_wdt_add(NULL)`
5. **Testare sempre la SD card** - È il punto più critico di fallimento
6. **Verificare heap disponibile** - Dopo ogni modifica pesante
7. **Usare `volatile` per variabili condivise tra ISR e task**

### File da non modificare senza analisi preventiva
- `src/main.cpp` - Logica di sistema complessa, richiede test hardware
- `src/storage/graph_storage.cpp` - Formato file binario specifico
- `src/storage/crash_logger.cpp` - Gestione crash criticica
- `platformio.ini` - Dipendenze librerie consolidate

### Aggiunta di nuovi endpoint
1. Aggiungere dichiarazione in `web_manager.h`
2. Implementare handler in `web_manager.cpp`
3. Registrare route in `setup()` del main.cpp
4. Testare con browser e verificare JSON di risposta

### Aggiunta di nuovi sensori
1. Verificare disponibilità bus I2C/SPI
2. Aggiungere mutex se necessario
3. Creare task dedicato o integrare in SensorLoop
4. Aggiungere variabili globali `volatile`
5. Aggiornare display e API web

---

## # 2026-08-05 - Watchdog Investigation Checkpoint

### Stato del firmware
- Creata baseline stabile salvata in Git.
- Commit: 2c56497
- Messaggio commit:
  "Stabilized firmware, DHT task disabled for watchdog investigation"

### DHT
- DHTTask disabilitato tramite:
  `#define ENABLE_DHT_TASK false`
- DHTTask non viene compilato né creato.
- Sensori DHT inizializzati correttamente all'avvio.
- Nessuna lettura periodica DHT durante il test.

### Misure temporali DHT
Diagnostica corretta effettuata.

Tempi osservati:
- extTemp ≈ 6-9 ms
- extHum ≈ 15-25 us
- intTemp ≈ 5-9 ms
- intHum ≈ 2-6 us
- tempo totale ciclo ≈ 12-16 ms

Conclusioni:
- La precedente misura di circa 2 secondi era dovuta a strumentazione errata.
- Le letture DHT non mostrano blocchi di centinaia di millisecondi o secondi.

### SD Card
- La SD è stata trovata in stato RAW.
- CHKDSK ha ripristinato FAT32.
- Rilevate numerose catene FAT corrotte e collegamenti non validi.
- Alcuni file di log risultavano persi dopo il recupero.
- Successivamente la SD è stata formattata e riutilizzata.

### Hardware / I2C
Durante alcuni avvii:
- nessun dispositivo I2C rilevato
- RTC assente
- ADS1115 assente
- LIS3DH assente

Dopo riavvio:
- RTC rilevato
- ADS1115 principale rilevato
- ADS1115 solare rilevato
- LIS3DH rilevato

Possibile problema di contatti, cablaggio o affidabilità del bus I2C.

### Stabilità osservata
Con `ENABLE_DHT_TASK=false`:
- uptime osservato di circa 18 ore
- nessun INT_WDT osservato nei log raccolti
- heap stabile
- stack stabile
- ADS=1
- LIS=1
- SD funzionante
- WebServer funzionante

### Baseline attuale
Configurazione considerata stabile e usata come riferimento per i successivi test e refactoring.

---

## # 2026-08-06 - Refactoring completo sottosistema DHT

### Refactoring
- Refactoring completo del sottosistema DHT.
- Creazione di:
  - `src/dht/dht_manager.h`
  - `src/dht/dht_manager.cpp`

### Trasferimento nel modulo DHT
- Variabili:
  - `tempInt` / `humInt`
  - `tempExt` / `humExt`
- Istanze:
  - `dhtInt`
  - `dhtExt`
- Funzioni:
  - `initDHT()`
  - `readDHTExternal()`
  - `readDHTInternal()`
  - `updateDHT()`

### Rimozione codice morto
- `readTempInternal()`
- `readTempExternal()`
- Include DHT inutili
- `extern` inutili dei sensori DHT
- Macro pin DHT duplicate

### Stato attuale
- DHT inizializzato ma non attivo.
- `updateDHT()` presente e pronta all'uso.
- `DHTTask` ancora presente ma disabilitata.
- Nessuna regressione osservata.

### Test eseguiti
- Compilazione OK
- Upload OK
- Uptime circa 20 ore
- ADS=1 stabile
- LIS=1 stabile
- Heap stabile
- Nessun watchdog osservato

---

## #  ️2026-08-19 - Diagnostica avanzata INT_WDT e riattivazione DHT

### DHT completamente migrato
- Migrazione completata nel modulo dedicato:
  - `src/dht/dht_manager.h`
  - `src/dht/dht_manager.cpp`

### DHTTask
- Mantenuta ma **non utilizzata**.
- La lettura DHT è ora gestita da `SensorLoop`.

### updateDHT()
- Eseguito da `SensorLoop` ogni **2 secondi**.
- Letture DHT riattivate tramite `SensorLoop`.

### Rimozione definitiva
- `readTempInternal()`
- `readTempExternal()`
- `readTempLiveMinimal()`
- Include DHT inutili
- `extern` DHT inutili

### Diagnostica avanzata INT_WDT
Implementata diagnostica avanzata per l'analisi degli INT_WDT:
- **Heartbeat task**: segnaposto per ogni task principale
- **Timestamp task**: registrazione ultima esecuzione per task
- **Tempi massimi task**: misura del tempo massimo di esecuzione per task
- **Telemetria DHT**: conteggio letture, fallimenti e tempo massimo di lettura
- **Crash logger esteso**: registrazione dati aggiuntivi per analisi post-reboot

### Nuove variabili diagnostiche
- **Heartbeat**:
  - `hbLoop`
  - `hbSensorLoop`
  - `hbLevelTask`
  - `hbDisplayTask`

- **Timestamp ultima esecuzione**:
  - `lastLoopMs`
  - `lastSensorLoopMs`
  - `lastLevelTaskMs`
  - `lastDisplayTaskMs`

- **Tempi massimi di esecuzione**:
  - `loopMaxUs`
  - `sensorLoopMaxUs`
  - `levelTaskMaxUs`
  - `displayTaskMaxUs`

- **Telemetria DHT**:
  - `dhtReads`
  - `dhtReadFailures`
  - `dhtMaxReadUs`

### Obiettivo
- Identificare il task o il sottosistema responsabile degli **INT_WDT** osservati il **19/08/2026**.

---

## # 2026-08-20 - Pulizia di codice morto in main.cpp

### Descrizione
Completata una prima pulizia di codice morto in `main.cpp`.

### Funzioni rimosse
- `yesno()`
- `readBatteryVoltage_singleEnded()`
- `reqNOrDefault()`
- `appendWaterDataToJson()`
- `appendAhToJson()`
- `appendLevelToJson()`
- `drawRightText()`

### Variabili globali inutilizzate rimosse
- `levelPitch`
- `levelRoll`
- `sampleCount`
- `currentCount`
- `solarCurrentCount`
- `lastSampleTime`

### Macros / constantes rimosse
- `AH_DAY_MAGIC`
- `AH_DAY_FILE`

### Estado
- **Nessuna modifica funzionale.**
- **Nessuna modifica a**:
  - DHT
  - SensorLoop
  - diagnostica watchdog
  - crash logger
  - storage
  - web server

### Testes
- Compilación verificada con éxito.

---

## 2026-08-20 - Flight recorder mejorado

### Flight recorder mejorado

- Extendido saveDiagSnapshot().
- Añadidos al archivo /last_diag.txt:

  - rtc=
  - epoch=
  - millis=
  - resetReason=

- rtc usa fecha y hora local cuando están disponibles.
- epoch usa nowEpoch().
- resetReason usa esp_reset_reason() con la misma traducción del crash logger.

### Objetivo

- Hacer identificable el momento exacto de la fotografía de diagnóstico.
- Distinguir los datos del boot actual de los del boot anterior.
- Mejorar el análisis post-mortem de los INT_WDT.

### Estado

- Ninguna modificación a la lógica de la aplicación.
- Ninguna modificación a SensorLoop.
- Ninguna modificación a DHT.
- Ninguna modificación a los temporizadores.
- Solo extensión de diagnóstico.

### Test

- Compilación verificada con éxito.

---

*Fine documento - Questo file è la memoria permanente del projecto e dovrebbe essere aggiornato ad ogni modifica significativa.*
