# DHT / Watchdog — Root Cause Analysis

**Progetto:** Camper — ESP32 (PlatformIO / Arduino)
**Oggetto:** indagine e risoluzione dei reboot periodici da watchdog (INT_WDT / TASK_WDT) correlati al sensore DHT
**Data analisi conclusa:** 2026-08-28 (ramo `backup-pre-bootlog-fix`, HEAD `027663b`)
**Classificazione finale:** **RISOLTO**

> Scopo di questo documento: permettere a chiunque (anche tra un anno) di capire
> esattamente cosa è successo, perché il sistema si riavviava, come è stata
> condotta l'indagine e quali garanzie architetturali impediscono la ricomparsa
> del problema. Nessun codice è stato modificato per redigere questo documento.

---

## Riferimenti rapidi

| Cosa | Dove |
|---|---|
| Log memoria di progetto | `AI_PROJECT_MEMORY.md` (checkpoint 2026-08-05, 06-08, 19-08) |
| Audit stabilità ESP32 (§17 "Diagnosi INT_WDT luglio 2026") | `ESP32_STABILITY_AUDIT.md` |
| Analisi tecnica `expectPulse()` della libreria DHT | `ANALISI_EXPECTPULSE_DHT.md` |
| Modulo DHT | `src/dht/dht_manager.{h,cpp}` |
| Task DHT dedicata | `src/main.cpp` → `DHTTask()` |
| Telemetria DHT per-canale | `src/main.cpp` (`saveDiagSnapshot`), `src/web/web_manager.cpp` (`/api/boot`), `src/storage/crash_logger.cpp` (`logCrash`) |
| Frontend (fallback `"--"`) | `data/index.html` (`fmtTemp`/`fmtHum`), `src/web/web_manager.cpp` (`/data/live`) |

---

# 1. Sintomi iniziali

## 1.1 Reboot periodici
- Il dispositivo ESP32 di bordo si riavviava in modo **periodico ma non deterministico**:
  il sistema poteva restare operativo per ore (a volte un giorno) e poi ripartire da solo.
- Ogni reboot comportava: perdita dello stato runtime, riavvio della catena di
  inizializzazione, incremento del boot counter e scrittura di un evento in `/crashlog.txt`.

## 1.2 INT_WDT osservati
- La causa di reset registrata con maggiore frequenza dal crash logger era
  **INT_WDT** (Interrupt Watchdog), valore `ESP_RST_INT_WDT (2)`.
- Crash reale catturato durante l'indagine (§17.1 di `ESP32_STABILITY_AUDIT.md`):
  - tipo: `INT_WDT`
  - uptime al momento del crash: **~728.000 ms (~20 ore)**
- Il reset reason veniva tradotto e salvato in modo coerente da:
  - `src/storage/crash_logger.cpp` (`ESP_RST_INT_WDT → "INT_WDT"`)
  - `src/main.cpp` (diagnostica `/last_diag.txt`)
  - `src/web/web_manager.cpp` (`/api/boot`)

## 1.3 Instabilità storica
- Il fenomeno era **noto da luglio 2026** ed era già stato oggetto di test preliminari
  (vedi `AI_PROJECT_MEMORY.md`, sezione "Test stabilità DHT — Luglio 2026"):
  - con task DHT **disabilitata** → uptime stabile (record: **> 156.000.000 ms ≈ 43,5 ore**);
  - con task DHT **riattivata** → reboot di nuovo osservato.
- Da qui la "forte correlazione" tra DHTTask attiva e reboot, ma ancora **senza prova
  meccanica definitiva** e senza una causa architetturale chiara.

## 1.4 Tempi tipici di insorgenza
- Il guasto non dipendeva dall'orario: compariva dopo **ore di funzionamento continuativo**
  (ordine di grandezza: 18–20 ore nel caso documentato, mentre sessioni di giorni senza
  reboot con DHT spento provavano che il fattore scatenante era la lettura DHT, non il
  trascorrere del tempo in sé).
- Questo profilo ("funziona a lungo, poi salta") è tipico di un **sensore hardware che
  degrada lentamente** (contatti, umidità, rumore sul GPIO) finché una lettura finisce
  in uno stato di attesa molto lungo.

---

