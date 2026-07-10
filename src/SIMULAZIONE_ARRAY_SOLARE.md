# Simulazione Logica degli Array Solare

## Variabili Globali Analizzate

```cpp
float solAhHourlyToday[24];       // Ah solari per ogni ora del giorno corrente
float solAhHourlyYesterday[24]; // Ah solari del giorno precedente
float solAhTodayTotal_global;    // Totale Ah solari oggi
float solAhYesterdayTotal_global; // Totale Ah solari ieri
```

## Logica di Rollover (SensorLoop, linee 1758-1787)

```cpp
static int prevHour = -1;
bool dayRolled = false;
if (prevHour == -1) prevHour = currentHour;
if (prevHour > currentHour) dayRolled = true;
prevHour = currentHour;

if (dayRolled) {
    for (int i = 0; i < 24; i++) {
        ahHourlyYesterday[i]      = ahHourlyToday[i];
        ahChargeHourlyYesterday[i] = ahChargeHourlyToday[i];
        solAhHourlyYesterday[i]    = solAhHourlyToday[i];
        ahHourlyToday[i] = 0.0f;
        ahChargeHourlyToday[i] = 0.0f;
        solAhHourlyToday[i] = 0.0f;
    }
    solarAhToday = 0;
    ahTodayTotal_global = 0;
    ahChargeTodayTotal_global = 0;
    solAhYesterdayTotal_global = solAhTodayTotal_global;
    solAhTodayTotal_global = 0;
}
```

## Logica di Salvataggio (saveAhToSD, linee 2370-2410)

Salva in `/ah_day.bin`:
- Header con data (year, month, day)
- `ahHourlyToday[24]`
- `ahChargeHourlyToday[24]`
- `solAhHourlyToday[24]`
- `solAhHourlyYesterday[24]`
- `solAhTodayTotal_global`
- `solAhYesterdayTotal_global`

## Logica di Ripristino (restoreAhDayFromSD, linee 2440-2490)

- Legge header con data
- **Se data non corrisponde**: scarta tutti i dati e ritorna senza modificare gli array
- **Se data corrisponde**: ripristina tutti gli array

---

# SCENARIO 1 - Boot ore 08:00, Produzione fino alle 14:35

## Stato iniziale (BOOT)
```
solAhHourlyToday[24]     = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
solAhHourlyYesterday[24] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
solAhTodayTotal_global    = 0.0
solAhYesterdayTotal_global = 0.0
```

## Dopo produzione (ore 08:00 - 14:35)
Assumiamo produzione costante di 5A per semplicità:

| Ora | Ah accumulati (5A * ore) |
|-----|------------------------|
| 08  | 5.0                    |
| 09  | 5.0                    |
| 10  | 5.0                    |
| 11  | 5.0                    |
| 12  | 5.0                    |
| 13  | 5.0                    |
| 14  | 5.0 * 35/60 ≈ 2.92      |

**Stato a 14:35:**
```
solAhHourlyToday[24] = {0,0,0,0,0,0,0,0,5.0,5.0,5.0,5.0,5.0,5.0,2.92,0,0,0,0,0,0,0,0,0}
solAhHourlyYesterday[24] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
solAhTodayTotal_global = 27.92
solAhYesterdayTotal_global = 0.0
```

**VERIFICA:**
- ✅ valori oggi: 27.92 Ah totali, distribuiti nelle ore 8-14
- ✅ valori ieri: 0 Ah (giorno precedente)
- ✅ totale oggi: 27.92 Ah
- ✅ totale ieri: 0.0 Ah

**RISULTATO: PASS**

---

# SCENARIO 2 - Riavvio ESP alle 14:35

## Prima del riavvio (stato salvato su SD)
```
/ah_day.bin contiene:
- Header: giorno corrente (es. 2025-07-10)
- solAhHourlyToday[24] = {0,0,0,0,0,0,0,0,5.0,5.0,5.0,5.0,5.0,5.0,2.92,0,0,0,0,0,0,0,0,0}
- solAhHourlyYesterday[24] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
- solAhTodayTotal_global = 27.92
- solAhYesterdayTotal_global = 0.0
```

