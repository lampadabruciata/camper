# AUDIT COMPLETO - ISTOGRAMMI SOLARE ORARIA (Ah) - VERSIONE AGGIORNATA

## SEZIONE A - Flusso dati reale attuale

### 1. Strutture dati per grafico solare orario

**Array RAM (main.cpp):**
```
float solarCurrentSamples[1440];     // RIGA 298 - Corrente solare per ogni minuto (24h)
float solAhHourlyToday[24];          // RIGA 447 - Ah solari prodotti oggi per ogni ora
float solAhHourlyYesterday[24];      // RIGA 448 - Ah solari prodotti ieri per ogni ora
float solAhTodayTotal_global;        // RIGA 451 - Totale Ah solari oggi
float solAhYesterdayTotal_global;    // RIGA 452 - Totale Ah solari ieri
```

**Buffer (main.cpp righe 283-290):**
```
float voltageSamples[1440];          // Tensione per ogni minuto
char timeLabels[1440][6];           // Etichette orario "HH:MM"
uint32_t epochMinutes[1440];        // Epoch in minuti UTC
int sampleIndex;                    // Indice corrente nel buffer circolare
```

**Struct (main.cpp righe 337-342, graph_storage.h righe 47-49):**
```
struct AhStateFile {
    uint32_t magic;
    uint32_t epoch;
    float ahNow;
    float battVolt;
};
struct GraphHeader { uint32_t magic; uint16_t ver; uint16_t rsv; };
```

**File SD:**
- `/graph24h.bin` - Grafico 24h completo (v3) - contiene anche i totali solari
- `/ah_day.bin` - Dati orari Ah (24 + 24 + 24 valori)
- `/ah_hours.bin` - Solo ahHourlyToday (24 valori)
- `/ah_state.bin` - Stato Ah corrente

### 2. Funzioni coinvolte

**Salvataggio dati solari orari:**
- `saveAhToSD()` (main.cpp righe 2360-2389) - Salva ogni minuto
- `saveHourlyAhToSD()` (main.cpp righe 2335-2358) - Salva al cambio ora
- `saveGraphToSD()` (graph_storage.cpp righe 11-82) - Salva ogni 5 minuti

**Ripristino dati da SD:**
- `restoreGraphFromSD()` (graph_storage.cpp righe 84-262)
- `restoreAhDayFromSD()` (main.cpp righe 2419-2445)
- `restoreHourlyAhFromSD()` (main.cpp righe 2392-2417)

**Cambio ora/giorno:**
- `SensorLoop()` (main.cpp righe 1702-2079) - Logica cambio giorno righe 1748-1777

**Endpoint web JSON:**
- `/data/solar` (web_manager.cpp righe 185-238) - Dati solari + istogramma orario

**JavaScript grafico:**
- `updateSolarAhBars()` (index.html righe 1524-1553)
- `showSolarAh()` (index.html righe 1555-1577)

### 3. Determinazione ora corrente

**Come viene determinata (main.cpp righe 1710-1722):**
```cpp
if (timeValid) {
    updateDST();
    time_t tLocal = utcToLocal(nowEpoch());
    tmElements_t tm;
    breakTime(tLocal, tm);
    currentHour = tm.Hour;
    currentMinute = tm.Minute;
}
```

**Risposta a domande specifiche:**
- **L'ora corrente viene calcolata live** dal RTC (nowEpoch() + offset timezone)
- **Le ore passate vengono riscritte** - il problema principale!
- **Il grafico usa l'RTC ESP32** (non l'orologio del browser)

### 4. Flusso OGGI/IERI

**Quali file vengono letti:**
- `restoreAhDayFromSD()` legge `/ah_day.bin` (main.cpp righe 2419-2445)
- `restoreGraphFromSD()` legge `/graph24h.bin` (graph_storage.cpp)

**Come viene identificata la data:**
- **PROBLEMA CRITICO:** Non viene mai salvata la data nei file!
- Il codice usa solo `currentHour` per rilevare il cambio giorno (riga 1752)
- `lastHourlyDay` è dichiarato (riga 418) ma **MAI USATO**

**Rischio di mescolare i dati:**
- **SÌ, GRANDE RISCHIO** - vedi problema 5.2