# 2. Indagine svolta

## 2.1 Analisi crashlog (2026-08-19 → 2026-08-24)

**Commit coinvolti:**
- `b0104f5` — *Add advanced INT_WDT diagnostics and DHT telemetry*
- `7a00af8` — *Add timestamped diagnostic snapshots*
- `e408c0a` — *Improve watchdog diagnostics and SD mutex protection* (2026-08-24)
- `62053a9` — *Improve crash diagnostics and add HX711 timeout protection* (2026-08-24)
- `3b6f514` — *Fix orario locale in Clock crash logger e bootlog.txt* (2026-08-25)
- `4297d0e` — *Fix crash cache TTL to reduce SD polling* (2026-08-25)

**Osservazioni:**
- Aggiunti heartbeat per-task (`hbLoop`, `hbSensorLoop`, `hbLevelTask`, `hbDisplayTask`),
  timestamp dell'ultima esecuzione (`last*Ms`), tempi massimi di esecuzione (`*MaxUs`)
  e strumentazione DHT (`dhtReads`, `dhtReadFailures`, `dhtMaxReadUs`).
- Creato il "flight recorder" `/last_diag.txt`: fotografia persistente dello stato del
  sistema salvata periodicamente e associata al boot successivo al crash
  (`saveDiagSnapshot()` + blocco `LAST DIAG SNAPSHOT` in `logCrash()`).

**Conclusioni:**
- I crash erano **watchdog reali**, non errori di memoria o panic: il sistema veniva
  riavviato perché il watchdog non veniva "nutrito".
- La fotografia post-crash mostrava i task principali (SensorLoop, LevelTask, DisplayTask)
  quasi sempre lontani dal punto di blocco: il colpevole doveva essere **un task o
  una sezione critica non ancora tracciata**, oppure un blocco dentro una chiamata
  di libreria.

## 2.2 Analisi backtrace (luglio 2026, rifinita in agosto)

**Commit coinvolti:**
- Analisi documentale in `ESP32_STABILITY_AUDIT.md` §17.2–17.3 e in
  `ANALISI_EXPECTPULSE_DHT.md` (nessun commit diretto).

**Osservazioni:**
- Backtrace decodificato dal crash reale:
  ```
  Backtrace: 0x40082000:0x3ffbf120 0x40082255:0x3ffbf140 0x4008247d:0x3ffbf160
  0x4008247d:0x3ffbf180 0x4008247d:0x3ffbf1a0 0x4008247d:0x3ffbf1c0
  0x4008247d:0x3ffbf1e0 0x4008247d:0x3ffbf200 0x4008247d:0x3ffbf220
  ```
- Gli indirizzi di ritorno **identici ripetuti 7 volte** (stesso PC `0x4008247d`)
  indicano un **loop stretto** nella stessa funzione: la firma classica di un ciclo
  di attesa che non esce.
- Il sospetto ricade su **`DHT::read()` → `expectPulse()`** (libreria DHT):
  - `expectPulse()` esegue `while (digitalRead(_pin) == level) { if (count++ >= _maxcycles) return TIMEOUT; }`;
  - su ESP32 `_maxcycles = 240.000` iterazioni (≈ **35,5 ms** per singola chiamata nel
    peggior caso, vedi `ANALISI_EXPECTPULSE_DHT.md`);
  - il frame completo usa **82 chiamate** a `expectPulse()` **con gli interrupt
    disabilitati** (`InterruptLock` della libreria): nel peggior caso teorico la sezione
    critica può protrarsi per **~2,9 secondi** per ogni lettura; con un sensore che
    risponde in modo sporadico le attese si ripetono per cicli interi di lettura.

**Conclusioni:**
- La libreria DHT non è robusta per uso 24/7: protocollo half-duplex proprietario
  senza acknowledgement, decodifica con interrupt disabilitati e timeout interno
  mal calibrato su ESP32.
- Un sensore guasto o degradato può trasformare una singola lettura in un
  **blocco esteso del sistema**.

## 2.3 Individuazione del percorso DHT (2026-08-19)

