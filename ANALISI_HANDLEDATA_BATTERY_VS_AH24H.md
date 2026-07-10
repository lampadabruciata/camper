# Analisi Confronto: Soluzione A vs Soluzione B

## Contesto
Il frontend (index.html) nella funzione `updateBatteryPage()` (line 1125-1128) si aspetta i seguenti dati orari Ah:
- `ahHourlyToday[24]` - Ah scaricati oggi per ogni ora
- `ahHourlyYesterday[24]` - Ah scaricati ieri per ogni ora  
- `ahChargeHourlyToday[24]` - Ah caricati oggi per ogni ora
- `ahChargeHourlyYesterday[24]` - Ah caricati ieri per ogni ora

Attualmente `handleDataBattery()` NON fornisce questi array, causando errori nel frontend.

---

## SOLUZIONE A: Aggiungere i dati Ah a handleDataBattery()

### Dimensione JSON stimata
```
JSON corrente (handleDataBattery):
- 5 array × 300 elementi = ~30.100 bytes (solo array)
- 6 valori scalari (vBattery, ampNow, solarNow, socPct, ahNow, ahMax, eff) = ~100 bytes
- Totale attuale: ~30.200 bytes (MA... il buffer è solo 16.000!)

Con aggiunta 4 array Ah orari (24 elementi ciascuno):
- 4 array × 24 elementi = 4 × (24 × 20 + 20) = 4 × 500 = 2.000 bytes
- Nuovo totale: ~32.200 bytes
```

**Stima JSON output:** ~2.5-3 KB in più (solo i 4 array orari)

### Impatto RAM
- **Buffer JSON**: 16.000 bytes → **INSUFFICIENTE** (mancano ~16.200 bytes)
- **Stringa di output**: `String out` aggiunge ~3-4 KB heap temporaneo
- **Stack task**: +200-300 bytes per variabili aggiuntive

### Impatto heap
- **Heap consumato**: 16.000 (buffer) + 4.000 (String) = **20.000 bytes**
- **Heap disponibile ESP32**: Tipicamente 200-250 KB dopo avvio
- **Margine heap**: Negativo - rischio di crash o JSON troncato

### Numero richieste web
- **1 richiesta ogni secondo** (già esistente per `/data/battery`)
- Nessun cambiamento

### Modifiche frontend necessarie
- **Nessuna** - i dati saranno disponibili nello stesso endpoint

### Modifiche backend necessarie
```cpp
// In handleDataBattery(), dopo le righe 310-313 aggiungere:
JsonArray ahHourlyTodayArr = json.createNestedArray("ahHourlyToday");
for (int i = 0; i < 24; i++) ahHourlyTodayArr.add(ahHourlyToday[i]);

JsonArray ahHourlyYesterdayArr = json.createNestedArray("ahHourlyYesterday");
for (int i = 0; i < 24; i++) ahHourlyYesterdayArr.add(ahHourlyYesterday[i]);

JsonArray ahChargeHourlyTodayArr = json.createNestedArray("ahChargeHourlyToday");
for (int i = 0; i < 24; i++) ahChargeHourlyTodayArr.add(ahChargeHourlyToday[i]);

JsonArray ahChargeHourlyYesterdayArr = json.createNestedArray("ahChargeHourlyYesterday");
for (int i = 0; i < 24; i++) ahChargeHourlyYesterdayArr.add(ahChargeHourlyYesterday[i]);
```

**PROBLEMA CRITICO**: Il buffer JSON di 16.000 bytes è già insufficiente!

---

## SOLUZIONE B: Completare handleDataAh24h() e usare endpoint dedicato

### Dimensione JSON stimata
```
handleDataAh24h() attuale:
- 2 array × 24 elementi (discharge, charge) = 2 × 500 = 1.000 bytes
- 1 array labels × 24 elementi = 500 bytes
- Totale: ~1.500 bytes

Con aggiunta 2 array mancanti:
- ahChargeHourlyToday[24]
- ahChargeHourlyYesterday[24]
- Totale: ~2.500 bytes

Stima JSON output: ~1.5-2 KB
```

### Impatto RAM
- **Buffer JSON**: 4.096 bytes → **SUFFICIENTE** (ne mancano ~1.500)
- **Stringa di output**: ~2 KB heap temporaneo
- **Stack task**: +100-150 bytes