**Cosa succede al cambio giorno (23:59 -> 00:00):**
```cpp
if (prevHour > currentHour) dayRolled = true;  // main.cpp riga 1752
if (dayRolled) {
    for (int i = 0; i < 24; i++) {
        solAhHourlyYesterday[i] = solAhHourlyToday[i];  // main.cpp riga 1767
        solAhHourlyToday[i] = 0.0f;                    // main.cpp riga 1771
    }
}
```

**Cosa succede dopo un reboot:**
- `restoreAhDayFromSD()` sovrascrive `solAhHourlyToday` con i valori da SD
- **PROBLEMA:** I valori letti potrebbero essere di un giorno diverso!

---

## SEZIONE B - Problemi trovati

### PROBLEMA 1: **Totale solare non inviato nell'endpoint**
**File:** src/web/web_manager.cpp, righe 225-228
**Descrizione:** L'endpoint `/data/solar` invia `solAhHourlyToday` e `solAhHourlyYesterday` ma **NON invia mai** `solAhTodayTotal_global` né `solAhYesterdayTotal_global`.
**Impatto:** Il frontend non può mostrare il totale Ah giornaliero.

### PROBLEMA 2: **Nessun controllo data nei file SD**
**File:** src/main.cpp, riga 418
**Descrizione:** `lastHourlyDay` è dichiarato ma **mai utilizzato**. I file `/ah_day.bin` e `/ah_hours.bin` non contengono informazioni sulla data.
**Impatto:** Dopo un reboot, i dati di ieri potrebbero essere sovrascritti con quelli di oggi (o viceversa) se il dispositivo è rimasto spento per più di un giorno.

### PROBLEMA 3: **Rilevamento cambio giorno fragile**
**File:** src/main.cpp, righe 1748-1753
**Descrizione:** Il cambio giorno è rilevato solo quando `prevHour > currentHour`. Questo fallisce se:
- Il dispositivo si riavvia a mezzanotte (prevHour = -1)
- Il dispositivo è stato spento per ore e si riaccende a un'ora precedente
**Impatto:** Il rollover OGGI/IERI potrebbe non avvenire mai o avvenire all'ora sbagliata.

### PROBLEMA 4: **Doppio salvataggio incoerente**
**File:** src/main.cpp, righe 1756-1761, 1760-1761
**Descrizione:** `saveHourlyAhToSD()` salva SOLO `ahHourlyToday` (24 float), mentre `saveAhToSD()` salva tutti e 3 gli array orari.
**Impatto:** I file `/ah_hours.bin` e `/ah_day.bin` hanno contenuti diversi e vengono usati funzioni di ripristino diverse.

### PROBLEMA 5: **Ripristino dati orari sovrascrive senza controlli**
**File:** src/main.cpp, righe 2419-2445
**Descrizione:** `restoreAhDayFromSD()` legge i dati da `/ah_day.bin` e li copia direttamente in `ahHourlyToday` senza verificare se sono del giorno corrente.
**Impatto:** Dopo reboot, i dati orari potrebbero essere di un giorno diverso da quello corrente.

### PROBLEMA 6: **Ore passate non congelate - sovrascritte ogni minuto**
**File:** src/main.cpp, righe 1924-1926
**Descrizione:** Ogni minuto, il codice aggiunge Ah all'ora corrente:
```cpp
ahHourlyToday[currentHour] += minuteDischarge;
```
Ma **non c'è alcun meccanismo per "congelare" le ore passate**. Se l'ora corrente è 10:30, le ore 0-9 vengono comunque sovrascritte se il codice continua ad eseguire.
**Impatto:** Le ore già complete vengono continuamente modificate.

### PROBLEMA 7: **Totale non allineato con somma oraria**
**File:** src/main.cpp, riga 1866
**Descrizione:** `solAhTodayTotal_global` viene incrementato separatamente da `solAhHourlyToday`, ma **non viene mai usato per il totale giornaliero**. Inoltre, il totale non viene mai inviato nell'endpoint.
**Impatto:** Il valore mostrato nel display (solarAhToday) potrebbe non corrispondere alla somma dei valori dell'istogramma.

### PROBLEMA 8: **Frontend cerca campi inesistenti**
**File:** data/index.html, righe 1154-1160, 1567-1572
**Descrizione:** Il JavaScript cerca `solarAhTodayTotal` e `solarAhYesterdayTotal` che **non vengono mai inviati** dall'endpoint.
**Impatto:** Il totale Ah non viene mai mostrato correttamente.