## Esecuzione setup() → restoreHourlyAhFromSD() → restoreAhDayFromSD()

### restoreHourlyAhFromSD()
- Legge `/ah_hours.bin` (esiste? no, creato solo da saveHourlyAhToSD)
- Se non esiste: **nessun cambiamento** agli array

### restoreAhDayFromSD()
- Legge `/ah_day.bin`
- Header: year=2025, month=7, day=10
- Confronta con currentDay=10, currentMonth=7, currentYear=2025
- **Data corrisponde** → ripristina tutti i valori

**Stato dopo restore:**
```
solAhHourlyToday[24] = {0,0,0,0,0,0,0,0,5.0,5.0,5.0,5.0,5.0,5.0,2.92,0,0,0,0,0,0,0,0,0}
solAhHourlyYesterday[24] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
solAhTodayTotal_global = 27.92
solAhYesterdayTotal_global = 0.0
```

**VERIFICA:**
- ✅ Nessun dato perso: gli array sono identici a prima del riavvio

**RISULTATO: PASS**

---

# SCENARIO 3 - Rollover reale ore 23:59 → 00:01

## Stato PRIMA del rollover (ore 23:59)
Assumiamo produzione solare su tutto il giorno:

```
solAhHourlyToday[24] = {0.5,0.8,1.2,2.0,3.5,4.8,5.2,6.0,7.5,8.0,7.8,6.5,5.0,4.2,3.8,3.0,2.5,2.0,1.5,1.0,0.8,0.5,0.3,0.2}
solAhHourlyYesterday[24] = {0.4,0.6,1.0,1.8,3.0,4.5,5.0,5.8,7.0,7.5,7.2,6.0,4.8,4.0,3.5,2.8,2.2,1.8,1.2,0.8,0.6,0.4,0.2,0.1}
solAhTodayTotal_global = 82.1
solAhYesterdayTotal_global = 75.3
```

## Esecuzione rollover (ore 00:01)

Il codice rileverà `prevHour(23) > currentHour(0)` → `dayRolled = true`

**Operazioni eseguite:**
1. `solAhHourlyYesterday[i] = solAhHourlyToday[i]` (copia oggi → ieri)
2. `solAhHourlyToday[i] = 0.0f` (azzera oggi)
3. `solAhYesterdayTotal_global = solAhTodayTotal_global` (trasferisci totale)
4. `solAhTodayTotal_global = 0` (azzera totale oggi)

## Stato DOPO il rollover (ore 00:01)
```
solAhHourlyToday[24] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
solAhHourlyYesterday[24] = {0.5,0.8,1.2,2.0,3.5,4.8,5.2,6.0,7.5,8.0,7.8,6.5,5.0,4.2,3.8,3.0,2.5,2.0,1.5,1.0,0.8,0.5,0.3,0.2}
solAhTodayTotal_global = 0.0
solAhYesterdayTotal_global = 82.1
```

**RISULTATO: PASS**

---

# SCENARIO 4 - Reboot immediato alle 00:05

## Stato PRIMA del reboot (ore 00:05, dopo rollover)
Dopo lo scenario 3, il file `/ah_day.bin` è stato sovrascritto con:
- Header: giorno NUOVO (es. 2025-07-11)
- solAhHourlyToday[24] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
- solAhHourlyYesterday[24] = {0.5,0.8,1.2,2.0,3.5,4.8,5.2,6.0,7.5,8.0,7.8,6.5,5.0,4.2,3.8,3.0,2.5,2.0,1.5,1.0,0.8,0.5,0.3,0.2}
- solAhTodayTotal_global = 0.0
- solAhYesterdayTotal_global = 82.1

## Esecuzione setup() → restoreAhDayFromSD()

- Header: year=2025, month=7, day=11
- Confronta con currentDay=11, currentMonth=7, currentYear=2025
- **Data corrisponde** → ripristina tutti i valori