### Impatto heap
- **Heap consumato**: 4.096 (buffer) + 2.000 (String) = **6.000 bytes**
- **Margine heap**: **POSITIVO** - 20x più sicuro del buffer attuale

### Numero richieste web
- **2 richieste ogni secondo**:
  - `/data/battery` (già esistente)
  - `/data/ah24h` (nuovo, ogni 2-3 secondi)
- Oppure: `/data/ah24h` solo al cambio pagina o ogni 5-10 secondi

### Modifiche frontend necessarie
```javascript
// Modificare updateBatteryPage() per aggiungere chiamata separata:
async function fetchAh24h() {
    const resp = await fetch("/data/ah24h");
    const d = await resp.json();
    ahToday           = normalizeHourlyArray(d.discharge);
    ahYesterday       = normalizeHourlyArray(d.charge);
    ahChargeToday     = normalizeHourlyArray(d.charge);
    ahChargeYesterday = normalizeHourlyArray(d.chargeYesterday);
    // Aggiorna grafici...
}
```

### Modifiche backend necessarie
```cpp
// In handleDataAh24h(), aggiungere:
JsonArray chgToday = json.createNestedArray("chargeToday");
for (int i = 0; i < 24; i++) chgToday.add(ahChargeHourlyToday[i]);

JsonArray chgYesterday = json.createNestedArray("chargeYesterday");
for (int i = 0; i < 24; i++) chgYesterday.add(ahChargeHourlyYesterday[i]);

// Rinominare "discharge" → "dischargeToday" e aggiungere "dischargeYesterday"
// oppure mantenere compatibilità con frontend esistente
```

---

## CONFRONTO SINTETICO

| Parametro | Soluzione A | Soluzione B |
|-----------|-------------|-------------|
| **Dimensione JSON** | ~32 KB (buffer insufficiente) | ~2.5 KB (buffer sufficiente) |
| **Impatto RAM** | CRITICO - buffer overflow | SICURO - margine ampio |
| **Impatto heap** | ~20 KB (rischio) | ~6 KB (sicuro) |
| **Richieste web** | 1/sec (stesso endpoint) | 2 richieste (o 1 ogni 5s) |
| **Modifiche frontend** | Nessuna | Moderate (nuova funzione fetch) |
| **Modifiche backend** | Aggiungi 4 array a funzione instabile | Completamento funzione dedicata |
| **Stabilità ESP32** | ⚠️ Rischio crash | ✅ Sicuro |

---

## RACCOMANDAZIONE: SOLUZIONE B

### Motivi tecnici:
1. **Stabilità**: Il buffer di 16.000 bytes in `handleDataBattery()` è già al limite (o sopra). Aggiungere altri 2.000 bytes porterà a:
   - `serializeJson()` che fallisce silenziosamente
   - JSON troncato o malformato
   - Crash del sistema (out of memory)

2. **Separazione responsabilità**: 
   - `/data/battery` → dati istantanei + grafico 24h (tensione/corrente)
   - `/data/ah24h` → dati orari Ah (consumo/carica)
   - Architettura più pulita e manutenibile

3. **Memory footprint**: 
   - Soluzione A: 20 KB heap in un colpo
   - Soluzione B: 6 KB heap, distribuito nel tempo

4. **Scalabilità**: Se in futuro serviranno altri dati orari (es. temperature orarie), l'endpoint dedicato può crescere senza impattare sull'endpoint battery

### Implementazione consigliata:
1. Aumentare il buffer `handleDataAh24h()` a 6.000 bytes (margine sicuro)
2. Aggiungere gli array mancanti: `chargeToday`, `chargeYesterday`
3. Modificare il frontend per fetchare `/data/ah24h` indipendentemente
4. Opzionale: chiamare `/data/ah24h` ogni 5-10 secondi invece di ogni secondo

### Note importanti:
- Il frontend attuale chiama `/data/ah24h` ma non usa i dati (vedi linea 1125-1128 che legge `d.ahHourlyToday` che non esiste)
- Il backend deve rinominare gli array per renderli più espliciti:
  - `discharge` → `dischargeToday`
  - Aggiungere `dischargeYesterday`
  - `charge` → `chargeToday`
  - Aggiungere `chargeYesterday`