### PROBLEMA 9: **Salvataggio SD ogni minuto con rimozione file**
**File:** src/main.cpp, riga 2368
**Descrizione:** `saveAhToSD()` esegue `SD.remove("/ah_day.bin")` ogni minuto, creando usura eccessiva sulla SD.
**Impatto:** Degradazione prematura della scheda SD.

### PROBLEMA 10: **Inizializzazione array non eseguita se graphRestored=true**
**File:** src/main.cpp, righe 1633-1640
**Descrizione:** Se `graphRestored` è true, gli array non vengono inizializzati a zero. Ma `solAhHourlyToday` e `solAhHourlyYesterday` **non sono inclusi** in questa inizializzazione.
**Impatto:** Array orari potrebbero contenere valori spazzatura all'avvio.

### PROBLEMA 11: **Duplicato campo JSON - confuzione di denominazione**
**File:** src/web/web_manager.cpp, righe 222-223
**Descrizione:** L'endpoint invia sia `solAhToday` che `solarAhToday` con lo stesso valore:
```cpp
json["solAhToday"] = solarAhToday;
json["solarAhToday"] = solarAhToday;
```
Ma il totale (`solAhTodayTotal_global`) non viene mai inviato.
**Impatto:** Confusione nel frontend, totale mancante.

### PROBLEMA 12: **solAhYesterdayTotal_global non trasferito al rollover**
**File:** src/main.cpp, righe 1763-1777
**Descrizione:** Al rollover giornaliero, il totale di ieri NON viene salvato:
```cpp
if (dayRolled) {
    for (int i = 0; i < 24; i++) {
        ahHourlyYesterday[i]      = ahHourlyToday[i];
        ahChargeHourlyYesterday[i] = ahChargeHourlyToday[i];
        solAhHourlyYesterday[i]    = solAhHourlyToday[i];
    }
    // MANCA: solAhYesterdayTotal_global = solAhTodayTotal_global;
    solAhTodayTotal_global = 0;
}
```
**Impatto:** Il totale di ieri viene perso al rollover.

---

## SEZIONE C - Livello di gravità

| Problema | Gravità | Impatto |
|----------|---------|---------|
| 1. Totale solare non inviato | **CRITICO** | Il totale Ah non è mai mostrato |
| 2. Nessun controllo data nei file | **CRITICO** | Dati di giorni diversi mescolati |
| 3. Rilevamento cambio giorno fragile | **ALTO** | Rollover OGGI/IERI non affidabile |
| 4. Doppio salvataggio incoerente | **MEDIO** | File parziali/incompleti |
| 5. Ripristino senza controlli | **ALTO** | Dati sbagliati dopo reboot |
| 6. Ore passate non congelate | **CRITICO** | Istogramma orario corrotto |
| 7. Totale non allineato | **MEDIO** | Incoerenza dati |
| 8. Frontend cerca campi inesistenti | **CRITICO** | UI non funzionante |
| 9. Salvataggio SD ogni minuto | **BASSO** | Usura SD eccessiva |
| 10. Inizializzazione array incompleta | **MEDIO** | Valori spazzatura all'avvio |
| 11. Duplicato campo JSON | **BASSO** | Confusione naming |
| 12. Totale ieri non trasferito | **ALTO** | Totale ieri perso al rollover |

---

## SEZIONE D - Soluzione architetturale consigliata

### Architettura corretta per istogramma solare orario

```
1. RTC ESP32 fornisce epoch UTC
2. Ogni minuto:
   a. Calcola l'ora corrente (0-23)
   b. Se l'ora è diversa da quella del minuto precedente:
      - Aggiorna l'array orario dell'ora appena finita
      - Salva su SD con controllo data
3. Al cambio giorno (00:00):
   a. Copia solAhHourlyToday → solAhHourlyYesterday
   b. Copia solAhTodayTotal_global → solAhYesterdayTotal_global
   c. Azzera solAhHourlyToday
   d. Azzera solAhTodayTotal_global
4. Salvataggio su SD:
   - File: /solar_YYYY-MM-DD.bin (un file per ogni giorno)
   - Ogni riga: ora + Ah + timestamp
5. Ripristino:
   - Leggi la data corrente
   - Carica /solar_YYYY-MM-DD.bin per OGGI
   - Carica /solar_ieri.bin per IERI
```

