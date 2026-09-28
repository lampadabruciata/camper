#ifndef WIFI_MONITOR_H
#define WIFI_MONITOR_H

// ============================================================
// TELEMETRIA WiFi SoftAP (solo osservabilità)
// ============================================================
// Modulo PUAMENTE ADDITIVO: osserva gli eventi del SoftAP e ne espone
// contatori e ultimo evento. NON introduce recovery, NON riavvia l'AP, NON
// tocca DHCP, captive portal, timeout, watchdog, task o storage.
//
// Eventi registrati (WiFi.onEvent(), tre registrazioni filtrate sullo stesso
// callback, quindi nessuna invocazione per gli altri eventi WiFi):
//   ARDUINO_EVENT_WIFI_AP_STACONNECTED    -> staConnectedCount
//   ARDUINO_EVENT_WIFI_AP_STADISCONNECTED -> staDisconnectedCount
//   ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED   -> dhcpAssignedCount (IP concesso
//                                            dal DHCP server interno dell'AP)
//
// NON viene registrato ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED: la ricezione delle
// probe request è disabilitata a compilazione (WIFI_MONITOR_ENABLE_PROBE_REQUEST
// = 0, vedi nota sotto). probeRequestCount/lastProbeMs restano quindi
// permanentemente a 0 e vengono esposti solo per non cambiare il formato dei
// consumatori (/last_diag.txt, /api/boot, crashlog.txt).
//
// Il callback salva SOLO contatori e ultimo evento: nessuna Serial, nessuna
// scrittura SD, nessuna allocazione dinamica, nessuna chiamata alle API WiFi.
//
// Esposizione: blocco "wifi" di /api/boot (src/web/web_manager.cpp).
//
// PROBE REQUEST (DISABILITATE): con Arduino ESP32 2.0.17 (IDF v4.4) il driver
// WiFi non consegna WIFI_EVENT_AP_PROBEREQRECVED all'application finché è
// impostato il bit WIFI_EVENT_MASK_AP_PROBEREQRECVED, che è la mask di default
// del driver (esp_wifi.h: "Default WiFi event mask is
// WIFI_EVENT_MASK_AP_PROBEREQRECVED"). Per ricevere l'evento si dovrebbe
// azzerare SOLO quel bit con esp_wifi_set_event_mask(): è esattamente ciò che fa
// wifiMonitorRearmProbeEventMask(), ma la feature è disabilitata a compilazione
// (WIFI_MONITOR_ENABLE_PROBE_REQUEST = 0), quindi:
//   - la mask del driver viene LASCIATA al default (il bit di probe resta
//     impostato e il driver filtra l'evento);
//   - nessun WiFi.onEvent(ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED) viene registrato;
//   - probeRequestCount/lastProbeMs restano a 0 (campi mantenuti per compatibilità).
// Motivo della scelta: il contatore delle probe, da solo, non distingue "il
// telefono non riesce a collegarsi" da "il telefono sta solo cercando l'SSID":
// genera quindi numero di callback non trascurabile a ogni scansione attiva,
// senza aggiungere informazione diagnostica sul guasto. Per il quadro del
// guasto servono invece activeStaNum/activeStaMacs (chi è associato) e
// dmaMinFree/dmaLargestBlock (il driver WiFi ha memoria DMA per i buffer RX).
//
// Le probe request NON aggiornano lastEvent/lastStaAid/lastStaMac/lastStaIp:
// chi sonda l'SSID non è una stazione associata e, essendo le probe molto più
// frequenti delle associazioni (e provenienti anche da dispositivi che non si
// collegheranno mai), "lastEvent" perderebbe il significato di "ultima stazione
// vista". Il traffico probe ha quindi campi dedicati (probeRequestCount e
// lastProbeMs), qui inerti a 0 con la feature disabilitata.
// ============================================================

#include <stddef.h>
#include <stdint.h>

// ---- FLAG DI COMPILAZIONE DEL MODULO ----

// Ricezione delle probe request: 0 = DISABILITATA (default, mask driver al
// default e nessun callback registrato), 1 = attiva (azzeramento del solo bit
// WIFI_EVENT_MASK_AP_PROBEREQRECVED + registrazione del callback).
#define WIFI_MONITOR_ENABLE_PROBE_REQUEST 0

