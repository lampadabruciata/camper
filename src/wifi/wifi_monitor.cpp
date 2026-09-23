#include "wifi_monitor.h"

#include <Arduino.h>
#include <WiFi.h>
// WiFi.h NON include esp_wifi.h (solo esp_wifi_types.h): serve per
// esp_wifi_get_event_mask()/esp_wifi_set_event_mask() e per il bit
// WIFI_EVENT_MASK_AP_PROBEREQRECVED usati in initWifiMonitor().
#include "esp_wifi.h"
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ============================================================
// STATO INTERNO (scritto solo dal callback eventi, letto dai task)
// ============================================================
// Modello dati grezzo: qui non esistono campi stringa né formattazioni (il
// callback non formatta nulla). MAC e IP restano nei formati nativi degli
// eventi: uint8_t[6] e IPv4 in network byte order (come lwIP).
typedef struct {
  uint32_t staConnectedCount;
  uint32_t staDisconnectedCount;
  uint32_t dhcpAssignedCount;
  uint32_t probeRequestCount;

  uint8_t  lastEvent;      // vedi enum WifiMonLastEvent
  uint32_t lastStaAid;
  uint8_t  lastStaMac[6];  // tutti 0 = nessuna stazione mai vista
  uint32_t lastStaIp;      // network byte order, 0 = mai assegnato

  uint32_t lastAssocMs;
  uint32_t lastDisconnMs;
  uint32_t lastDhcpMs;
  uint32_t lastProbeMs;
} wifiMonitorState_t;

// Etichette dell'ultimo evento (valori interni, indipendenti dalla
// numerazione di arduino_event_id_t).
enum WifiMonLastEvent {
  WIFI_MON_EVENT_NONE = 0,
  WIFI_MON_EVENT_STACONNECTED,
  WIFI_MON_EVENT_STADISCONNECTED,
  WIFI_MON_EVENT_STAIPASSIGNED
};

// Stato condiviso: il callback scrive in contesto task (event task di
// Arduino), /api/boot legge dal task del WebServer. Gli accessi compositi
// (ultimo evento = AID + MAC + millis) sono protetti dalla stessa sezione
// critica usata in main.cpp per lo stato condiviso.
static wifiMonitorState_t s_wifiMonState;
static portMUX_TYPE s_wifiMonMux = portMUX_INITIALIZER_UNLOCKED;

// Configurazione AP campionata in initWifiMonitor(): scritta una sola volta
// in setup(), prima della registrazione dei callback (nessuna concorrenza
// possibile con l'handler).
static uint8_t s_wifiMonMode    = 0; // WIFI_MODE_NULL
static uint8_t s_wifiMonChannel = 0;
static int8_t  s_wifiMonTxPower = 0;

// initWifiMonitor() è idempotente: la seconda chiamata non registra di nuovo
// i callback (evita contatori raddoppiati).
static bool s_wifiMonInitialized = false;

// ============================================================
// HELPER DI FORMATTAZIONE (usati solo da wifiMonitorGet())
// ============================================================
// NB: nessuna formattazione avviene nel callback eventi.

static const char* wifiMonLastEventLabel(uint8_t event) {
  switch (event) {
    case WIFI_MON_EVENT_STACONNECTED:    return "AP_STACONNECTED";
    case WIFI_MON_EVENT_STADISCONNECTED: return "AP_STADISCONNECTED";
    case WIFI_MON_EVENT_STAIPASSIGNED:   return "AP_STAIPASSIGNED";
    default:                             return "NONE";
  }
}