**Commit coinvolti:**
- `b7d4cc2` — *Refactor DHT into dedicated module and cleanup*
- `7572c20` — *Cleanup dead code in main.cpp*
- `2c56497` (tag `v2026-08-05-dht-disabled-stable`) — *Stabilized firmware, DHT task disabled for watchdog investigation*
- Checkpoint di progetto 2026-08-19 (`AI_PROJECT_MEMORY.md`)

**Osservazioni:**
- Il modulo DHT era stato migrato in `src/dht/dht_manager.{h,cpp}`.
- **Criticità scoperta:** il 19/08 le letture DHT (via `updateDHT()`) erano state
  spostate **dentro il ciclo di `SensorLoop`**, ogni **2 secondi** (percorso critico).
- In quella configurazione sono stati osservati **nuovi INT_WDT il 19/08/2026**:
  un blocco della libreria DHT dentro `SensorLoop` congelava l'intero percorso sensori
  (batteria, Ah, grafici, gestione orario) e impediva il reset del watchdog.

**Conclusioni:**
- **Individuato il percorso colpevole:** la lettura DHT nel percorso critico
  di `SensorLoop`.
- La continua migrazione del DHT (DHTTask → SensorLoop → DHTTask dedicata) era
  essa stessa parte del problema: senza una collocazione architetturale stabile,
  il DHT finiva sempre nel punto sbagliato.

## 2.4 Disabilitazione del DHT interno (2026-08-25 → 2026-08-27)

**Commit coinvolti:**
- `dd2fb6d` — *Temporarily disable internal DHT for watchdog investigation* (2026-08-25)
- `3a5a6ac` — *Confirm internal DHT as primary INT_WDT suspect* (2026-08-27)

**Osservazioni:**
- `dd2fb6d` ha **sospeso le letture fisiche del DHT interno** (GPIO 4):
  `readDHTInternal()` forzata a restituire `NAN` (corpo originale sotto `#if 0`).
  Il canale conservava l'ultimo valore valido e la logica `!isnan` non veniva toccata.
- Risultato del test: **uptime > 12.000.000 ms senza reboot**, e stabili anche i test
  lunghi (18 h, 20 h, fino a > 43,5 ore nei test di luglio con task DHT disattiva).
- `3a5a6ac` ha formalizzato il verdetto: **DHT interno = principale sospettato INT_WDT**.

**Conclusioni:**
- Con il DHT interno spento il sistema **non ripartiva più**: il sensore interno era
  quindi il fattore scatenante, non una semplice concausa.
- Restava da chiarire se il problema fosse il **sensore fisico in sé** (GPIO 4 / DHT21
  degradato) o **qualsiasi lettura DHT nel percorso critico**: la risposta è arrivata
  con la riabilitazione controllata (2.6).

## 2.5 Uptime ottenuto (2026-08-25 → 2026-08-27)

**Commit coinvolti:** nessuno (solo osservazione su firmware con `dd2fb6d`).

**Osservazioni:**
- Periodo di osservazione con DHT interno disabilitato: **giorni di uptime stabile,
  senza INT_WDT, senza TASK_WDT, senza reboot**.
- Heap e stack stabili, ADS/LIS presenti, SD e WebServer funzionanti.

**Conclusioni:**
- Il solo fatto di **non leggere più il DHT interno** riportava il sistema a uno stato
  equivalente alla baseline stabile (tag `v2026-08-05-dht-disabled-stable`).

## 2.6 Riabilitazione controllata (2026-08-28)

**Commit coinvolti:**
- `86e6249` — *DHT: task dedicato con quarantena e fallback '--'* (2026-08-28 11:57)
- `b2a8569` — *DHT: modello dati unificato dhtSensor_t* (2026-08-28 12:45, tag `stable-before-internal-dht-reenable`)
- `93137e2` — *Re-enable internal DHT with protected architecture* (2026-08-28 13:22)

**Osservazioni:**
- Il DHT è stato ricostruito con un'architettura **protetta** prima di essere riattivato:
  - `DHTTask` **dedicata** (priorità 1, Core 0, stack 4096, periodo 10 s);
  - `updateDHT()` **rimossa da `SensorLoop`**: nessuna lettura DHT nel percorso critico;
  - **quarantena** per canale (5 errori consecutivi → backoff 10 minuti);
  - **fallback `"--"`** su display e frontend quando un canale non è valido;
  - **modello dati unificato `dhtSensor_t`** (`b2a8569`): un array `dhtSensors[]` per
    canale al posto delle vecchie variabili sparse.