### Meccanismo di "congelamento ore"

```cpp
// Array di stato
bool hourFrozen[24] = {false};  // true quando l'ora è passata

void onNewMinute() {
    int newHour = getCurrentHour();
    
    if (newHour != currentHour) {
        // L'ora appena finita è ora "congelata"
        hourFrozen[currentHour] = true;
        currentHour = newHour;
        
        // Reset accumulatore per la nuova ora
        solAhHourlyAccum = 0;
    }
    
    // Aggiorna solo se l'ora non è congelata
    if (!hourFrozen[currentHour]) {
        solAhHourlyToday[currentHour] += delta;
    }
}
```

### Endpoint web corretto

```json
{
  "solarA": [...],
  "labels": ["10:00", "10:01", ...],
  "solAhHourlyToday": [0.5, 0.3, ...],
  "solAhHourlyYesterday": [0.2, 0.1, ...],
  "solAhTodayTotal": 12.5,
  "solAhYesterdayTotal": 8.3
}
```

---

## SEZIONE E - Elenco preciso file e righe da modificare

### File: src/main.cpp

| Righe | Azione | Descrizione |
|-------|--------|-------------|
| 418 | **AGGIUNGI** | `int lastHourlyDay = -1;` diventa `time_t lastHourlyDayEpoch = 0;` |
| 1748-1777 | **MODIFICA** | Logica cambio giorno: usa `lastHourlyDayEpoch` invece di `prevHour` |
| 1752 | **SOSTITUISCI** | `if (prevHour > currentHour)` con controllo data reale |
| 1767 | **AGGIUNGI** | `solAhYesterdayTotal_global = solAhTodayTotal_global;` |
| 1771 | **AGGIUNGI** | `solAhTodayTotal_global = 0;` |
| 1866 | **RIMUOVI** | `solAhTodayTotal_global += solarAmps / 3600.0f;` (duplicato) |
| 1924-1926 | **AGGIUNGI** | Controllo `if (!hourFrozen[currentHour])` prima dell'aggiornamento |
| 1633-1640 | **AGGIUNGI** | Inizializzazione `solAhHourlyToday` e `solAhHourlyYesterday` |

### File: src/web/web_manager.cpp

| Righe | Azione | Descrizione |
|-------|--------|-------------|
| 222-223 | **RIMUOVI** | Rimuovi `json["solarAhToday"] = solarAhToday;` (duplicato) |
| 225-228 | **AGGIUNGI** | Invio `solAhTodayTotal_global` e `solAhYesterdayTotal_global` nell'JSON |

### File: data/index.html

| Righe | Azione | Descrizione |
|-------|--------|-------------|
| 1154-1160 | **MODIFICA** | Usa `solAhTodayTotal` invece di `solarAhTodayTotal` |
| 1567-1572 | **MODIFICA** | Usa `solAhTodayTotal` e `solAhYesterdayTotal` |

### File: src/storage/graph_storage.h

| Righe | Azione | Descrizione |
|-------|--------|-------------|
| 23-24 | **AGGIUNGI** | `extern float solAhTodayTotal_global;` (se non presente) |
| 31-32 | **AGGIUNGI** | `extern float solAhYesterdayTotal_global;` (se non presente) |

### File: src/storage/graph_storage.cpp

| Righe | Azione | Descrizione |
|-------|--------|-------------|
| 52-53 | **AGGIUNGI** | Salvataggio `solAhTodayTotal_global` e `solAhYesterdayTotal_global` |
| 222-227 | **AGGIUNGI** | Ripristino `solAhTodayTotal_global` e `solAhYesterdayTotal_global` |

---

## RIEPILOGO PROBLEMI CRITICI

1. **Istogramma orario corrotto**: Le ore passate vengono continuamente sovrascritte
2. **Dati di giorni diversi mescolati**: Nessun controllo data nei file SD
3. **Totale Ah mai mostrato**: Campi mancanti nell'endpoint web
4. **Rollover OGGI/IERI non affidabile**: Dipende solo dal confronto ore
5. **Totale ieri perso al rollover**: Non viene salvato durante il cambio giorno