// MAC "aa:bb:cc:dd:ee:ff"; "--" se nessuna stazione è mai stata vista
// (memoria a zero = nessun evento di associazione/disassociazione ricevuto).
static void wifiMonFormatStaMac(const uint8_t mac[6], char *dst, size_t dstLen) {
  bool seen = false;
  for (size_t i = 0; i < 6; i++) {
    if (mac[i] != 0) { seen = true; break; }
  }
  if (!seen) {
    snprintf(dst, dstLen, "--");
    return;
  }
  snprintf(dst, dstLen, "%02x:%02x:%02x:%02x:%02x:%02x",
           (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
           (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
}

// IPv4 in network byte order -> "a.b.c.d" con lo stesso ordine di ottetti di
// IPAddress::toString() (byte 0 = primo ottetto). "--" se mai assegnato.
static void wifiMonFormatStaIp(uint32_t ipNetOrder, char *dst, size_t dstLen) {
  if (ipNetOrder == 0) {
    snprintf(dst, dstLen, "--");
    return;
  }
  snprintf(dst, dstLen, "%u.%u.%u.%u",
           (unsigned)((ipNetOrder >> 0)  & 0xFFu),
           (unsigned)((ipNetOrder >> 8)  & 0xFFu),
           (unsigned)((ipNetOrder >> 16) & 0xFFu),
           (unsigned)((ipNetOrder >> 24) & 0xFFu));
}

// ============================================================
// CALLBACK EVENTI SoftAP
// ============================================================
// Vincoli rispettati: nessuna Serial, nessuna scrittura SD, nessuna
// allocazione dinamica, nessuna chiamata alle API WiFi. Solo contatori e
// ultimo evento: assegnazioni a 32 bit e memcpy di 6 byte.
static void wifiMonitorEventHandler(arduino_event_t *event) {
  if (event == NULL) return;

  // millis() letto fuori dalla sezione critica: la finestra critica resta di
  // poche assegnazioni e l'event task non accumula latenza.
  const uint32_t nowMs = millis();

  taskENTER_CRITICAL(&s_wifiMonMux);

  switch (event->event_id) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      s_wifiMonState.staConnectedCount++;
      s_wifiMonState.lastEvent  = WIFI_MON_EVENT_STACONNECTED;
      s_wifiMonState.lastStaAid = event->event_info.wifi_ap_staconnected.aid;
      memcpy(s_wifiMonState.lastStaMac,
             event->event_info.wifi_ap_staconnected.mac,
             sizeof(s_wifiMonState.lastStaMac));
      s_wifiMonState.lastAssocMs = nowMs;
      break;

    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      s_wifiMonState.staDisconnectedCount++;
      s_wifiMonState.lastEvent = WIFI_MON_EVENT_STADISCONNECTED;
      // Anche la disassociazione riporta l'identità della stazione: AID/MAC
      // restano quindi "l'ultima stazione vista" (vedi nota in
      // wifi_monitor.h sul motivo non disponibile con IDF v4.4).
      s_wifiMonState.lastStaAid = event->event_info.wifi_ap_stadisconnected.aid;
      memcpy(s_wifiMonState.lastStaMac,
             event->event_info.wifi_ap_stadisconnected.mac,
             sizeof(s_wifiMonState.lastStaMac));
      s_wifiMonState.lastDisconnMs = nowMs;
      break;

    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      s_wifiMonState.dhcpAssignedCount++;
      s_wifiMonState.lastEvent = WIFI_MON_EVENT_STAIPASSIGNED;
      // In IDF v4.4 ip_event_ap_staipassigned_t contiene solo l'IP: il MAC
      // resta quello registrato dagli eventi di associazione.
      s_wifiMonState.lastStaIp  = event->event_info.wifi_ap_staipassigned.ip.addr;
      s_wifiMonState.lastDhcpMs = nowMs;
      break;

    case ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED:
      // Client che sondano l'SSID (probe request broadcast o dirette): provano
      // a collegarsi all'AP anche senza arrivare all'associazione. Nessun
      // campo dell'evento viene registrato (mac/rssi non esposti) e lastEvent
      // resta l'ultimo evento di STAZIONE: come gli altri case, due sole
      // assegnazioni a 32 bit dentro la sezione critica.
      s_wifiMonState.probeRequestCount++;
      s_wifiMonState.lastProbeMs = nowMs;
      break;

    default:
      // Le registrazioni sono filtrate sui quattro eventi sopra: qui non si
      // arriva. Nessun contatore "altro" per non alterare i totali.
      break;
  }

  taskEXIT_CRITICAL(&s_wifiMonMux);
}

// ============================================================
// INIT
// ============================================================
void initWifiMonitor() {
  if (s_wifiMonInitialized) return;
  s_wifiMonInitialized = true;

  // ---- Campionamento della configurazione AP (sola lettura) ----
  // Il firmware configura l'AP una sola volta in setup() (canale 6, 15 dBm) e
  // non la modifica più a runtime: questi valori si leggono una sola volta
  // qui, così /api/boot non esegue chiamate WiFi aggiuntive.
  s_wifiMonMode    = (uint8_t)WiFi.getMode();
  s_wifiMonChannel = (uint8_t)WiFi.channel();
  // WiFi.getTxPower() (esp_wifi_get_max_tx_power) esprime la potenza in passi
  // di 0,25 dBm (es. WIFI_POWER_15dBm = 60): conversione in dBm arrotondata
  // al valore intero più vicino (15 dBm in esercizio).
  s_wifiMonTxPower = (int8_t)(((int32_t)WiFi.getTxPower() + 2) / 4);

  // ---- Abilitazione della ricezione delle probe request (mask eventi) ----
  // Con IDF v4.4 (Arduino ESP32 2.0.17) il bit WIFI_EVENT_MASK_AP_PROBEREQRECVED
  // è la mask di default del driver: finché è impostato, WIFI_EVENT_AP_PROBEREQRECVED
  // non viene consegnato al system event handler e quindi non raggiunge nemmeno
  // il core Arduino (che pure sa tradurlo in ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED).
  // Si legge la mask corrente e si azzera SOLO quel bit: gli altri bit restano
  // invariati. L'effetto è circoscritto alla NOTIFICA degli eventi al software:
  // nessun tocco ad AP, beacon/probe response, SSID, canale, potenza, DHCP,
  // captive portal o comportamento di rete, nessun recovery. L'API è utilizzabile
  // a runtime (richiede solo esp_wifi_init, già avvenuto con WiFi.softAP()).
  uint32_t maskBefore = 0;
  uint32_t maskAfter  = 0;
  if (esp_wifi_get_event_mask(&maskBefore) == ESP_OK) {
    maskAfter = maskBefore & ~((uint32_t)WIFI_EVENT_MASK_AP_PROBEREQRECVED);
  } else {
    // Difensivo (getter non disponibile): nel driver la mask di default ha come
    // unico bit WIFI_EVENT_MASK_AP_PROBEREQRECVED, quindi azzerare la mask
    // equivale a rimuovere il solo filtro delle probe request.
    maskAfter = 0;
  }
  esp_err_t maskErr = esp_wifi_set_event_mask(maskAfter);

  // ---- Registrazione dei quattro eventi SoftAP (sola notifica) ----
  // WiFi.onEvent() aggiunge un handler alla catena già gestita dal core: non
  // riconfigura l'AP, non tocca il DHCP server, non tocca il captive portal.
  // Quattro registrazioni filtrate sullo stesso callback evitano di essere
  // invocati per gli altri eventi WiFi.
  // La registrazione avviene poche decine di ms dopo la creazione del SoftAP:
  // nessuna associazione può andare persa; per le probe request la perdita
  // possibile è limitata ai primissimi ms di vita dell'AP (prima della chiamata
  // sopra il filtro del driver è ancora attivo), quindi i contatori vanno letti
  // come "da telemetria attiva", non come totale dall'accensione.
  WiFi.onEvent(wifiMonitorEventHandler, ARDUINO_EVENT_WIFI_AP_STACONNECTED);
  WiFi.onEvent(wifiMonitorEventHandler, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);
  WiFi.onEvent(wifiMonitorEventHandler, ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED);
  WiFi.onEvent(wifiMonitorEventHandler, ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED);

  Serial.printf("[WIFI-MON] probe request: mask 0x%08x -> 0x%08x (%s)\n",
                (unsigned)maskBefore, (unsigned)maskAfter,
                (maskErr == ESP_OK) ? "ricezione abilitata" : "set_event_mask FALLITA");
  Serial.printf("[WIFI-MON] telemetria SoftAP attiva: mode=%s ch=%u tx=%d dBm\n",
                wifiMonitorModeLabel(s_wifiMonMode),
                (unsigned)s_wifiMonChannel,
                (int)s_wifiMonTxPower);
}

// ============================================================
// LETTURA (snapshot per /api/boot)
// ============================================================
void wifiMonitorGet(wifiMonitorData_t *out) {
  if (out == NULL) return;

  // Copia dello stato sotto la sezione critica del callback: lo snapshot non
  // può osservare un "ultimo evento" composto a metà (es. MAC nuovo con AID
  // vecchio). La sezione critica copre solo copie di memoria.
  uint8_t  rawMac[6];
  uint32_t rawIp;
  uint8_t  lastEvent;

  taskENTER_CRITICAL(&s_wifiMonMux);
  out->staConnectedCount    = s_wifiMonState.staConnectedCount;
  out->staDisconnectedCount = s_wifiMonState.staDisconnectedCount;
  out->dhcpAssignedCount    = s_wifiMonState.dhcpAssignedCount;
  out->probeRequestCount    = s_wifiMonState.probeRequestCount;
  out->lastStaAid           = s_wifiMonState.lastStaAid;
  out->lastAssocMs          = s_wifiMonState.lastAssocMs;
  out->lastDisconnMs        = s_wifiMonState.lastDisconnMs;
  out->lastDhcpMs           = s_wifiMonState.lastDhcpMs;
  out->lastProbeMs          = s_wifiMonState.lastProbeMs;
  memcpy(rawMac, s_wifiMonState.lastStaMac, sizeof(rawMac));
  rawIp     = s_wifiMonState.lastStaIp;
  lastEvent = s_wifiMonState.lastEvent;
  taskEXIT_CRITICAL(&s_wifiMonMux);

  // Formattazione fuori dalla sezione critica (solo nel task chiamante).
  out->lastEventLabel = wifiMonLastEventLabel(lastEvent);
  wifiMonFormatStaMac(rawMac, out->lastStaMac, sizeof(out->lastStaMac));
  wifiMonFormatStaIp(rawIp, out->lastStaIp, sizeof(out->lastStaIp));

  // Configurazione AP: valori campionati all'init, non più modificati.
  out->wifiMode = s_wifiMonMode;
  out->channel  = s_wifiMonChannel;
  out->txPower  = s_wifiMonTxPower;
}

// ============================================================
// ETICHETTA MODALITA' WiFi
// ============================================================
const char* wifiMonitorModeLabel(uint8_t mode) {
  switch (mode) {
    case WIFI_MODE_NULL:  return "NULL";   // WiFi spento (WIFI_OFF)
    case WIFI_MODE_STA:   return "STA";
    case WIFI_MODE_AP:    return "AP";
    case WIFI_MODE_APSTA: return "AP_STA";
    default:              return "UNKNOWN";
  }
}