// Elenco delle MAC delle stazioni attivamente associate (sta_list del driver):
// 0 = non vengono interrogate le API del driver per l'elenco, 1 = attivo.
// Il numero di stazioni (activeStaNum) resta comunque sempre disponibile.
#define WIFI_MONITOR_ENABLE_STA_MAC_LIST 1

// Numero massimo di MAC riportate: allineato a WiFi.softAP(..., 8) di setup(),
// che è il limite di connessioni dell'AP (wifi_sta_list_t contiene
// ESP_WIFI_MAX_CONN_NUM = 15 voci e occupa ~200 byte).
#define WIFI_MONITOR_MAX_STA_MAC 8

// Buffer per l'elenco MAC in formato "aa:bb:cc:dd:ee:ff,aa:bb:cc:dd:ee:ff,..."
// (17 caratteri + separatore per stazione + terminatore).
#define WIFI_MONITOR_STA_MAC_LIST_BUF (WIFI_MONITOR_MAX_STA_MAC * 18 + 1)
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
  // Campi mantenuti nel formato ma INERTI a 0: la ricezione delle probe request
  // è disabilitata (WIFI_MONITOR_ENABLE_PROBE_REQUEST = 0), quindi questi due
  // campi non vengono mai alimentati. Non rimuoverli: /last_diag.txt, /api/boot
  // e crashlog.txt li leggono (o li confronterebbero con versioni precedenti).
  uint32_t probeRequestCount;    // eventi AP_PROBEREQRECVED (sempre 0)

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
  uint32_t lastProbeMs;    // millis() dell'ultima probe request (sempre 0)

  // ---- Configurazione AP campionata in initWifiMonitor() ----
  uint8_t wifiMode; // wifi_mode_t (2 = WIFI_MODE_AP in esercizio)
  uint8_t channel;  // canale AP
  int8_t  txPower;  // potenza TX in dBm (la sorgente è in passi di 0,25 dBm)

  // ---- Stato IMBALZIATO dell'AP (campionato in wifiMonitorGet()) ----
  // Letture senza stato, senza lock e senza allocazioni: sono query al driver,
  // NON modificano nulla dell'AP e non possono far fallire un'associazione.
  uint8_t  activeStaNum;    // stazioni ATTUALMENTE associate (WiFi.softAPgetStationNum)
  uint32_t dmaMinFree;      // byte DMA liberi (MALLOC_CAP_DMA, free size)
  uint32_t dmaLargestBlock; // blocco contiguo DMA più grande (largest free block)
  //   Lettura: il driver WiFi alloca i buffer RX/TX da heap DMA-capable; se
  //   dmaLargestBlock è vicino a 0 i buffer non si riescono ad allocare e
  //   l'associazione fallisce pur con l'SSID visibile. Con ~40 KB di
  //   frammentazione l'AP resta "scansione + timeout" senza mai associare.
} wifiMonitorData_t;

// ============================================================
// NOTA SUL MOTIVO DI DISCONNESSIONE (lastDisconnReason NON esiste)
// ============================================================
// Il motivo della disassociazione NON viene esposto, per due ragioni indipendenti:
//
// 1) Con Arduino core 2.0.17 (ESP-IDF v4.4) wifi_event_ap_stadisconnected_t
//    contiene solo mac/aid/is_mesh_child: il reason code non è disponibile
//    dall'evento (a differenza di IDF 5.x). Non si introdurrebbe quindi nessun
//    parsing di log né modifiche al comportamento della rete; per il motivo della
//    disassociazione resta il log del driver WiFi (livello WARN) sulla Serial.
//
// 2) ARDUINO_EVENT_WIFI_STA_DISCONNECTED NON è l'evento giusto: descrive
//    l'ESP32 in modalità STATION che si disconnette da un Access Point esterno
//    (wifi_sta_disconnected_t), NON un client che si disconnette dal SoftAP di
//    questo dispositivo. In questo firmware il SoftAP gira in WIFI_MODE_AP:
//    l'evento corrispondente è ARDUINO_EVENT_WIFI_AP_STADISCONNECTED, che non
//    viene mai emesso. Registrare STA_DISCONNECTED per ricavare un "motivo di
//    disconnessione del SoftAP" produrrebbe quindi un campo sempre a 0 e
//    semanticamente falso.

// ============================================================
// API DEL MODULO
// ============================================================

