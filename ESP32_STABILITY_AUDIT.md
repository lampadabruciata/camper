# ESP32 Firmware Stability Audit Report

**Data Audit:** 2026-07-10  
**Progetto:** Camper ESP32 Monitoring System  
**Versione:** main.cpp (3168 righe)

---

## Executive Summary

Questo audit ha identificato **14 problemi critici** e **10 problemi di medie/alta gravità**. Il firmware presenta una buona architettura multi-task con mutex e watchdog, ma ha aree critiche di miglioramento per la stabilità a lungo termine.

---

## 1. Memory Leak

### 1.1 DynamicJsonDocument non deallocati correttamente
- **Gravità:** CRITICA
- **File:** `src/web/web_manager.cpp`
- **Descrizione:** I `DynamicJsonDocument` vengono creati con `new` (allocazione heap) ma non esiste una chiamata esplicita a `json.clear()` o deallocazione. ArduinoJson su ESP32 con heap limitata può accumulare frammentazione.
- **Probabilità:** Alta (si verifica ad ogni richiesta HTTP)
- **Correzione:** Usare `StaticJsonDocument` con dimensioni fisse o aggiungere `json.clear()` dopo `serializeJson()`.

### 1.2 String objects in web responses
- **Gravità:** CRITICA
- **File:** `src/web/web_manager.cpp` (tutte le funzioni handleData*)
- **Descrizione:** Uso massiccio di `String out; serializeJson(json, out)` creando oggetti String temporanei ad ogni richiesta. La classe String su ESP32 è nota per causare heap fragmentation.
- **Probabilità:** Molto alta
- **Correzione:** Usare `char buffer[512]` con `serializeJson(json, buffer, sizeof(buffer))` o `serializeJson(json, server.client().remoteIP())`.

### 1.3 malloc in restoreGraphFromSD
- **Gravità:** MEDIA
- **File:** `src/storage/graph_storage.cpp:147`
- **Descrizione:** `malloc(fileSize)` alloca ~100KB per il buffer di restore. Se il puntatore è NULL, la funzione esce ma non c'è cleanup preventivo.
- **Probabilità:** Media (dipende dallo heap disponibile)
- **Correzione:** Verificare `heap_caps_get_free_size(MALLOC_CAP_8BIT)` prima del malloc, usare SPIRAM se disponibile.

---

## 2. Heap Fragmentation

### 2.1 Uso prolungato di String
- **Gravità:** CRITICA
- **File:** `src/web/web_manager.cpp`, `src/gas/gas_manager.cpp`, `src/storage/crash_logger.cpp`
- **Descrizione:** L'uso sistematico di `String` per JSON, parsing file SD, e log di crash causa frammentazione progressiva dell'heap.
- **Probabilità:** Molto alta
- **Correzione:** Sostituire con `char[]` e funzioni C standard.

### 2.2 DynamicJsonDocument con capacità elevate
- **Gravità:** ALTA
- **File:** `src/web/web_manager.cpp`
- **Descrizione:** `DynamicJsonDocument json(16000)` in `handleDataBattery()` richiede 16KB di heap contigui. Con più richieste simultanee può esaurire la memoria.
- **Probabilità:** Media
- **Correzione:** Ridurre le dimensioni o usare streaming JSON.

---

## 3. Uso della classe String

### 3.1 Parsing JSON con String
- **Gravità:** CRITICA
- **File:** `src/web/web_manager.cpp:89-91, 105-107, ecc.`
- **Descrizione:** Ogni endpoint web usa `String out` per serializzare JSON. Questo è il pattern più pericoloso su ESP32.
- **Probabilità:** Molto alta
- **Correzione:** Vedi 1.2

### 3.2 Parsing file SD con String
- **Gravità:** ALTA
- **File:** `src/gas/gas_manager.cpp:168`, `src/water/water_manager.cpp:70`, `src/level/level_manager.cpp:102`
- **Descrizione:** `String s = f.readStringUntil('\n')` legge l'intero file in RAM.
- **Probabilità:** Media
- **Correzione:** Usare `fgets()` o buffer di dimensione fissa.

