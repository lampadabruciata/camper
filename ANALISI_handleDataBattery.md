# Analisi handleDataBattery()

## 1. Dichiarazione esatta del DynamicJsonDocument

```cpp
DynamicJsonDocument json(16000);
```

## 2. Elementi inseriti negli array

| Array | Tipo | Max elementi | Note |
|-------|------|--------------|------|
| `volt` | float | 300 | stesso ciclo del for |
| `amps` | float | 300 | stesso ciclo del for |
| `solar` | float | 300 | stesso ciclo del for |
| `labels` | string (char[6]) | 300 | stesso ciclo del for |
| `epoch` | uint32_t | 300 | stesso ciclo del for |

**Dettaglio ciclo for:**
```cpp
const int N = 360;
int step = 1;
if (N > 300) {
    step = (N + 300 - 1) / 300;  // = 2
}
for (int k = 0; k < N && validCount < 300; k += step)
```

- Iterazioni: 360/2 = 180 (ma validCount < 300 permette di aggiungere anche quando ep==0)
- Max elementi per array: 300

## 3. Calcolo capacità con ArduinoJson

### Formule ArduinoJson 6 (ESP32, 32-bit):
- `JSON_ARRAY_SIZE(n) = n × sizeof(Slot) + sizeof(Slot)`
- `JSON_OBJECT_SIZE(n) = n × sizeof(Slot) + sizeof(Slot)`
- `sizeof(Slot) = 20 bytes`

### Stringhe chiave (5):
- "volt" (5) + null = 6 bytes
- "amps" (5) + null = 6 bytes
- "solar" (6) + null = 7 bytes
- "labels" (7) + null = 8 bytes
- "epoch" (6) + null = 7 bytes
- **Totale stringhe: 34 bytes**

### Stringhe valore (labels):
- "--:--" (5) + null = 6 bytes (usato quando ep==0)
- timeLabels[idx] = char[6] = 6 bytes
- **Totale stringhe valore: 6 bytes** (una sola copia per stringa unica)

### Calcolo capacità:

```
JSON_OBJECT_SIZE(7) = 7 × 20 + 20 = 160 bytes
JSON_ARRAY_SIZE(300) × 5 = 5 × (300 × 20 + 20) = 5 × 6020 = 30.100 bytes
Stringhe: 34 + 6 = 40 bytes
```

**Capacità necessaria (max): 30.300 bytes**

## 4. Confronto capacità

| | Valore |
|---|--------|
| **Capacità attuale** | 16.000 bytes |
| **Capacità necessaria (max)** | ~30.300 bytes |
| **Margine residuo** | **-14.300 bytes** (NEGATIVO!) |

## 5. Verifica limite

**Il documento è oggi vicino al limite?**

**SÌ, È ANCHE SOPRA IL LIMITE!**

Con 16.000 byte allocati, il documento è **insufficiente** per contenere 300 elementi in 5 array. Questo può causare:
- `serializeJson()` che fallisce silenziosamente
- JSON troncato o malformato
- Crash del sistema

## Risultato JSON tipico

```json
{
  "volt": [12.5, 12.4, ...],     // max 300 elementi
  "amps": [0.5, 0.3, ...],       // max 300 elementi
  "solar": [0.0, 0.0, ...],     // max 300 elementi
  "labels": ["12:00", "12:01", ...], // max 300 stringhe
  "epoch": [12345678, ...],      // max 300 uint32_t
  "vBattery": 12.5,
  "ampNow": 0.5,
  "solarNow": 0.0,
  "socPct": 85.0,
  "ahNow": 42.5,
  "ahMax": 50.0,
  "eff": 0.85
}