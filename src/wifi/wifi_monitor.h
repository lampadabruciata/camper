#ifndef WIFI_MONITOR_H
#define WIFI_MONITOR_H

// ============================================================
// TELEMETRIA WiFi SoftAP (solo osservabilità)
// ============================================================
// Modulo PUAMENTE ADDITIVO: osserva gli eventi del SoftAP e ne espone
// contatori e ultimo evento. NON introduce recovery, NON riavvia l'AP, NON
// tocca DHCP, captive portal, timeout, watchdog, task o storage.
//
// Eventi registrati (WiFi.onEvent(), quattro registrazioni filtrate sullo stesso
// callback, quindi nessuna invocazione per gli altri eventi WiFi):
//   ARDUINO_EVENT_WIFI_AP_STACONNECTED    -> staConnectedCount
//   ARDUINO_EVENT_WIFI_AP_STADISCONNECTED -> staDisconnectedCount
//   ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED   -> dhcpAssignedCount (IP concesso
//                                            dal DHCP server interno dell'AP)
//   ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED  -> probeRequestCount / lastProbeMs
//                                            (client che cercano l'SSID, non
//                                            necessariamente associati)
//
// Il callback salva SOLO contatori e ultimo evento: nessuna Serial, nessuna
// scrittura SD, nessuna allocazione dinamica, nessuna chiamata alle API WiFi.
//
// Esposizione: blocco "wifi" di /api/boot (src/web/web_manager.cpp).
//
// PROBE REQUEST (abilitazione): con Arduino ESP32 2.0.17 (IDF v4.4) il driver
// WiFi non consegna WIFI_EVENT_AP_PROBEREQRECVED all'application finché è
// impostato il bit WIFI_EVENT_MASK_AP_PROBEREQRECVED, che è la mask di default
// del driver (esp_wifi.h: "Default WiFi event mask is
// WIFI_EVENT_MASK_AP_PROBEREQRECVED"). initWifiMonitor() legge la mask con
// esp_wifi_get_event_mask() e azzera SOLO quel bit con esp_wifi_set_event_mask(),
// così che l'evento raggiunga anche il core Arduino (che lo traduce in
// ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED e lo consegna a onEvent). La chiamata
// cambia unicamente la NOTIFICA degli eventi al software: non tocca
// beacon/probe response, SSID, canale, potenza, DHCP o captive portal, non
// riavvia l'AP e non introduce alcun recovery.
//
// Le probe request NON aggiornano lastEvent/lastStaAid/lastStaMac/lastStaIp:
// chi sonda l'SSID non è una stazione associata e, essendo le probe molto più
// frequenti delle associazioni (e provenienti anche da dispositivi che non si
// collegheranno mai), "lastEvent" perderebbe il significato di "ultima stazione
// vista". Il traffico probe ha quindi campi dedicati: probeRequestCount e
// lastProbeMs.
// ============================================================

#include <stdint.h>

// ============================================================
// SNAPSHOT DI TELEMETRIA
// ============================================================
// Copia coerente dello stato interno, riempita da wifiMonitorGet().
// I campi stringa (lastStaMac/lastStaIp) sono costruiti da wifiMonitorGet()
// a partire dai valori grezzi registrati dal callback: valgono "--" finché
// l'evento corrispondente non è mai avvenuto (nel callback non si formatta
// nulla).
typedef struct {
  // ---- Contatori cumulativi da boot ----
  uint32_t staConnectedCount;    // eventi AP_STACONNECTED
  uint32_t staDisconnectedCount; // eventi AP_STADISCONNECTED
  uint32_t dhcpAssignedCount;    // eventi AP_STAIPASSIGNED (IP assegnati)
  uint32_t probeRequestCount;    // eventi AP_PROBEREQRECVED (probe request)

  // ---- Ultimo evento registrato ----
  // Etichetta dell'ultimo evento di stazione osservato:
  // "NONE" | "AP_STACONNECTED" | "AP_STADISCONNECTED" | "AP_STAIPASSIGNED".
  // ("AP_PROBEREQRECVED" non compare: le probe non aggiornano questo campo.)
  // Disambigua i campi "last*" sottostanti, che descrivono l'ultima STAZIONE
  // vista (associazione o disassociazione) e non l'ultimo evento in assoluto.
  const char *lastEventLabel;

  uint32_t lastStaAid;     // AID dell'ultima stazione vista (0 = mai vista)
  char     lastStaMac[18]; // MAC "aa:bb:cc:dd:ee:ff" ("--" se mai visto)
  char     lastStaIp[16];  // ultimo IP assegnato ("192.168.4.2", "--" se mai)

  uint32_t lastAssocMs;    // millis() dell'ultima associazione (0 = mai)
  uint32_t lastDisconnMs;  // millis() dell'ultima disassociazione (0 = mai)
  uint32_t lastDhcpMs;     // millis() dell'ultima assegnazione IP (0 = mai)
  uint32_t lastProbeMs;    // millis() dell'ultima probe request (0 = mai)

  // ---- Configurazione AP campionata in initWifiMonitor() ----
  uint8_t wifiMode; // wifi_mode_t (2 = WIFI_MODE_AP in esercizio)
  uint8_t channel;  // canale AP
  int8_t  txPower;  // potenza TX in dBm (la sorgente è in passi di 0,25 dBm)
} wifiMonitorData_t;

// ============================================================
// NOTA SUL MOTIVO DI DISCONNESSIONE
// ============================================================
// Con Arduino core 2.0.17 (ESP-IDF v4.4) wifi_event_ap_stadisconnected_t
// contiene solo mac/aid/is_mesh_child: il reason code NON è disponibile
// dall'evento (a differenza di IDF 5.x). Non viene quindi esposto alcun
// campo "lastDisconnReason" per non introdurre parsing di log o modifiche
// al comportamento della rete. Per il motivo della disassociazione resta
// il log del driver WiFi (livello WARN) sulla Serial.

// ============================================================
// API DEL MODULO
// ============================================================

// Inizializza il monitor: campiona la configurazione AP, abilita la ricezione
// delle probe request (mask eventi del driver, vedi nota in testa) e registra i
// callback degli eventi. Va chiamata UNA VOLTA, dopo WiFi.softAP() e
// WiFi.setTxPower() (in main.cpp, subito dopo la creazione del SoftAP), così da
// campionare i valori reali in esercizio e da poter usare
// esp_wifi_set_event_mask() (richiede solo esp_wifi_init, già avvenuto).
// Idempotente.
void initWifiMonitor();

// Copia coerente dello stato corrente in *out (nessuna allocazione dinamica).
// Chiamabile da qualsiasi task; *out == NULL è un no-op.
void wifiMonitorGet(wifiMonitorData_t *out);

// Etichetta leggibile di wifi_mode_t: "NULL" | "STA" | "AP" | "AP_STA" |
// "UNKNOWN".
const char* wifiMonitorModeLabel(uint8_t mode);

#endif // WIFI_MONITOR_H