### 3.3 Crash logger con String
- **Gravità:** ALTA
- **File:** `src/storage/crash_logger.cpp:60-81, 128-173`
- **Descrizione:** Costruzione di log con molti oggetti String concatenati.
- **Probabilità:** Media
- **Correzione:** Usare `snprintf()` con buffer.

---

## 4. Stack Overflow Potenziali

### 4.1 SensorLoop task con stack limitato
- **Gravità:** ALTA
- **File:** `src/main.cpp:1642-1650`
- **Descrizione:** `SensorLoop` ha 12288 bytes di stack. Il task esegue operazioni complesse con array locali, mediane, e chiamate a funzioni che usano buffer.
- **Probabilità:** Media
- **Correzione:** Aumentare lo stack a 14000-16000 bytes o spostare buffer locali in globali.

### 4.2 DisplayTask con drawOLED
- **Gravità:** MEDIA
- **File:** `src/main.cpp:2084-2121`
- **Descrizione:** `drawOLED()` è una funzione grande con molti buffer locali `char buf[32]`, `char buf1[10]`. Con 4096 bytes di stack potrebbe avvicinarsi al limite.
- **Probabilità:** Bassa-media
- **Correzione:** Verificare `uxTaskGetStackHighWaterMark()` in produzione.

---

## 5. Race Condition

### 5.1 Accesso non protetto a variabili volatile
- **Gravità:** CRITICA
- **File:** `src/main.cpp`
- **Descrizione:** Variabili `volatile` come `vBattery`, `solarAmps`, `tempInt` vengono lette in `loop()` e in task senza protezione. `volatile` non è sufficiente per thread-safety.
- **Probabilità:** Alta
- **Correzione:** Usare `atomic` o copie locali con mutex.

### 5.2 pulseCount in interrupt
- **Gravità:** ALTA
- **File:** `src/main.cpp:270-278, 1870-1879`
- **Descrizione:** `pulseCount` è `volatile unsigned long` ma l'accesso in `SensorLoop` con `detachInterrupt/attachInterrupt` non è atomico. Il `pulseCount = 0` potrebbe essere interrotto.
- **Probabilità:** Media
- **Correzione:** Usare `portENTER_CRITICAL(&spinlock)` o `atomic<uint32_t>`.

### 5.3 currentPage in loop()
- **Gravità:** MEDIA
- **File:** `src/main.cpp:133, 2550, 2573-2557`
- **Descrizione:** `currentPage` è modificato in `loop()` e letto in `DisplayTask` senza protezione.
- **Probabilità:** Bassa
- **Correzione:** Usare `atomic<int>` o mutex.

---

## 6. Queue Overflow

### 6.1 prefsQueue senza gestione overflow
- **Gravità:** MEDIA
- **File:** `src/main.cpp:1190`
- **Descrizione:** `xQueueCreate(10, sizeof(PrefsMessage))` ha capacità 10. Se arrivano più di 10 messaggi in 200ms, vengono persi.
- **Probabilità:** Bassa
- **Correzione:** Aumentare capacità a 20 o usare `xQueueSend` con timeout 0 e gestire il fallimento.

---

## 7. Deadlock

### 7.1 Ordine mutex non coerente
- **Gravità:** ALTA
- **File:** `src/main.cpp`, `src/gas/gas_manager.cpp`, `src/level/level_manager.cpp`
- **Descrizione:** In `i2cRecoverBus()` (line 603-621) si tenta di prendere `i2cMutex` ma la funzione può essere chiamata quando il mutex è già stato parzialmente acquisito. Rischio deadlock se `takeMutexWarn` fallisce.
- **Probabilità:** Bassa
- **Correzione:** Rivedere la logica di recovery, usare `xSemaphoreTakeRecursive` o evitare chiamate nidificate.

