# AI Project Memory - ESP32 Camper Monitoring System

> **Documento generato per:** Nuova AI che deve comprendere il progetto senza leggere tutto il codice  
> **Ultimo aggiornamento:** 28 settembre 2026  
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
| DHT21 (AM2301) | Temp/Hum | GPIO 14 (esterno), GPIO 13 (interno) | Temperatura/umidità ambiente |
| LIS3DH | Accelerometro | I2C (0x19) | Livellamento camper (roll/pitch) |
| YF-S402C | Flussometro | GPIO 27 | Misuratore portata acqua (impulsi) |

### Attuatori
- **LED integrato**: **disabilitato** (`ledPin` è commentato in `main.cpp`); GPIO 2 libero
- **Pulsante**: GPIO 4 (cambio pagina display)

> **Pinout in esercizio (dal test di isolamento del 2026-09-16):** DHT interno su GPIO 13 (in precedenza GPIO 4) e pulsante su GPIO 4 (in precedenza GPIO 13); GPIO 2 libero perché il LED integrato è disabilitato.

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
| **LevelTask** | 4 | 4096 | 1 | Aggiornamento livella LIS3DH | 40ms (attiva) / 100ms (idle); `updateLevel()` interno 40ms (fast) o 120ms (slow) |
| **DHTTask** | 1 | 4096 | 0 | Lettura DHT interno/esterno (`updateDHT()`) | 10 secondi |
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
- **Password**: `12345678`
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

### Struttura file `/ah_day.bin` (412 byte totali)

Layout scritto da `saveAhToSD()` e letto da `restoreAhDayFromSD()` (`src/main.cpp`).