// Inizializza il monitor: campiona la configurazione AP, abilita (solo se
// WIFI_MONITOR_ENABLE_PROBE_REQUEST = 1) la ricezione delle probe request e
// registra i callback degli eventi. Va chiamata UNA VOLTA, dopo WiFi.softAP() e
// WiFi.setTxPower() (in main.cpp, subito dopo la creazione del SoftAP), così da
// campionare i valori reali in esercizio e da poter usare
// esp_wifi_set_event_mask() (richiede solo esp_wifi_init, già avvenuto).
// Idempotente.
void initWifiMonitor();

// Riarma la ricezione delle probe request (mask eventi del driver) azzerando
// SOLO il bit WIFI_EVENT_MASK_AP_PROBEREQRECVED: è la stessa operazione eseguita
// da initWifiMonitor(), estratta per poter essere riusata dopo la ricreazione
// del SoftAP.
// Serve perché softAPdisconnect()/softAPConfig()/softAP() provocano il
// deinit/reinit del driver WiFi e la mask eventi torna al valore di default
// (WIFI_EVENT_MASK_AP_PROBEREQRECVED).
// Con WIFI_MONITOR_ENABLE_PROBE_REQUEST = 0 (configurazione in esercizio) la
// funzione è un NO-OP: NON scrive la mask (il bit di probe resta al default e il
// driver continua a filtrare l'evento) e ritorna true. Il chiamante può quindi
// invocarla senza sapere a runtime se la feature è attiva.
// NON registra callback: le WiFi.onEvent() restano quelle registrate una sola
// volta da initWifiMonitor() (nessuna registrazione duplicata, quindi nessun
// contatore raddoppiato). Non riavvia l'AP e non tocca SSID, canale, potenza,
// DHCP, captive portal, telemetria o watchdog; non fa alcun reboot.
// maskBeforeOut/maskAfterOut (opzionali, ammessi NULL) ricevono la mask letta
// dal driver e quella effettivamente in vigore (con feature disabilitata coincide
// con la prima, perché nulla viene scritto).
// Ritorna true se esp_wifi_set_event_mask() ha avuto esito ESP_OK (o se la
// feature è disabilitata e quindi non c'è stata alcuna scrittura).
bool wifiMonitorRearmProbeEventMask(uint32_t *maskBeforeOut, uint32_t *maskAfterOut);

// Copia coerente dello stato corrente in *out (nessuna allocazione dinamica).
// Chiamabile da qualsiasi task; *out == NULL è un no-op.
// Oltre ai contatori, campiona in lettura activeStaNum/dmaMinFree/
// dmaLargestBlock (query al driver e all'heap: nessuna modifica, nessun lock,
// nessuna attesa).
void wifiMonitorGet(wifiMonitorData_t *out);

// Elenco FACOLTATIVO delle MAC delle stazioni attualmente associate al SoftAP,
// letto dalla sta_list del driver (esp_wifi_ap_get_sta_list) e formattato in
// dst come "aa:bb:cc:dd:ee:ff,aa:bb:cc:dd:ee:ff,..." (massimo
// WIFI_MONITOR_MAX_STA_MAC voci, allineato a WiFi.softAP(..., 8) di setup()).
// dst deve avere almeno WIFI_MONITOR_STA_MAC_LIST_BUF byte; se dst è NULL o
// dstLen è insufficiente, dst viene comunque terminato come stringa vuota.
// Ritorna il numero di MAC effettivamente scritte.
//
// Semantica: è uno "istantanea", non un contatore unlike probeRequestCount —
// elenca chi è associato ADESSO, che è l'informazione utile per distinguere
// "il telefono non si è mai associato" (lista vuota) da "si è associato e poi è
// caduto" (lista vuota ma staConnectedCount > 0 e lastDisconnMs != 0).
// Query in sola lettura: non alloca, non prende lock applicativi, non modifica
// l'AP. Compilata solo con WIFI_MONITOR_ENABLE_STA_MAC_LIST = 1; altrimenti è un
// no-op che scrive "" e ritorna 0 (activeStaNum resta comunque disponibile).
uint8_t wifiMonitorGetStaMacList(char *dst, size_t dstLen);

// Etichetta leggibile di wifi_mode_t: "NULL" | "STA" | "AP" | "AP_STA" |
// "UNKNOWN".
const char* wifiMonitorModeLabel(uint8_t mode);

#endif // WIFI_MONITOR_H