### 7.2 SD access in crash logger
- **Gravità:** MEDIA
- **File:** `src/storage/crash_logger.cpp:44-57`
- **Descrizione:** `logCrash()` chiama `initSD()` che tenta di prendere `sdMutex`. Se il crash avviene durante un'operazione SD, si può verificare deadlock.
- **Probabilità:** Bassa
- **Correzione:** Usare un mutex separato per crash log o `xSemaphoreTake` con timeout 0.

---

## 8. Watchdog Risk

### 8.1 DHTTask con delay bloccante
- **Gravità:** MEDIA
- **File:** `src/main.cpp:651-670`
- **Descrizione:** `DHT.readTemperature()` può bloccare fino a 2 secondi. Il task ha WDT ma se entra in stato di errore può non resettare il watchdog.
- **Probabilità:** Bassa
- **Correzione:** Aggiungere timeout o flag di errore.

### 8.2 restoreGraphFromSD senza WDT
- **Gravità:** ALTA
- **File:** `src/storage/graph_storage.cpp:84-262`
- **Descrizione:** La funzione non aggiunge il task al WDT (`esp_task_wdt_add(NULL)`). Con file grandi può superare il timeout WDT.
- **Probabilità:** Media
- **Correzione:** Aggiungere `esp_task_wdt_reset()` nei loop di parsing.

---

## 9. Accessi Concorrenti SD

### 9.1 SD.write senza protezione
- **Gravità:** CRITICA
- **File:** `src/main.cpp:2342-2358, 2360-2389`
- **Descrizione:** `saveHourlyAhToSD()` e `saveAhToSD()` usano `takeMutexWarn` ma non c'è protezione se `SD.open()` fallisce dopo il mutex.
- **Probabilità:** Media
- **Correzione:** Verificare `f` prima di ogni operazione, usare RAII pattern.

### 9.2 File handle non chiusi in caso di errore
- **Gravità:** ALTA
- **File:** `src/storage/graph_storage.cpp:20-26`
- **Descrizione:** Se `SD.open()` fallisce, il mutex viene rilasciato ma non c'è logging dell'errore.
- **Probabilità:** Bassa
- **Correzione:** Aggiungere logging errori SD.

---

## 10. Problemi WiFi

### 10.1 Nessun reconnect WiFi
- **Gravità:** MEDIA
- **File:** `src/main.cpp:1400-1418`
- **Descrizione:** WiFi è configurato solo in `setup()`. Se l'AP si disconnette, non c'è meccanismo di riconnessione.
- **Probabilità:** Bassa (AP software non si disconnette mai)
- **Correzione:** Aggiungere check periodico in `loop()`.

### 10.2 DNS server senza protezione
- **Gravità:** BASSA
- **File:** `src/main.cpp:1575-1585`
- **Descrizione:** `dnsServer.processNextRequest()` è chiamato senza protezione. Non è critico ma può causare problemi se il DNS server è in uno stato inconsistente.
- **Probabilità:** Molto bassa
- **Correzione:** Aggiungere protezione mutex.

---

## 11. Problemi WebServer

### 11.1 Endpoint /level con String
- **Gravità:** CRITICA
- **File:** `src/main.cpp:1550-1582`
- **Descrizione:** L'endpoint `/level` crea `DynamicJsonDocument` e `String out` in ogni chiamata. Con polling a 5Hz può esaurire la heap.
- **Probabilità:** Alta
- **Correzione:** Vedi 1.1, 1.2.

### 11.2 Nessun rate limiting su endpoint pesanti
- **Gravità:** ALTA
- **File:** `src/web/web_manager.cpp:185-238`
- **Descrizione:** `handleDataBattery()` e `handleDataSolar()` non hanno rate limiting. Un client maligno può esaurire la heap.
- **Probabilità:** Media
- **Correzione:** Aggiungere rate limiting con timestamp.

### 11.3 server.arg() senza validazione
- **Gravità:** MEDIA
- **File:** `src/main.cpp:1037-1088`
- **Descrizione:** `server.arg("kg").toFloat()` non valida che l'argomento sia un numero valido.
- **Probabilità:** Bassa
- **Correzione:** Validare input con regex o controlli numerici.