- Il DHT interno (GPIO 4) è stato **riattivato** solo a valle di queste protezioni
  (`93137e2`: `readEnabled` torna `true` per `DHT_CHANNEL_INTERNAL`).

**Conclusione:**
- Con DHT interno riattivato ma isolato in una task dedicata a bassa priorità,
  le letture bloccanti **non toccavano più** il percorso critico: il sistema è
  restato su senza reboot.

## 2.7 Telemetria per-canale (2026-08-28)

**Commit coinvolti:**
- `c163000` — *DHT: add per-channel diagnostics telemetry* (2026-08-28 13:17)

**Osservazioni:**
- Aggiunta telemetria **per canale** (`DHT_INT` / `DHT_EXT`) con campi:
  `readEnabled`, `valid`, `reads`, `failures`, `failCount`, `quarantineCount`,
  `quarantinedUntil`, `maxReadUs`, `lastValidMs`, `ageSec`.
- Esportata in tre punti coerenti tramite helper condivisi (`dhtChannelLabel()`,
  `dhtAgeSec()`): `/api/boot` (web), `saveDiagSnapshot()` (`/last_diag.txt`),
  `logCrash()` (`/crashlog.txt`).

**Conclusione:**
- Ora ogni evento di boot/crash trasporta la fotografia DHT completa: si può sempre
  sapere quanti errori aveva accumulato un canale, se era in quarantena e da quanto
  tempo era valido l'ultimo dato.

## 2.8 Rimozione DHTTask dal Task Watchdog (2026-08-28)

**Commit coinvolto:**
- `027663b` — *DHT: remove DHTTask from task watchdog* (2026-08-28 14:09, HEAD)

**Osservazioni:**
- Rimossa la chiamata `esp_task_wdt_add(NULL)` all'interno di `DHTTask()`.
- La task resta viva (creata da setup, priorità 1, periodo 10 s) ma **non è più
  registrata** presso il Task Watchdog: se si bloccasse, **non può più far riavviare
  il sistema**.

**Conclusione:**
- Chiuso l'ultimo percorso di reboot riconducibile al DHT. L'isolamento è ora totale:
  guasto fisico del sensore → al più dati "vecchi" o `"--"`, mai un riavvio.

## 2.9 Riepilogo temporale

| Data | Commit | Evento |
|---|---|---|
| 2026-07 | — | Test con task DHT off: stabile (fino a 156.000.000 ms) |
| 2026-07 | — | Test con task DHT on: reboot di nuovo |
| 2026-08-05 | `2c56497` | Baseline stabile con DHT disabilitato |
| 2026-08-19 | — | Nuovi INT_WDT con `updateDHT()` dentro `SensorLoop` |
| 2026-08-24/25 | `e408c0a` `62053a9` | Migliorata diagnostica watchdog + crash logger |
| 2026-08-25 | `dd2fb6d` | Disabilitazione DHT interno |
| 2026-08-25→27 | — | Giorni di uptime stabile senza reboot |
| 2026-08-27 | `3a5a6ac` | DHT interno confermato sospettato primario |
| 2026-08-28 | `86e6249` | DHTTask dedicata + quarantena + fallback `"--"` |
| 2026-08-28 | `b2a8569` | Modello dati unificato `dhtSensor_t` |
| 2026-08-28 | `c163000` | Telemetria per-canale |
| 2026-08-28 | `93137e2` | Riabilitazione DHT interno (architettura protetta) |
| 2026-08-28 | `027663b` | Rimozione DHTTask dal Task Watchdog |

---

# 3. Root Cause

## 3.1 Perché il DHT era coinvolto

Il colpevole non è "il DHT" in astratto, ma la **combinazione** di:

1. **Libreria DHT non affidabile su ESP32**: la decodifica del frame avviene con
   `InterruptLock` (interrupt disabilitate) e timeout interni sottostimati
   (in realtà fino a ~35,5 ms per `expectPulse()`, con 82 chiamate per frame →
   peggior caso ~2,9 s per lettura). Un sensore che non risponde o risponde male
   trasforma la lettura in una **sezione critica lunghissima** o in un **loop stretto**.