```
struct AhDayHeader {
  uint16_t year;   // 2 byte
  uint8_t  month;  // 1 byte
  uint8_t  day;    // 1 byte
};                 // 4 byte totali

float ahHourlyToday[24];             // 96 byte
float ahChargeHourlyToday[24];       // 96 byte
float solAhHourlyToday[24];          // 96 byte
float solAhHourlyYesterday[24];      // 96 byte
float ahTodayTotal_global;           // 4 byte
float ahYesterdayTotal_global;       // 4 byte
float ahChargeTodayTotal_global;     // 4 byte
float ahChargeYesterdayTotal_global; // 4 byte
float solAhTodayTotal_global;        // 4 byte
float solAhYesterdayTotal_global;    // 4 byte
                                     // 412 byte totali
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
| `/data/climate` | GET | Istogrammi clima 24h (JSON) |
| `/data/solar` | GET | Dati solare (JSON) |
| `/data/water` | GET | Dati acqua (JSON) |
| `/data/gas` | GET | Dati gas (JSON) |
| `/data/ah24h` | GET | Istogrammi orari Ah oggi/ieri + totali (JSON) |
| `/data/ahLive` | GET | Ah ultimi 60 minuti (JSON) |
| `/api/boot` | GET | Snapshot diagnostico di boot, incluse le telemetrie DHT (JSON) |
| `/api/crash` | GET | Lista eventi del crash log, dal più recente (JSON) |
| `/api/crashdetail` | GET | Blocco completo dell'evento `?id=N` (testo) |
| `/setTime` | GET | Imposta orologio RTC |
| `/setAh` | GET | Imposta capacità batteria Ah |
| `/setAhMax` | GET | Imposta Ah massimi |
| `/setEff` | GET | Imposta efficienza carica |
| `/setTank` | GET | Imposta i litri attualmente presenti nel serbatoio (`?litri=` o `?val=`); la capacità resta invariata |
| `/manualGas1` | GET | Imposta livello gas bombola 1 |
| `/manualGas2` | GET | Imposta livello gas bombola 2 |
| `/level` | GET | Dati livella (roll/pitch) |
| `/levelZero` | GET | Salva offset livella corrente |
| `/levelZeroLoad` | GET | Ripristina offset livella da SD |
| `/levelZeroClear` | GET | Cancella offset livella |
| `/calSolarZero` | GET | Calibra offset solare |

Note:
- l'endpoint `/data` risponde **410** ("Endpoint /data dismesso");
- gli endpoint del captive portal (`/generate_204`, `/gen_204`, `/hotspot-detect.html`, `/connecttest.txt`, `/ncsi.txt`) e `/favicon.ico` rispondono **200**;
- gli handler `/data/*` e `/api/*` sono dichiarati in `web_manager.h` e definiti in `web_manager.cpp`, mentre le route sono registrate nel `setup()` di `main.cpp`.

### API
- Tutte le API `/data/*` restituiscono JSON con dati numerici
- Le API di configurazione accettano parametri query (`?val=10.5`; per `/setTank` `?litri=`/`?val=`, per `/manualGas1`/`/manualGas2` `?kg=`, per `/setTime` `?time=`)
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
- **Scopo**: Sensori temperatura/umidità (esterno + interno)
- **File**: `src/dht/dht_manager.h` / `src/dht/dht_manager.cpp` — modello unificato `dhtSensors[]` con i canali `DHT_CHANNEL_EXTERNAL` / `DHT_CHANNEL_INTERNAL`
- **Dipendenze**: **DHTNEW 0.5.5** (Rob Tillaart, vendorizzata in `lib/DHTNEW/`); il fork `lib/DHT/` ("Soluzione B") resta nel repository non utilizzato, come rollback

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
- **Pagina Batteria**:
  - Endpoint `/data/ah24h` **attivo** (registrato in `main.cpp` → `handleDataAh24h`): la rimozione era temporanea e oggi è la fonte degli istogrammi orari Ah
  - Dati Ah presenti anche in `/data/battery`
- **Fix rollover giornaliero**:
  - Sostituito `prevHour > currentHour`
  - Con confronto day epoch RTC
- **Fix restore RTC**:
  - Inizializzazione `currentYear/currentMonth/currentDay`
  - Da RTC al boot
- **Diagnosi watchdog**:
  - DHTTask **oggi attivo** (`#define ENABLE_DHT_TASK true`): task dedicato, 10 s, Core 0, priorità 1
  - Uptime > 12.000.000 ms senza reboot osservato con la lettura DHT sospesa (misura storica)

---

## # Problemi ancora aperti

- **Confermare su sessioni più lunghe l'esito positivo di DHTNEW**: è la libreria di lettura **in esercizio** dal 2026-09-17; in caso di rollback resta disponibile il fork "Soluzione B" in `lib/DHT/`, non più percorso dal codice
- **Confermare definitivamente la correlazione DHT ↔ INT_WDT**: nessun nuovo `INT_WDT` osservato con DHTNEW nella finestra di prova

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

> **Superato (aggiornamento 2026-09-16):** la lettura DHT è tornata nel task dedicato `DHTTask` (Core 0, priorità 1, periodo **10 s**) e `SensorLoop` non legge più dal bus DHT. Quanto descritto in questa sezione vale solo per lo stato del 2026-08-19.

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

---

## 2026-08-20 - Correzione range grafici batteria e solare

### Problema

- I pulsanti **1h**, **6h** e **24h** funzionavano nel frontend.
- Il frontend inviava correttamente:
  - n=60
  - n=360
  - n=1440
- I backend:
  - `handleDataBattery()`
  - `handleDataSolar()`
  
  ignoravano completamente il parametro.
- Entrambi usavano:
  - `N = 360`

### Conseguenza

- **1h**
- **6h**
- **24h**

mostravano sostanzialmente la stessa finestra temporale.

### Soluzione

- Lettura del parametro `n` dal backend.
- Introduzione downsampling automatico.
- Nessuna modifica al frontend.
- Nessuna modifica al formato JSON.
- Nessuna modifica allo storage.

### Limiti applicati

**Batteria:**
- massimo 300 punti

**Solare:**
- massimo 250 punti

### Motivazione

- evitar JSON troppo grandi
- evitar aumento dell'heap
- evitar rallentamenti lato ESP32
- mantenere compatibilità con `DynamicJsonDocument` esistente

### Resultado

- **1h** ≈ 60 puntos
- **6h** ≈ 180 puntos
- **24h** ≈ 240-300 puntos

### Verifiche

- compilazione completada con éxito
- test browser completado
- test Network tab completado

### Dimensión respuestas observadas

- n=60  -> circ. 2.1 kB
- n=360 -> circ. 5.0 kB
- n=1440 -> circ. 8.5 kB

### Estado

- bug resuelto
- ventana 24h realmente visualizada
- downsampling backend activo

---
## 2026-08-20 - Eliminazione rumore corrente nel grafico storico

### Problema

- Il grafico storico della corrente batteria mostrava piccole barre anche quando la corrente reale era sostanzialmente nulla.
- Il valore medio del minuto (avgI) veniva salvato nello storico senza una soglia anti-rumore dedicata.
- Il rumore residuo dell'ADC poteva quindi generare campioni diversi da zero in `currentSamples[]`.

### Analisi

- Esistevano già soglie anti-rumore per:
  - `amps_raw`
  - `currentFiltered`
  - `integrateAh()`

- Esisteva inoltre una soglia analoga per:
  - `avgSolar`

- Mancava però una soglia sulla media del minuto:
  - `avgI`

### Soluzione

Soglia aggiunta:

```cpp
if (fabs(avgI) < 0.01f)
    avgI = 0.0f;
```

immediatamente prima della scrittura in:

`currentSamples[]`

### Obiettivi

- eliminare le barre fantasma nel grafico storico
- mantenere invariata la misura reale
- non modificare il display live
- non modificare la logica Ah
- non modificare storage o formato dati

### Impatto

- grafico storico batteria più pulito
- display live invariato
- integrazione Ah invariata
- batteryAh invariato
- API live invariata

### Test

- compilazione completata con successo
- nessun nuovo warning
- modifica confinata in `src/main.cpp`

---
## 2026-08-20 - Correzione comando riempimento serbatoio agua

### Problema

- Il pulsante "Riempimento serbatoio" apriva correttamente il popup.
- Il valore veniva inserito correttamente.
- Il frontend inviava:

  /setTank?litri=<valore>

- Il backend però cercava:

  server.arg("val")

### Conseguenza

- risposta HTTP 400
- messaggio: "missing val"
- il valore del serbatoio non veniva aggiornato

### Analisi

- endpoint registrato corretamente
- refresh pagina corretto
- variabili agua corrette
- bug dovuto esclusivamente al nome del parametro

### Soluzione

handleSetTank() ora accetta:

- litri (parametro principal)
- val (retrocompatibilità)

### Comportamento

- /setTank?litri=100 -> 200 OK
- /setTank?val=100 -> 200 OK
- /setTank -> 400 Bad Request

### Obiettivi

- ripristinare el pulsante riabilitato correcto
- mantener compatibilidad con vecchie llamadas

### Test

- compilazione completata con éxito
- endpoint verificato
- logica serbatoio invariata

---

## 2026-08-20

### Correzione logica riempimento serbatoio

### Problema

- Il popup chiedeva:
  "Quanti litri contiene ORA il serbatoio?"
- Il backend interpretava il valore inserito come nuova capacità del serbatoio.
- La capacità veniva sovrascritta.
- La percentuale risultava sempre 100%.

### Analisi

- La grafica era corretta.
- La formula della percentuale era corretta.
- Il problema era nella logica di handleSetTank().

### Soluzione

- tankCapacity non viene più modificata.
- Il valore inserito rappresenta i litri realmente presenti.
- remainingLiters viene aggiornato con il valore inserito.
- totalLitersConsumed viene ricalcolato come:

  tankCapacity - remainingLiters

- Viene salvato su SD il valore corretto di totalLitersConsumed.

### Risultato

- 0 L -> 0%
- 20 L -> 20%
- 50 L -> 50%
- 100 L -> 100%

### Test

- verifica reale sul dispositivo
- grafica corretta
- litri rimanenti corretti
- compilazione completata con successo

---

## 2026-08-21

### Correzione persistenza immediata configurazione batteria

### Problema

- Dopo "Imposta Ah batterie", i valori venivano aggiornati correttamente in RAM.
- Se l'ESP32 veniva riavviato entro circa 30 secondi, i nuovi valori andavano persi.
- Al reboot venivano ripristinati i vecchi valori.
- Il problema era riproducibile:

  1. Imposta Ah batterie = 20
  2. SOC = 100%
  3. Riavvio immediato
  4. Ritorno ai precedenti 10 Ah e SOC 100%

### Analisi

- handleSetAh() e handleSetAhMax() usavano solo salvataggio differito.
- La persistenza reale avveniva tramite PrefsTask.
- PrefsTask utilizza un debounce di 30 secondi.
- Un reboot prima dello scadere del debounce causava la perdita delle modifiche.

### Soluzione

- Aggiunta persistenza immediata in NVS per:
  - handleSetAh()
  - handleSetAhMax()

- Mantenuto invariato il debounce di 30 secondi per il funzionamento normale.

- Introdotta funzione:

  saveAhConfigImmediateToPrefs()

- Aggiunta sincronizzazione tramite prefsMutex.

### Risultato

- I valori configurati sopravvivono anche a un riavvio immediato.
- Nessuna modifica all'integrazione Ah.
- Nessuna modifica alla formula SOC.
- Nessuna modifica al frontend.

### Test

1. Imposta Ah batterie = 20
2. SOC = 100%
3. Riavvio immediato
4. Dopo il reboot:
   - Capacità = 20 Ah
   - Ah disponibili = 20 Ah
   - SOC = 100%

Test superato.

## 2026-08-25 - Deadband anti-rumore sui grafici Ah (aggregazione minuto)

### Problema

- Tutti i grafici Ah mostravano micro-barre generate dal rumore residuo che supera sporadicamente i gate da 10/20 mA della corrente.
- Le barre comparivano nel live 60 minuti (`ahMinuteDischarge[]`/`ahMinuteCharge[]`), negli istogrammi orari (`ahHourlyToday[]`/`ahChargeHourlyToday[]`) e gonfiavano i totali giornalieri.

### Analisi numerica

- Gate esistenti a monte: `amps_raw < 0.02 A -> 0`, `currentFiltered < 0.01 A -> 0`, `iForAh < 0.01 A -> 0`, dt = 1 s.
- Un spike che passa il gate contribuisce <= 0.05/3600 = 1.4e-5 Ah per secondo.
- Micro-barra tipica osservabile via `[AH-NOISE]`: 1e-5...1e-4 Ah/min; picco ~3e-4 Ah/min.
- Piu' piccolo carico reale continuo (50 mA): 8.3e-4 Ah/min.

### Soluzione

Deadband unico applicato nel tick del minuto (punto unico di aggregazione), PRIMA della scrittura di:

- `ahMinuteDischarge[60]` / `ahMinuteCharge[60]`
- `ahHourlyToday[24]` / `ahChargeHourlyToday[24]`
- `ahTodayTotal_global` / `ahChargeTodayTotal_global`

```cpp
const float AH_MINUTE_DEADBAND_AH = 0.0005f;  // 0,5 mAh/min = 30 mA medi per minuto pieno
if (minuteDischargeRaw >= AH_MINUTE_DEADBAND_AH)
    minuteDischarge = minuteDischargeRaw;     // altrimenti resta 0
```

Scarto secco (non carry-over): il carry-over riemetterebbe periodicamente il rumore accumulato come nuove micro-barre.

### Coerenza matematica

Ore, totali e barre derivano dalla stessa variabile filtrata nello stesso tick:
somma barre = somma ore = totale visualizzato, per costruzione.
Il SOC (`batteryAh_now`) non e' toccato: integra in `integrateAh()` a parte.
Sottostima statistica massima teorica 0.72 Ah/giorno (irrealistica), tipica < 0.05 Ah/giorno.

### Non modificato

- Frontend (nessun filtro lato browser).
- Endpoint `/data/ahLive`, `/data/ah24h`, `/data/battery` (serializzano le variabili gia' filtrate alla fonte).
- Storage SD (persistisce gli array gia' filtrati).

### File modificati

- `src/main.cpp` (solo)


---
---
## 2026-09-16 - Investigazione DHT, FRAME_TIMEOUT e stabilità firmware

### Problema

Storicamente il firmware ha mostrato reboot sporadici compatibili con problemi di watchdog (INT_WDT).
I sensori DHT erano tra i principali sospettati.

Dopo il refactoring del sottosistema DHT e l'introduzione della libreria DHT custom "Soluzione B", i reboot sembrano essersi drasticamente ridotti o eliminati.
Parallelamente è emersa una forte degradazione dell'affidabilità delle letture DHT, soprattutto sul canale interno.

### Contesto hardware

Sensori:

- DHT esterno: AM2301 (DHT21), GPIO14
- DHT interno: AM2301 (DHT21), GPIO4

Verifiche effettuate:

- sostituito il DHT interno con un AM2301 nuovo
- invertiti fisicamente i due sensori
- utilizzo di coppia twistata CAT5 per DATA/GND sul sensore interno
- aggiunta pull-up esterna 4.7kΩ tra DATA e 3.3V sul canale interno

Nessuna delle prove ha eliminato il problema del DHT interno.

### Diagnostica introdotta

Aggiunta telemetria nella libreria DHT:

- `DHT::lastFailure()`

Categorie:

- NONE
- PULSE_LOW_TIMEOUT
- PULSE_HIGH_TIMEOUT
- FRAME_TIMEOUT
- BIT_TIMEOUT
- CHECKSUM_ERROR

Successivamente aggiunti contatori cumulativi per canale:

- pulseLowTimeouts
- pulseHighTimeouts
- frameTimeouts
- bitTimeouts
- checksumErrors

Esposizione tramite `/api/boot` per DHT_EXT e DHT_INT.

### Risultati osservati

Campione raccolto:

DHT_EXT

- reads = 377
- failures = 91
- pulseLowTimeouts = 0
- pulseHighTimeouts = 0
- frameTimeouts = 0
- bitTimeouts = 0
- checksumErrors = 91

Conclusione: il DHT esterno completa correttamente il protocollo ma produce occasionalmente errori di checksum.

DHT_INT

- reads = 377
- failures = 376
- pulseLowTimeouts = 0
- pulseHighTimeouts = 0
- frameTimeouts = 361
- bitTimeouts = 15
- checksumErrors = 0

Conclusione: il DHT interno non soffre di errori di handshake. Il protocollo parte correttamente ma nella quasi totalità dei casi il frame non viene completato entro il budget temporale previsto dalla libreria. FRAME_TIMEOUT è la modalità di errore dominante.

### Analisi del fork DHT

Confronto con la libreria Adafruit originale:

Originale:

- timeout basati su conteggio cicli CPU
- timeout reale su ESP32 ≈ 35 ms per pulse
- nessun budget sull'intero frame
- interrupt disabilitati per l'intero frame
- possibile permanenza in sezione critica fino a circa 2,9 s

Fork "Soluzione B":

- `DHT_PULSE_BUDGET_US = 1000`
- `DHT_FRAME_BUDGET_US = 20000`
- timeout basati su `micros()`
- interrupt disabilitati solo per il singolo pulse
- durata massima di `read()` circa 24 ms

Conclusione: il fork è molto più severo verso il sensore ma molto più sicuro per la stabilità dell'ESP32.

### Impatto sulla stabilità

Dopo l'introduzione della Soluzione B:

- nessun INT_WDT osservato durante il test
- uptime superiore a molte ore consecutive
- forte evidenza che la precedente implementazione DHT fosse coinvolta nei reboot

La correlazione DHT ↔ watchdog rimane il principale candidato per spiegare gli INT_WDT storici.

### Decisione progettuale

Priorità:

1. stabilità del firmware
2. eliminazione dei reboot
3. affidabilità delle letture DHT

Non effettuare rollback alla libreria DHT originale senza un piano di mitigazione watchdog.
Eventuali miglioramenti futuri dovranno preservare:

- budget temporali protetti
- assenza di blocchi prolungati degli interrupt
- stabilità osservata del sistema

### Stato attuale

- firmware stabile
- DHT esterno: problemi prevalentemente di checksum
- DHT interno: problemi prevalentemente di frame timeout
- nessun conflitto software individuato sui GPIO 4 e 14
- nessuna evidenza di sensore guasto
- indagine DHT ancora aperta


### Addendum tecnico (verifica sul codice, 2026-09-16)

Analisi completa in **`ANALISI_DHT_FRAME_TIMEOUT.md`**. In sintesi:

- **`FRAME_TIMEOUT` non significa "frame lento"**: in `DHT.cpp` il `TIMEOUT` restituito da `expectPulse()` non interrompe il loop dei 40 bit (viene testato solo dopo, nel secondo loop). Un pulse in timeout precoce fa bruciare il budget da 1000 µs alle attese residue, quindi il tetto dei 20 ms scatta **prima** del controllo → `FRAME_TIMEOUT`; lo stesso evento in coda al frame diventa `BIT_TIMEOUT`. **Sono lo stesso evento fisico classificato in base al punto di interruzione** (361/15 ⇒ interruzione tipica nei primi ~80% del frame).
- Un sensore sano ma lento **non può** generare ≥16 attese morte: servono più di 1 ms di linea ferma, ripetuti ⇒ il canale interno **smette di commutare subito dopo l'handshake**.
- `PULSE_*_TIMEOUT = 0` esclude cablaggio interrotto/pull-up assente; `CHECKSUM_ERROR = 0` sul canale interno esclude distorsione del segnale: è un guasto **duro e sistematico** sulla rete di GPIO 4, non una marginalità.
- I 91 checksum error di EXT (0 timeout) sono compatibili con la finestra di prelazione fra la misura LOW e HIGH dello stesso bit: `InterruptLock` è per-pulse e `DHTTask` gira su Core 0 a priorità 1 (WiFi/lwIP a 18–23). Effetto collaterale noto del fork, non un motivo per tornare all'originale.
- **Ipotesi principali**: H1 rete/pin GPIO 4 (il guasto segue il canale, non il sensore); H2 margine di alimentazione del sensore (brownout a metà frame); H3 desincronizzazione del lettore.
- **Escluso**: conflitti software sui pin (GPIO 4 usato solo dal DHT interno; flow 27, button 13, HX711 32/33 e 25/26), sensore guasto, pull-up mancante, budget troppo severo.
- **Telemetria aggiunta** (solo osservabilità; budget, timeout, quarantena, `readEnabled` e task invariati): `DHT::lastFrameDiag()` nella libreria e, per canale, `lastFramePulses` / `lastFrameLowTimeouts` / `lastFrameHighTimeouts` / `lastFrameMaxPulseUs` su `/api/boot`, più i cumulativi `frameLowTimeoutPulses` / `frameHighTimeoutPulses` / `maxPulseUs` anche in `/last_diag.txt` e `/crashlog.txt`. Discriminano "linea statica" da "bitstream lento/desincronizzato".
- **Prossimi passi** (dal più economico): nuovo campione da `/api/boot` letto con la tabella di interpretazione; scambio dei due pin in software per capire se il guasto segue il pin o il sensore; sensore interno in banco con filo corto e decoupling locale; misura di VDD e DATA durante la lettura; inversione dell'ordine di lettura EXT/INT.

Build verificata: `pio run -e esp32dev` → **SUCCESS** (RAM 34,1%, Flash 79,5%).

### File modificati (solo telemetria additiva)

- `lib/DHT/DHT.h`, `lib/DHT/DHT.cpp`
- `src/dht/dht_manager.h`, `src/dht/dht_manager.cpp`
- `src/web/web_manager.cpp`
- `src/main.cpp` (`saveDiagSnapshot`)
- `src/storage/crash_logger.cpp` (`logCrash`)
- `ANALISI_DHT_FRAME_TIMEOUT.md` (nuovo)


---
---
## 2026-09-16 - Investigazione DHT e watchdog: esito consolidato

Checkpoint conclusivo dell'indagine riassunta nella sezione precedente.
Dettaglio tecnico in **`ANALISI_DHT_FRAME_TIMEOUT.md`** e **`DHT_WATCHDOG_ROOT_CAUSE_ANALYSIS.md`**.

### Problema iniziale

- Reboot sporadici del firmware compatibili con **INT_WDT**, noti da luglio 2026 e mai osservati con la task DHT sospesa.
- La libreria Adafruit DHT originale disabilita gli interrupt per l'intero frame: caso peggiore stimato fino a ~2,9 s di sezione critica, quindi oltre i limiti del watchdog.
- Dopo il refactoring del sottosistema DHT e l'adozione del fork **"Soluzione B"** i reboot sono di fatto scomparsi, ma è emersa una forte degradazione dell'affidabilità delle letture, concentrata sul **canale interno** (GPIO 4).

### Interventi svolti

Hardware:

- sostituito il DHT interno con un **AM2301 (DHT21) nuovo**: nessun miglioramento;
- **invertiti fisicamente** i due sensori: il problema è rimasto associato al ramo interno e non al sensore testato. Non è ancora stato eseguito lo scambio software dei GPIO, quindi il legame definitivo con GPIO4 non è ancora stato dimostrato.
- doppino twistato CAT5 per DATA/GND sul canale interno;
- **pull-up esterna aggiuntiva 4.7kΩ** tra DATA e 3.3V sul canale interno: nessun miglioramento significativo.

Software (solo telemetria additiva, nessuna modifica alla logica di lettura):

- `DHT::lastFailure()` con le categorie `NONE`, `PULSE_LOW_TIMEOUT`, `PULSE_HIGH_TIMEOUT`, `FRAME_TIMEOUT`, `BIT_TIMEOUT`, `CHECKSUM_ERROR`;
- contatori cumulativi per canale (`pulseLowTimeouts`, `pulseHighTimeouts`, `frameTimeouts`, `bitTimeouts`, `checksumErrors`) esposti in `/api/boot` per `DHT_EXT` e `DHT_INT`;
- verifica dei pin sul repository: **nessun conflitto software** su GPIO 4 e GPIO 14 (GPIO 4 usato solo dal DHT interno, GPIO 14 solo dal DHT esterno).

### Telemetria raccolta

DHT_EXT (GPIO 14, AM2301)

- reads = 377
- failures = 91
- pulseLowTimeouts = 0 / pulseHighTimeouts = 0
- frameTimeouts = 0 / bitTimeouts = 0
- checksumErrors = 91

DHT_INT (GPIO 4, AM2301)

- reads = 377
- failures = 376
- pulseLowTimeouts = 0 / pulseHighTimeouts = 0
- frameTimeouts = 361 / bitTimeouts = 15
- checksumErrors = 0

Lettura dei dati:

- **DHT_EXT**: il protocollo si completa sempre, l'errore è solo di **checksum** (91/377) → bitstream integro ma occasionalmente disturbato.
- **DHT_INT**: handshake sempre corretto, frame quasi mai completato; `FRAME_TIMEOUT` (361) e `BIT_TIMEOUT` (15) sono lo **stesso evento fisico**, classificato in base al punto di interruzione del frame.
- `PULSE_*_TIMEOUT = 0` su entrambi i canali esclude cablaggio interrotto o pull-up assente; `CHECKSUM_ERROR = 0` sul canale interno esclude distorsione del segnale.

### Conclusioni raggiunte

- Il guasto del canale interno è **duro e sistematico** sulla rete di GPIO 4: non è una marginalità e non è il sensore (sostituito e invertito senza effetto).
- La pull-up aggiuntiva da 4.7kΩ non ha prodotto miglioramenti misurabili. L'ipotesi 'pull-up troppo debole' perde forza, ma non è possibile escludere completamente fenomeni elettrici dinamici, disturbi locali o problemi legati al percorso fisico del segnale.
- Nessun conflitto software o di assegnazione pin individuato nel repository.
- Il fork **"Soluzione B"** (`DHT_PULSE_BUDGET_US = 1000`, `DHT_FRAME_BUDGET_US = 20000`, timeout su `micros()`, interrupt disabilitati solo per il singolo pulse, `read()` limitata a ~24 ms) è **fortemente correlato alla scomparsa dei reboot watchdog**: uptime osservato superiore a molte ore consecutive, senza INT_WDT anomali.
- Resta l'ipotesi di un problema **elettrico locale** sul canale interno (rete/pin di GPIO 4 o margine di alimentazione del sensore), non un difetto della libreria.

### Decisione progettuale attuale

- **Mantenere il fork "Soluzione B"**: nessun rollback alla libreria Adafruit originale senza un piano di mitigazione watchdog.
- Priorità invariata: 1. stabilità del firmware, 2. assenza di reboot, 3. affidabilità delle letture DHT.
- Non alterare i budget temporali e non reintrodurre sezioni critiche lunghe: ogni modifica futura deve preservare l'assenza di blocchi prolungati degli interrupt.
- L'indagine sul DHT interno resta **aperta ma non bloccante**: la degradazione attuale del canale interno non compromette la stabilità del sistema.


---
---
## 2026-09-17 - Prova DHTNEW 0.5.5: esito su 16 ore di esercizio

### Oggetto della prova

Prova sul campo di una libreria di lettura DHT alternativa al fork Adafruit "Soluzione B":

- libreria **DHTNEW 0.5.5** di Rob Tillaart, vendorizzata in `lib/DHTNEW/` e **non modificata**;
- il fork `lib/DHT/` (Soluzione B) resta nel repository, **non modificato**, come riferimento e possibile rollback;
- la prova è pilotata da `src/dht/dht_manager.cpp`; all'introduzione era dichiarata come "PROVA SPERIMENTALE DHTNEW" nell'intestazione di `src/dht/dht_manager.h`, etichettatura rimossa nel cleanup del 2026-09-18 perché DHTNEW è nel frattempo diventata la libreria di lettura in esercizio.

### Configurazione di test

Hardware:

- DHT esterno: **AM2301** (DHT21) su **GPIO 14**;
- DHT interno: **DHT22** su **GPIO 13** (GPIO liberato spostando il pulsante di cambio pagina);
- pulsante cambio pagina su **GPIO 4**; LED integrato (GPIO 2) ancora disabilitato.

> Nota: questo pinout è quello **in esercizio** ed è riportato nella tabella "Hardware"; le sezioni precedenti a questa data indicano DHT interno su GPIO 4 e pulsante su GPIO 13, pinout ormai superato. I commenti nel codice che lo etichettavano "TEST TEMPORANEO 2026-09-16" sono stati rimossi nel cleanup del 2026-09-18.

Software (invariato rispetto alla Soluzione B):

- modello dati `dhtSensors[]` e canali `DHT_CHANNEL_EXTERNAL` / `DHT_CHANNEL_INTERNAL`;
- quarantena: 5 errori consecutivi e backoff (1 minuto, `DHT_QUARANTINE_DURATION_MS`; valore in esercizio, il tag `[DIAG-TEMP]` è stato rimosso dal codice nel cleanup del 2026-09-18);
- finestra di validità dell'ultimo dato valido: 60 s (`DHT_VALIDITY_TIMEOUT_MS`);
- `DHTTask` dedicato (Core 0, priorità 1, periodo 10 s) e watchdog **invariati**: nessuna lettura DHT nel percorso critico.

Adattamento alla nuova libreria:

- `setType(22)`: tipo forzato per evitare l'autodetect bloccante (1 s e fino a 3 letture) alla prima `read()`;
- `setWaitForReading(false)`: `read()` non bloccante, restituisce `DHTLIB_WAITING_FOR_READ` invece di attendere in loop;
- una sola `read()` per canale per ciclo; i valori sono prelevati **solo** con `DHTLIB_OK`, perché in errore DHTNEW restituisce `DHTLIB_INVALID_VALUE` (-999) e non NAN: il contratto `!isnan` di `updateDhtChannel()` resta identico a quello del fork.

### Risultati osservati (sessione notturna)

- **uptime = 58682 s (~16 ore)** e **nessun reboot osservato durante la notte**;
- `reads` coerente con il periodo del `DHTTask`: ~10,5 s per ciclo sulle ~16 ore (10 s nominali più i cicli saltati dalla quarantena).

| Canale | Sensore | Pin | reads | failures | lastFailure | successo |
|---|---|---|---|---|---|---|
| DHT_EXT | AM2301 | GPIO 14 | 5600 | 348 | SENSOR_NOT_READY | ~93,8% |
| DHT_INT | DHT22 | GPIO 13 | 5600 | 62 | NONE | ~98,9% |

Lettura dei dati:

- **DHT_INT**: `lastFailure = NONE` → l'ultima lettura fisica si è chiusa con `DHTLIB_OK`; 62 errori su 5600 tentativi (~1,1%);
- **DHT_EXT**: `lastFailure = SENSOR_NOT_READY` → ultima lettura scartata prima dell'handshake (il sensore non ha portato LOW la linea entro la finestra di attesa); 348 errori su 5600 (~6,2%), compatibili con disturbi transitori sul ramo esterno, della stessa natura degli errori già visti con il fork (lì tutti classificati come `CHECKSUM_ERROR`);
- **nessun `INT_WDT`** e nessun reboot nella finestra di osservazione.

### Telemetria disponibile con DHTNEW

- l'esito grezzo dell'ultima lettura fisica per canale è esposto in `/api/boot` come `lastFailure` (etichetta di `dhtFailureLabel()`) e `lastStatus` (codice `DHTLIB_*` grezzo memorizzato da `dht_manager.cpp`, perché DHTNEW non espone un getter dello stato);
- i campi diagnostici avanzati del fork — `FrameDiag` (`lastFramePulses` / `lastFrameLowTimeouts` / `lastFrameHighTimeouts` / `lastFrameMaxPulseUs`) e i contatori per categoria (`pulseLowTimeouts` / `pulseHighTimeouts` / `frameTimeouts` / `bitTimeouts` / `checksumErrors`) con i cumulativi relativi (`frameLowTimeoutPulses` / `frameHighTimeoutPulses` / `maxPulseUs`) — **restano nel modello dati e in `/api/boot` per compatibilità, ma sono inerti a 0**: DHTNEW non li alimenta; solo i cumulativi (`frameLowTimeoutPulses` / `frameHighTimeoutPulses` / `maxPulseUs`) sono riportati anche in `/last_diag.txt` e `/crashlog.txt`;
- conseguenza operativa: con DHTNEW **non è più possibile classificare la fase del frame** (handshake / bitstream / checksum). L'analisi per categoria del 2026-09-16 resta valida ma non è riproducibile senza riattivare il fork.

### Confronto con la Soluzione B (fork Adafruit, sessioni 2026-09-16)

| Grandezza | Soluzione B — fork (2026-09-16) | DHTNEW 0.5.5 (2026-09-17) |
|---|---|---|
| DHT esterno (GPIO 14) | reads 377 / failures 91 (24%) — tutti `checksumErrors` | reads 5600 / failures 348 (6,2%) |
| DHT interno | reads 377 / failures 376 (99,7%) — `frameTimeouts` 361 + `bitTimeouts` 15 | reads 5600 / failures 62 (1,1%) — `lastFailure` NONE |
| Stabilità (reboot / INT_WDT) | nessun INT_WDT, uptime di molte ore consecutive | nessun INT_WDT, uptime 58682 s (~16 h) |

Avvertenze sul confronto:

- i campioni non sono di pari ampiezza (377 vs 5600 letture per canale, ~15x): le percentuali della sessione DHTNEW sono molto più significative statisticamente;
- il canale interno è cambiato anche **fisicamente** (GPIO 4 → GPIO 13): il confronto misura l'insieme *libreria + pin*, non la sola libreria;
- entrambe le sessioni sono prive di reboot: la stabilità **non** distingue le due soluzioni.

### Conclusioni tecniche

- Il **DHT22 sul cavo lungo legge in modo molto affidabile**: 98,9% di successo su 5600 letture e ultima lettura `DHTLIB_OK`. Il quadro di "guasto duro e sistematico" della sessione 2026-09-16 **non si riproduce** in questa configurazione;
- **DHTNEW produce risultati nettamente migliori del fork Soluzione B** su entrambi i canali (interno: 99,7% → 1,1% di errori; esterno: 24% → 6,2%) con la **stessa architettura** di lettura (task dedicato, quarantena, finestra di validità, watchdog);
- **nessun nuovo `INT_WDT`** durante la prova: la migrazione della transazione fisica a DHTNEW non ha reintrodotto il rischio di reboot, coerente con l'ipotesi che il problema storico risiedesse nella sezione critica durante il frame;
- il limite della prova è l'**osservabilità ridotta**: senza `FrameDiag` e senza i contatori per categoria non è possibile dire *dove* si è perso il dato, solo *che* la lettura è fallita;
- i campi di telemetria del fork non sono stati rimossi dal modello dati: la scelta mantiene compilabili e confrontabili `main.cpp`, `web_manager.cpp` e `crash_logger.cpp` e rende immediato il ritorno alla classificazione per categoria.

### Decisione progettuale attuale

- **DHTNEW 0.5.5 è la libreria di lettura DHT attualmente in esercizio**: la prova si chiude con esito positivo su affidabilità e stabilità;
- il fork **"Soluzione B"** (`lib/DHT/`) resta nel repository **non modificato**: un eventuale rollback è immediato e non richiede di rimettere in discussione i budget temporali già validati;
- priorità invariata: **1. stabilità del firmware, 2. assenza di reboot, 3. affidabilità delle letture DHT**;
- nessuna modifica a budget temporali, timeout, quarantena, `readEnabled`, `DHTTask` o watchdog: la lettura resta confinata nel task dedicato (10 s, Core 0, priorità 1), fuori dal percorso critico;
- se in una futura indagine servisse la classificazione per fase del frame, la strada prevista è **riattivare temporaneamente il fork** (telemetria `FrameDiag` e contatori per categoria), non allargare i budget di DHTNEW;
- l'ipotesi "problema elettrico locale su GPIO 4" non è riaperta da questa prova: nella configurazione di test il canale interno è su GPIO 13 e si comporta in modo affidabile. Resta da confermare su sessioni più lunghe prima di considerare chiusa l'indagine.

---

## 2026-09-18 - Cleanup commenti e note obsolete (nessuna modifica di comportamento)

Intervento **solo su commenti e documentazione**, senza alcuna variazione di logica,
costanti, timeout, task, watchdog, librerie o API.

- rimossi i tag storici ormai fuorvianti dove non rappresentavano più lo stato reale:
  `TEST TEMPORANEO 2026-09-16` (pinout DHT interno/pulsante), `DIAG-TEMP`
  (quarantena 1 minuto), `DIAG-WDT` (`readEnabled`), `PROVA DHTNEW`;
- aggiornati i commenti che presentavano la migrazione DHTNEW come "prova sperimentale"
  o descrivevano il fork DHT come stato attuale: DHTNEW 0.5.5 è la libreria di lettura
  in esercizio, il fork resta come rollback non percorso;
- chiarito che i contatori di errore per categoria e i campi `FrameDiag`
  (`lastFrame*`, `frame*TimeoutPulses`, `maxPulseUs`) sono **INERTI a 0** con DHTNEW
  (presenti nel modello dati e in `/api/boot` solo per compatibilità);
- allineati i commenti sulle frequenze della livella: throttle `/level` a ~10 Hz (100 ms),
  `updateLevel()` a 40 ms (~25 Hz) con pagina livella aperta e 120 ms (~8 Hz) altrimenti;
- annotato che `updateDHTMaxUs` è misurato in `DHTTask` (non più in `SensorLoop`);
- la storia tecnica delle sessioni 2026-09-16 / 2026-09-17 resta invariata nelle sezioni
  cronologiche datate di questo documento.

## 2026-09-28 - Release WIFI-DIAG-01: baseline approvata per i test sul campo

**Release:** tag annotato `WIFI-DIAG-01` (branch `backup-pre-bootlog-fix`, commit `5be243c`).
Questa voce è **solo documentazione**: nessuna modifica al firmware, nessuna nuova
feature, nessuna nuova TODO tecnica, nessuna modifica alla configurazione PlatformIO.

### Contenuto della release

- **Monitoraggio `AP_PROBEREQRECVED` disabilitato**: `WIFI_MONITOR_ENABLE_PROBE_REQUEST = 0`
  in `wifi_monitor.h`. La mask eventi del driver resta al default (il driver continua a
  scartare l'evento) e **nessun** `WiFi.onEvent(ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED)`
  viene registrato; `probeRequestCount` / `lastProbeMs` restano nel modello dati e nelle
  API **ineri a 0**, solo per compatibilità con `/last_diag.txt` e `crashlog.txt`;
- **aggiunti `activeStaNum` e `activeStaMacs`**: quante stazioni sono associate *adesso*
  (`WiFi.softAPgetStationNum()`) e il loro elenco MAC, compilato da
  `wifiMonitorGetStaMacList()` nel buffer `WIFI_MONITOR_STA_MAC_LIST_BUF`;
- **aggiunte le metriche DMA `dmaMinFree` e `dmaLargestBlock`**: byte DMA liberi
  (`heap_caps_get_free_size(MALLOC_CAP_DMA)`) e blocco contiguo più grande
  (`heap_caps_get_largest_free_block(MALLOC_CAP_DMA)`), da cui il driver WiFi preleva i
  buffer RX. È la coppia che distingue "mai associato" / "associato e poi caduto" /
  "driver senza un blocco contiguo per i buffer RX";
- **migliorata la telemetria SoftAP**: i campi sono esposti sia in `/last_diag.txt` sia in
  `/api/boot` (`wifi.activeStaNum`, `wifi.activeStaMacs`, `wifi.dmaMinFree`,
  `wifi.dmaLargestBlock`); `activeStaNum` e `dmaLargestBlock` sono riportati anche nel
  blocco compatto del flight recorder (`crash_logger.cpp`);
- **persistenza automatica acqua**: `/last_liters.txt` viene riscritto quando
  `deltaLiters >= WATER_SAVE_DELTA_L` **oppure** quando è trascorso
  `WATER_SAVE_PERIOD_MS` con un delta sopra `WATER_SAVE_EPSILON_L`. I riferimenti
  `lastLitersSavedValue` / `lastLitersSaveMs` sono ricalcolati da `restoreLitersFromSD()`
  (avvio) e da `handleSetTank()` (set manuale), così il primo giro di `SensorLoop` non
  riscrive un file già identico;
- **perdita massima di acqua dopo reboot ridotta da circa 100 L a circa 0.5 L**.

### Verifiche eseguite

- Flash OK
- WiFi OK
- Web UI OK
- `activeStaNum` OK
- `activeStaMacs` OK
- `dmaMinFree` OK
- `dmaLargestBlock` OK
- `probeRequestEnabled=false` OK (booleano in `/api/boot`; lo stesso campo è stampato
  come `0` in `/last_diag.txt`)
- Persistenza acqua verificata (`lastLitersSaveMs` / `lastSavedLiters` in `/last_diag.txt`)

### Decisione

Build approvata per i test sul campo. Il tag `WIFI-DIAG-01` (commit `5be243c`) è la
**baseline** dei test: ogni anomalia successiva va riferita a quel commit.

### Prossima azione

Nessuna modifica firmware prevista. Raccogliere dati **solamente** in caso di:

- nuovo degrado WiFi;
- nuova anomalia reale osservata.

---

---
*Fine documento - Questo file è la memoria permanente del projecto e dovrebbe essere aggiornato ad ogni modifica significativa.*