---

## 12. Possibili Reboot Casuali

### 12.1 Brownout non gestito
- **Gravità:** ALTA
- **File:** `src/main.cpp:1195-1207`
- **Descrizione:** Il codice logga `BROWNOUT` ma non implementa protezioni (delay, riduzione funzionalità).
- **Probabilità:** Media (dipende dalla alimentazione)
- **Correzione:** Implementare strategia di fallback su brownout (es. disattivare display, ridurre frequenza sensori).

### 12.2 Stack overflow non rilevato
- **Gravità:** ALTA
- **File:** `src/main.cpp:2587-2603`
- **Descrizione:** `monitorTaskStacks()` logga lo stack ma non agisce se il watermark è basso.
- **Probabilità:** Media
- **Correzione:** Aggiungere azioni correttive se stack < 500 bytes.

---

## 13. Error Handling Mancante

### 13.1 DHT read senza timeout
- **Gravità:** MEDIA
- **File:** `src/main.cpp:656-665`
- **Descrizione:** Se il sensore DHT non risponde, `readTemperature()` può bloccare indefinitamente.
- **Probabilità:** Media
- **Correzione:** Aggiungere timeout o flag di errore.

### 13.2 SD.remove senza verifica
- **Gravità:** BASSA
- **File:** `src/storage/graph_storage.cpp:73, 136`
- **Descrizione:** `SD.remove()` fallisce silenziosamente se il file non esiste.
- **Probabilità:** Bassa
- **Correzione:** Verificare il risultato e loggare.

### 13.3 ads.readADC_Differential_0_1() senza controllo errore
- **Gravità:** ALTA
- **File:** `src/main.cpp:739, 807, 907`
- **Descrizione:** I valori ADC vengono usati direttamente senza verificare se la lettura è andata a buon fine.
- **Probabilità:** Media
- **Correzione:** Controllare valori di ritorno e implementare retry.

---

## 14. Resource Leak

### 14.1 Preferences non chiusi in caso di errore
- **Gravità:** BASSA
- **File:** `src/main.cpp:1329-1338`
- **Descrizione:** Se `prefs.begin()` fallisce, non c'è `prefs.end()`.
- **Probabilità:** Molto bassa
- **Correzione:** Aggiungere controllo errori.

### 14.2 File handle in crash logger
- **Gravità:** MEDIA
- **File:** `src/storage/crash_logger.cpp:178-183`
- **Descrizione:** Se `SD.open("/crashlog.txt", FILE_WRITE)` fallisce, il mutex non viene rilasciato.
- **Probabilità:** Bassa
- **Correzione:** Aggiungere controllo e `xSemaphoreGive` nel ramo di errore.

---

## Tabella Riassuntiva

| # | Problema | Gravità | File | Probabilità |
|---|----------|---------|------|-------------|
| 1 | DynamicJsonDocument memory leak | CRITICA | web_manager.cpp | Alta |
| 2 | String class usage | CRITICA | web_manager.cpp, gas_manager.cpp, crash_logger.cpp | Molto alta |
| 3 | Heap fragmentation | CRITICA | web_manager.cpp | Molto alta |
| 4 | Race condition variabili volatile | CRITICA | main.cpp | Alta |
| 5 | SD concurrent access | CRITICA | main.cpp, graph_storage.cpp | Media |
| 6 | Stack overflow SensorLoop | ALTA | main.cpp | Media |
| 7 | Deadlock mutex order | ALTA | main.cpp, crash_logger.cpp | Bassa |
| 8 | Watchdog risk restoreGraph | ALTA | graph_storage.cpp | Media |
| 9 | Brownout no handling | ALTA | main.cpp | Media |
| 10 | Stack monitoring no action | ALTA | main.cpp | Media |
| 11 | Web endpoint no rate limit | ALTA | web_manager.cpp | Media |
| 12 | DHT no timeout | MEDIA | main.cpp | Media |
| 13 | SD file handle leak | MEDIA | crash_logger.cpp | Bassa |
| 14 | Queue overflow risk | MEDIA | main.cpp | Bassa |
| 15 | Falso rollover giornaliero (prevHour > currentHour) | ALTA | main.cpp | Media |
| 16 | lastHourlyDay dichiarata ma non utilizzata | MEDIA | main.cpp | Media |