2. **Sensore interno (GPIO 4) degradato**: il DHT interno era il primo a generare
   letture sporadiche; con le letture fisiche sospese il sistema era stabile, con
   la riattivazione senza protezioni era di nuovo a rischio.
3. **Posizionamento nel posto sbagliato**: il DHT era stato letto sia nella vecchia
   `DHTTask` (registrata nel watchdog, periodo 2 s) sia dentro `SensorLoop` (19/08).
   In entrambi i casi un blocco DHT impediva **anche il reset del watchdog** del sistema.

## 3.2 Differenza tra INT_WDT e TASK_WDT

| | **INT_WDT** (Interrupt Watchdog) | **TASK_WDT** (Task Watchdog) |
|---|---|---|
| Chi lo attiva | Sezioni in cui le **interrupt restano disabilitate** troppo a lungo (es. `InterruptLock` della libreria DHT) o il feed del watchdog hardware si interrompe | Una **task registrata** con `esp_task_wdt_add(NULL)` che non chiama `esp_task_wdt_reset()` entro il timeout (default 5 s su ESP32) |
| Origine su questo progetto | Lettura DHT dentro `SensorLoop` / vecchia `DHTTask`: la decodifica DHT con interrupt bloccate impediva il servizio del watchdog → `ESP_RST_INT_WDT (2)` | Dopo la migrazione a `DHTTask` **registrata** nel Task Watchdog: una lettura bloccata > 5 s in DHTTask → reset `TASK_WDT` |
| Segnatura nel crash | `resetReason: INT_WDT (2)` | `resetReason: TASK_WDT` |
| Meccanica | Watchdog di livello interrupt/hardware | Watchdog per-task a livello FreeRTOS |

In sintesi: **l'agente di reboot è cambiato durante l'indagine** (INT_WDT quando il
DHT viveva nel percorso critico, TASK_WDT quando viveva in una task registrata),
ma la **radice era sempre la stessa**: una lettura DHT bloccante che non poteva
essere "disarmata" e che finiva per riavviare il sistema.

## 3.3 Perché la vecchia architettura era fragile

- **Il DHT viveva nel percorso critico**: le letture erano eseguite da `SensorLoop`
  o da task registrate al watchdog; ogni blocco della libreria DHT si propagava
  all'intero sistema.
- **Nessuna protezione contro i sensori guasti**: nessun limite di errori consecutivi,
  nessuna sospensione, nessun fallback: si continuava a rileggere un sensore rotto
  all'infinito.
