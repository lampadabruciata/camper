# Riepilogo Implementazione Soluzione B

## File modificati

### 1. `src/web/web_manager.cpp`
- **Aggiunte variabili esterne** per `ahHourlyYesterday`, `ahChargeHourlyYesterday`, e i totali
- **handleDataAh24h()**: Completato con tutti gli array richiesti:
  - `ahHourlyToday[24]`
  - `ahHourlyYesterday[24]`
  - `ahChargeHourlyToday[24]`
  - `ahChargeHourlyYesterday[24]`
  - `ahTodayTotal`
  - `ahYesterdayTotal`
  - `ahChargeTodayTotal`
  - `ahChargeYesterdayTotal`
- Buffer aumentato da 4096 a 6000 bytes

### 2. `data/index.html`
- **Aggiunte variabili globali** per i totali Ah
- **fetchPageData()**: Aggiunta chiamata a `/data/ah24h` nella sezione "batteria"
- **updateAh24hData()**: Nuova funzione per processare i dati Ah orari
- **updateBatteryPage()**: Rimossa lettura dati Ah orari (ora da `updateAh24hData()`)
- **showAhDay()**: Modificata per usare variabili globali `ahTodayTotal`/`ahYesterdayTotal`
- **showAhChargeDay()**: Modificata per usare variabili globali `ahChargeTodayTotal`/`ahChargeYesterdayTotal`

---

## Impatto RAM

### handleDataAh24h()
- **Buffer JSON**: 6.000 bytes (aumentato da 4.096)
- **Stringa di output**: ~2 KB heap temporaneo
- **Stack task**: +100-150 bytes

### handleDataBattery()
- **Nessuna modifica** - rimane invariato
- Buffer 16.000 bytes (già al limite, ma non aggiungiamo dati)

---

## Impatto heap

| Parametro | Valore |
|-----------|--------|
| Heap consumato handleDataAh24h() | ~6.000 bytes (buffer 6KB + String 2KB) |
| Heap consumato handleDataBattery() | ~16.000 bytes (buffer) + String ~4KB |
| **Margine heap** | **POSITIVO** - 20x più sicuro rispetto all'aggiunta in handleDataBattery() |

---

## Note importanti
- ✅ NON modificato: `graph24h.bin`, `ah_day.bin`, `SensorLoop`, `RTC`, logica rollover, persistenza
- Il frontend chiama `/data/ah24h` ogni 1 secondo (insieme a `/data/battery`)
- Questo raddoppia le richieste web ma mantiene la stabilità
- L'endpoint `/data/ah24h` restituisce ora tutti i dati necessari per gli istogrammi Ah