**Stato dopo restore:**
```
solAhHourlyToday[24] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
solAhHourlyYesterday[24] = {0.5,0.8,1.2,2.0,3.5,4.8,5.2,6.0,7.5,8.0,7.8,6.5,5.0,4.2,3.8,3.0,2.5,2.0,1.5,1.0,0.8,0.5,0.3,0.2}
solAhTodayTotal_global = 0.0
solAhYesterdayTotal_global = 82.1
```

**VERIFICA:**
- ✅ TODAY contiene: array azzerati (nuovo giorno)
- ✅ YESTERDAY contiene: dati del giorno precedente

**RISULTATO: PASS**

---

# SCENARIO 5 - ESP spento per 3 giorni

## Creazione file ah_day.bin datato giorno N

File creato: `/ah_day.bin` con header giorno N (es. 2025-07-10)

## Riavvio al giorno N+3 (es. 2025-07-13)

### Esecuzione restoreAhDayFromSD()

- Header: year=2025, month=7, day=10
- Confronta con currentDay=13, currentMonth=7, currentYear=2025
- **Data NON corrisponde** (10 ≠ 13)

**Operazione:**
```cpp
if (hdr.year != currentYear || hdr.month != currentMonth || hdr.day != currentDay) {
    Serial.printf("[AH] Data non corrisponde (file: %04d-%02d-%02d, attuale: %04d-%02d-%02d) - scarto i dati\n", ...);
    f.close();
    xSemaphoreGive(sdMutex);
    return;  // NESSUN ripristino!
}
```

## Stato dopo restoreAhDayFromSD()
```
solAhHourlyToday[24] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}  // valori di default
solAhHourlyYesterday[24] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}  // valori di default
solAhTodayTotal_global = 0.0  // valore di default
solAhYesterdayTotal_global = 0.0  // valore di default
```

**VERIFICA:**
- ✅ File scartato (data non corrisponde)
- ✅ Array riempiti con valori di default (tutti zero)

**RISULTATO: PASS** (comportamento corretto: non ripristina dati obsoleti)

---

# SCENARIO 6 - Cambio ora manuale tramite handleSetTime()

## Analisi del codice handleSetTime()

Il codice di `handleSetTime()` (time_manager.cpp, linee 77-153):
1. Parsa la data/ora dal parametro
2. Aggiorna `currentHour`, `currentMinute`, ecc.
3. **NON esegue alcun rollover**
4. **NON chiama saveAhToSD() o saveHourlyAhToSD()**

## Simulazione cambi mano

### Caso 1: 14:00 → 10:00
- `prevHour` vale 14 (valore precedente nel loop)
- `currentHour` diventa 10
- **Condizione `prevHour(14) > currentHour(10)` → TRUE**
- **Rollover FALSO generato!**

**Problema:** Il codice non distingue tra:
- Rollover reale (23:59 → 00:01)
- Cambio manuale orario (14:00 → 10:00)

### Caso 2: 10:00 → 23:00
- `prevHour` vale 10
- `currentHour` diventa 23
- Condizione `prevHour(10) > currentHour(23)` → FALSE
- **Nessun rollover** (corretto)

### Caso 3: 23:00 → 00:05
- `prevHour` vale 23
- `currentHour` diventa 0
- Condizione `prevHour(23) > currentHour(0)` → TRUE
- **Rollover generato** (corretto, simula mezzanotte)

## Stato dopo cambio 14:00 → 10:00 (FALSO POSITIVO)

```
solAhHourlyYesterday[24] = solAhHourlyToday[24]  // ❌ SPOSTA DATI CORRETTI
solAhHourlyToday[24] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}  // ❌ AZZERA DATI VALIDI
solAhYesterdayTotal_global = solAhTodayTotal_global  // ❌ SPOSTA TOTALE
solAhTodayTotal_global = 0  // ❌ AZZERA TOTALE
```

**RISULTATO: POSSIBILE BUG**

**Spiegazione tecnica:**
Il meccanismo `prevHour > currentHour` non è sufficiente a distinguere un rollover reale da un cambio manuale. Un cambio orario al contrario (es. 14:00 → 10:00) genera erroneamente un rollover, spostando i dati del giorno corrente in "ieri" e azzerando i valori attivi.

---

# SCENARIO 7 - RTC perso e poi corretto

## Analisi del problema