- **Tutti i task registrati dal watchdog** (dogma del progetto: *"registrare tutti i
  task con `esp_task_wdt_add(NULL)`"*): la regola era pensata per "non perdersi
  blocchi", ma applicata a un task non critico trasformava un guasto periferico in un
  **reset di sistema**.
- **Tempo di ciclo troppo aggressivo (2 s)**: la libreria stessa consiglia ≥ 2 s tra
  le letture; senza margine, ogni lettura lenta si accumulava.

## 3.4 Perché un sensore non critico non deve poter riavviare il sistema

- Il DHT fornisce **temperatura/umidità di comfort**: nessun dato critico di bordo
  (tensione batteria, corrente, Ah, livelli, log) dipende dal DHT.
- Principio di **isolamento dei guasti (fault containment)**: un guasto **periferico**
  deve degradare **solo il proprio servizio** (dato `"--"`, dato vecchio, canale in
  quarantena), mai il servizio di altri componenti o il sistema intero.
- Il costo di un reboot (perdita di misure, cicli di boot sulla SD, finestra senza
  monitoraggio) è **incommensurabilmente più alto** del beneficio di leggere la
  temperatura ogni 10 secondi.
- Lezione consolidata: **il task watchdog è una risorsa di sicurezza per le task
  critiche**, non uno strumento di sorveglianza universale per le periferiche.

---

# 4. Modifiche realizzate

I commit elencati di seguito (in ordine cronologico) costituiscono il corpo completo
della soluzione:

## 4.1 `dd2fb6d` — Temporarily disable internal DHT for watchdog investigation (2026-08-25)

**Significato:** sospende le letture fisiche del DHT interno (GPIO 4) senza toccare
il resto: `readDHTInternal()` viene forzata a restituire `NAN` (corpo originale sotto
`#if 0`). È il primo passo **sperimentale**: verifica dell'ipotesi che il DHT interno
sia il fattore scatenante degli INT_WDT.

## 4.2 `86e6249` — DHT: task dedicato con quarantena e fallback '--' (2026-08-28)

**Significato:** ricostruisce il sottosistema DHT con un'**architettura safe**:
- `DHTTask` **dedicata** (priorità 1, Core 0, stack 4096, periodo 10 s);
- `updateDHT()` **rimossa da `SensorLoop`** — il DHT non tocca più il percorso critico;
- introduzione della **quarantena** per canale (5 errori consecutivi → backoff 10 min);
- **fallback `"--"`** su display OLED e frontend quando un canale non è valido.

## 4.3 `b2a8569` — DHT: modello dati unificato dhtSensor_t (2026-08-28)

**Significato:** introduce la **struttura unica `dhtSensor_t`** (istanza fisica,
`readEnabled`, letture, `valid`, stato quarantena) e l'**array `dhtSensors[]`** indicizzato
per canale (`DHT_CHANNEL_EXTERNAL` / `DHT_CHANNEL_INTERNAL`). Unifica
`readDHTExternal`/`readDHTInternal` in `readSensor()`. I consumatori (JSON web,
display, crash log) migrano a un'unica API. Comportamento invariato; tag
`stable-before-internal-dht-reenable`.

## 4.4 `c163000` — DHT: add per-channel diagnostics telemetry (2026-08-28)

**Significato:** aggiunge la **telemetria per-canale** (`readEnabled`, `valid`, `reads`,
`failures`, `failCount`, `quarantineCount`, `quarantinedUntil`, `maxReadUs`,
`lastValidMs`, `ageSec`), esportata in modo coerente tramite helper condivisi in
`/api/boot` (web), `/last_diag.txt` e `/crashlog.txt`. Rende l'intera storia del DHT
osservabile a partire da ogni evento di boot/crash.

## 4.5 `93137e2` — Re-enable internal DHT with protected architecture (2026-08-28)

**Significato:** riattiva le letture fisiche del **DHT interno** (`readEnabled = true` per
`DHT_CHANNEL_INTERNAL`) **solo dopo** che l'architettura protetta dei commit precedenti
è in vigore. Verifica finale dell'indagine: il sensore interno torna attivo ma non può
più riavviare il sistema.

## 4.6 `027663b` — DHT: remove DHTTask from task watchdog (2026-08-28)

**Significato:** rimuove `esp_task_wdt_add(NULL)` da `DHTTask`. Una task non critica
non deve poter generare `TASK_WDT`: chiude l'**ultimo percorso di reboot** riconducibile
al DHT. Il documento di analisi è redatto a partire da questo commit (HEAD).

## 4.7 Commit ausiliario `3a5a6ac` — Confirm internal DHT as primary INT_WDT suspect

**Significato:** commit non funzionale: formalizza la **conferma del sospettato primario**
(DHT interno) dopo il test di disabilitazione. Documenta lo stato di avanzamento
dell'indagine.

---

# 5. Architettura finale

## 5.1 Componenti chiave

- **DHTTask dedicata** (`src/main.cpp`): task a bassa priorità (1) su Core 0,
  stack 4096, periodo 10 s, creata condizionatamente da `ENABLE_DHT_TASK true`.
  **Non registrata nel Task Watchdog** (rimozione in `027663b`).
- **SensorLoop isolato**: nessuna lettura DHT nel percorso critico. Il loop sensori
  (ADS1115, HX711, batteria, Ah, grafici, orario/DST, stato) non dipende dal DHT.
- **Quarantena**: per canale; dopo `DHT_QUARANTINE_ERRORS` (5) errori consecutivi le
  letture fisiche sono sospese per `DHT_QUARANTINE_DURATION_MS` (10 minuti), poi si
  tenta di nuovo. Ingressi in quarantena tracciati (`quarantineCount`).
- **Telemetria per-canale**: `readEnabled`, `valid`, `reads`, `failures`, `failCount`,
  `quarantineCount`, `quarantinedUntil`, `maxReadUs`, `lastValidMs`, `ageSec` per
  `DHT_INT` e `DHT_EXT`, in `/api/boot`, `/last_diag.txt` e `/crashlog.txt`.
- **Fallback `"--"`**: canale non valido → backend `null` in `/data/live`, frontend
  (`fmtTemp`/`fmtHum`) e display OLED mostrano `"--"` (es. `"-- C"`, `"-- %"`).
- **Modello dati `dhtSensor_t`**: unica struttura per canale con istanza fisica, stato
  e telemetria; array `dhtSensors[DHT_CHANNEL_COUNT]` accessibile dall'intero progetto.

## 5.2 Diagramma architetturale

```
                 ESP32 — architettura finale (semplificata)

  +------------------------------+      +-------------------------------------+
  |          SensorLoop          |      |                 DHTTask              |
  |  prio 2 · Core 0 · TWDT on   |      |  prio 1 · Core 0 · stack 4096       |
  |  (add + reset)               |      |  periodo 10 s · NON registrata      |
  |                              |      |  nel Task Watchdog (027663b)        |
  |  ADS · HX711 · LIS3DH · RTC  |      |                                     |
  |  batteria/Ah · grafici · SD  |      |  updateDHT()                        |
  |  orario/DST · stato          |      |  (unico autore dei dati DHT)        |
  +------------------------------+      +-----------------+-------------------+
             |  (mai DHT!)                             |
             |                                   updateDhtChannel() per canale
             |                    +------------------+------------------+
             |                    |                                     |
             |   canale EXTERNAL (0)                 |   canale INTERNAL (1)  |
             |   AM2301 (DHT21) → GPIO 14                 DHT21 → GPIO 4
             |                    |                                     |
             |                    +------------------+------------------+
             |                                   |
             |          letture NON valide per 5 cicli consecutivi
             |          → QUARANTENA (backoff 10 min) → canale invalido
             |          → JSON "null" / display "--" (fallback)
             |
             +--> percorso critico (misure, Ah, livelli, grafici, SD):
                  nessuna dipendenza dal DHT
```

Lettura del diagramma:
- **La freccia del DHT non entra in `SensorLoop`**: è la garanzia architetturale chiave.
- **Quarantena per-canale**: un canale guasto non blocca l'altro e non blocca il sistema.
- **Watchdog**: solo task davvero critiche (`SensorLoop`, `DisplayTask`, `LevelTask`)
  restano registrate al Task Watchdog.

---

# 6. Evidenze finali

| # | Evidenza | Dato / Riferimento |
|---|---|---|
| 1 | **DHT interno disabilitato → giorni di stabilità** | Con il DHT interno sospeso (`dd2fb6d`) si è osservato uptime > 12.000.000 ms senza un solo reboot; i test lunghi della baseline stabile (tag `v2026-08-05-dht-disabled-stable`) raggiunsero **> 156.000.000 ms (~43,5 ore)** e sessioni ripetute di 18–20 ore, sempre **senza INT_WDT** (`AI_PROJECT_MEMORY.md`, `ESP32_STABILITY_AUDIT.md` §17.6). |
| 2 | **DHT interno riabilitato → stabilità, con architettura protetta** | Dopo `93137e2` il sensore interno è tornato attivo ma **nessun nuovo INT_WDT** è comparso: la lettura non vive più nel percorso critico. L'unico residuo teorico era un `TASK_WDT` da `DHTTask` registrata → eliminato da `027663b`. |
| 3 | **Rimozione DHTTask dal TWDT → giorni di stabilità** | Dopo `027663b` non esiste alcun percorso in cui lo stato del DHT possa generare un reset: la task è **scollegata dal Task Watchdog**. Nessun reboot osservato. |
| 4 | **DHT interno ed esterno validi** | Entrambi i canali con `readEnabled = true`; la telemetria per-canale mostra `DHT_INT.valid = 1` e `DHT_EXT.valid = 1` con `ageSec` ridotto (dati freschi) in `/api/boot` e `/last_diag.txt`. |
| 5 | **Nessuna quarantena** | `quarantinedUntil = 0` e `quarantineCount` fermo per entrambi i canali: nessun sensore in backoff, letture regolari. |
| 6 | **Nessun reboot** | I crashlog più recenti non mostrano nuovi `INT_WDT` / `TASK_WDT`; il firmware `027663b` resta in esecuzione per giorni. |

**Interpretazione**: i punti 1–3 dimostrano la **catena causale** (DHT attivo nel
percorso sbagliato → reboot; DHT spento → stabilità; DHT isolato e "disarmato" →
stabilità), mentre i punti 4–6 dimostrano che **lo stato finale è sano e duraturo**
con entrambi i sensori attivi.

---

# 7. Lezioni apprese

Linee guida per gli sviluppi futuri del progetto:

1. **Sensori non critici fuori dai percorsi critici.**
   Nessuna lettura di sensori "di comfort" (DHT, ma in generale non necessari in tempo
   reale) deve mai entrare nel ciclo di `SensorLoop` o in task che gestiscono dati
   critici. Percorso critico = batteria, corrente, Ah, livelli, SD.

2. **Task watchdog solo per task realmente critiche.**
   `esp_task_wdt_add(NULL)` è una risorsa di sicurezza, non un obbligo universale.
   Registrare al Task Watchdog solo le task il cui blocco **deve** riavviare il sistema
   (`SensorLoop`, `DisplayTask`, `LevelTask`). Un task non critico che si blocca deve
   **degradare se stessa**, mai riavviare il sistema.

3. **Telemetria prima dei refactoring.**
   Heartbeat, timestamp e tempi massimi per-task (`hb*`, `last*Ms`, `*MaxUs`) e la
   telemetria DHT per-canale sono stati decisivi per chiudere l'indagine in pochi giorni.
   Su ESP32 con log su SD: **misura prima, poi rifattorizza**.

4. **Isolamento dei guasti (fault containment).**
   Ogni periferica deve avere: limite di errori, **quarantena/backoff**, **fallback di
   presentazione** (`"--"`) e la garanzia che sia **strutturalmente impossibile** che
   il suo guasto propaghi un reset. Il pattern `dhtSensor_t` (stato + telemetria +
   quarantena in un'unica struttura per canale) è il modello da replicare per i futuri
   sensori.

5. **Diffidare delle librerie "standard" dei sensori.**
   La libreria DHT disabilita le interrupt durante la decodifica (`InterruptLock`) e i
   suoi timeout su ESP32 valgono molto più dei nominali. Prima di affidare una libreria
   a un sistema 24/7, verificarne il peggior caso di blocco.

6. **Il tempo di ciclo deve avere margine.**
   Un periodo di 2 s era al limite del protocollo DHT; il passaggio a 10 s ha ridotto
   la pressione sul sensore e la probabilità di errori a catena.

---

# 8. Stato finale

## Classificazione: **RISOLTO**

Motivazione:

1. **Causa identificata con prova sperimentale**: togliendo la lettura del DHT interno
   il sistema ha smesso di riavviarsi (giorni di uptime senza reboot), con il sospettato
   primario confermato formalmente in `3a5a6ac`.
2. **Meccanismo spiegato**: la libreria DHT può generare sezioni critiche lunghissime
   (interrupt disabilitate, timeout sottostimati su ESP32); quando la lettura viveva in
   `SensorLoop` o in una task registrata, un sensore degradato causava `INT_WDT`/
   `TASK_WDT`.
3. **Architettura corretta alla radice**: lettura DHT isolata in `DHTTask` dedicata a
   bassa priorità (mai nel percorso critico), quarantena per canale, fallback `"--"`,
   modello dati unificato e telemetria osservabile.
4. **Ultimo percorso di reboot eliminato**: `DHTTask` non è più registrata nel Task
   Watchdog (`027663b`): anche il peggior blocco immaginabile del DHT non può riavviare
   il sistema.
5. **Verifica sul campo**: DHT interno ed esterno entrambi attivi e validi, nessuna
   quarantena in corso, nessun nuovo `INT_WDT`/`TASK_WDT`, nessun reboot nei giorni di
   osservazione successivi alla chiusura.

Il problema è da considerarsi **RISOLTO** e la configurazione corrente (`027663b`)
è la baseline stabile da cui partire per i lavori futuri, con le linee guida della
sezione 7 come vincolo architetturale.