---

## Raccomandazioni Prioritarie

1. **Immediata:** Sostituire tutti gli oggetti `String` con buffer C e `snprintf()`
2. **Immediata:** Usare `StaticJsonDocument` o aggiungere `json.clear()` dopo ogni uso
3. **Alta priorità:** Aggiungere protezione atomica per variabili condivise tra task
4. **Alta priorità:** Implementare rate limiting su tutti gli endpoint web
5. **Media priorità:** Aggiungere `esp_task_wdt_reset()` in operazioni lunghe SD
6. **Media priorità:** Aumentare lo stack del task SensorLoop a 14000+ bytes
7. **Bassa priorità:** Implementare strategia di fallback per brownout

---

## 15. Analisi File `/ah_day.bin`

### 15.1 Struttura dati (396 byte)
- **Header:** `AhDayHeader` (year, month, day) - 4 byte
- **Dati orari:** 4 array da 24 float ciascuno - 384 byte
- **Totali:** 2 float per totali solari - 8 byte

### 15.2 Funzione `restoreAhDayFromSD()`
- **Protezione data:** Verifica automatica anno/mese/giorno con RTC
- **Scarto automatico:** File di giorni diversi vengono ignorati
- **Ripristino completo:** Include anche `solAhHourlyYesterday` e totali

### 15.3 Stabilità
- **Mutex SD:** Protetto da `sdMutex`
- **Controlli di integrità:** Controlli espliciti su ogni `f.read()`
- **Gestione errori:** Rilascio mutex anche in caso di errore

---

## 16. Problemi Residui - Rollover Giornaliero

### 16.1 Falso rollover giornaliero (prevHour > currentHour)
- **Gravità:** ALTA
- **File:** `src/main.cpp:1752`
- **Descrizione:** Il meccanismo `prevHour > currentHour` non distingue un vero rollover giornaliero da un cambio manuale dell'orario all'indietro (es. 14:00 → 10:00 tramite `/setTime`). Questo genera erroneamente un `dayRolled = true`, spostando i dati del giorno corrente in "ieri" e azzerando i valori orari.
- **Impatto:** Dopo un cambio manuale orario all'indietro, i dati orari vengono spostati in `solAhHourlyYesterday` e gli array `Today` vengono azzerati, causando perdita di dati.
- **Probabilità:** Media (dipende dall'uso dell'endpoint `/setTime`)
- **Correzione:** Implementare tracciamento del giorno tramite `lastHourlyDayEpoch` o `currentDay` per verificare se è realmente passata la mezzanotte.

### 16.2 Variabile lastHourlyDay dichiarata ma non utilizzata
- **Gravità:** MEDIA
- **File:** `src/main.cpp:418`
- **Descrizione:** La variabile `int lastHourlyDay = -1` è dichiarata ma mai utilizzata nel codice. I file `/ah_day.bin` e `/ah_hours.bin` non contengono informazioni sulla data, quindi i salti temporali superiori a 24 ore non sono gestiti esplicitamente.
- **Impatto:** Dopo un reboot, se il dispositivo è rimasto spento per più di un giorno, i dati potrebbero essere sovrascritti in modo imprevedibile. Il tracciamento del giorno è affidato solo al confronto `prevHour > currentHour` che non è affidabile.
- **Probabilità:** Media
- **Correzione:** Utilizzare `lastHourlyDay` o una variabile `time_t lastHourlyDayEpoch` per tracciare il giorno e gestire correttamente i salti di giorni.

---

*Fine report*