Il codice di rilevare il rollover (SensorLoop, linea 1761):
```cpp
if (prevHour > currentHour) dayRolled = true;
```

## Simulazione: RTC perso

### Stato iniziale
- `timeValid = true`
- `currentHour = 10`
- `prevHour = 10`

### RTC si disconnette (es. collegamento I2C rotto)

Il codice in setup() (linea 1266-1279):
```cpp
if (rtc.begin()) {
    // ...
} else {
    Serial.println("[RTC] RTC non trovato");
}
```

Se RTC non risponde:
- `timeValid` rimane `false` (o viene impostato a false?)
- `currentHour` **non viene aggiornato** (rimane al valore precedente)

### RTC si riconnette

Quando RTC si riconnette:
- `rtcBaseEpoch` viene impostato all'epoca corrente
- `currentHour` viene aggiornato all'ora corrente

### Problema critico

Se durante l'intervallo in cui RTC era perso sono passate più ore:

**Esempio:**
- RTC perso alle 10:00
- RTC riconnesso alle 15:00 (5 ore dopo)
- `prevHour` vale 10 (ultimo valore prima della disconnessione)
- `currentHour` diventa 15
- Condizione `prevHour(10) > currentHour(15)` → FALSE
- **Nessun rollover** (corretto)

**Ma se:**
- RTC perso alle 23:00
- RTC riconnesso alle 02:00 (giorno successivo)
- `prevHour` vale 23
- `currentHour` diventa 2
- Condizione `prevHour(23) > currentHour(2)` → TRUE
- **Rollover generato** (corretto, se è passata la mezzanotte)

**Tuttavia, c'è un caso limite:**

Se RTC è perso per più di un giorno:
- RTC perso alle 10:00 del giorno N
- RTC riconnesso alle 08:00 del giorno N+2
- `prevHour` vale 10
- `currentHour` diventa 8
- Condizione `prevHour(10) > currentHour(8)` → TRUE
- **Rollover generato una sola volta** (non due volte!)

**RISULTATO: POSSIBILE BUG**

**Spiegazione tecnica:**
Il confronto `prevHour > currentHour` non tiene conto del numero di giorni trascorsi. Se l'RTC è offline per più di 24 ore, il rollover verrà generato una sola volta, anche se sono passati più giorni. Inoltre, se `timeValid` è false, `currentHour` potrebbe non essere aggiornato correttamente, portando a condizioni imprevedibili.

---

# RIEPILOGO FINALE

| Scenario | Risultato | Note |
|----------|-----------|------|
| 1 | PASS | Boot e accumulo corretto |
| 2 | PASS | Ripristino dati funziona |
| 3 | PASS | Rollover reale funziona correttamente |
| 4 | PASS | Stato dopo reboot corretto |
| 5 | PASS | File obsoleto scartato correttamente |
| 6 | POSSIBILE BUG | Cambio orario manuale al contrario genera falso rollover |
| 7 | POSSIBILE BUG | RTC offline >24h genera rollover incompleto |

## Note importanti

1. **handleSetTime()** non invalida gli array esistenti né salva lo stato
2. **Il confronto `prevHour > currentHour`** è fragile e non tiene conto del contesto
3. **Non vi è tracciamento del giorno** per distinguere un vero rollover da un cambio manuale
4. **Variabile `lastHourlyDay` dichiarata a riga 427 ma MAI UTILIZZATA** - esiste un meccanismo di tracciamento del giorno che non viene mai invocato

---

## Dettaglio tecnico: lastHourlyDay mai utilizzato

Nel file `main.cpp` a riga 427 è dichiarata:
```cpp
int lastHourlyDay = -1;
```

Tuttavia questa variabile **non è mai letta o scritta** nel codice. L'unico meccanismo per rilevare il cambio giorno è:
```cpp
static int prevHour = -1;
if (prevHour > currentHour) dayRolled = true;
```

Questo approccio:
- ✅ Funziona per il rollover reale (23→0)
- ❌ Genera falsi positivi per cambi manuali (14→10)
- ❌ Non tiene conto del numero di giorni trascorsi quando RTC è offline
```
