#include "wifi_monitor.h"

#include <Arduino.h>
#include <WiFi.h>
// WiFi.h NON include esp_wifi.h (solo esp_wifi_types.h): serve per
// esp_wifi_get_event_mask()/esp_wifi_set_event_mask() e per il bit
// WIFI_EVENT_MASK_AP_PROBEREQRECVED usati in initWifiMonitor().
#include "esp_wifi.h"
// heap_caps_get_free_size/heap_caps_get_largest_free_block: stato della memoria
// DMA-capable, da cui il driver WiFi preleva i buffer RX/TX. Sono query in sola
// lettura su strutture interne dell'heap (nessun lock, nessuna attesa).
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// SemaphoreHandle_t/xSemaphoreCreateMutex: protegge la wifi_sta_list_t condivisa
// usata da wifiMonitorGetStaMacList() (chiamata da due task).
#include "freertos/semphr.h"

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
  // Con WIFI_MONITOR_ENABLE_PROBE_REQUEST = 0 nessun callback di probe viene
  // registrato: i due campi restano a 0 e sono inerti (vd. wifi_monitor.h).
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
      // Il case resta nel codice ma è IRREACHABLE con
      // WIFI_MONITOR_ENABLE_PROBE_REQUEST = 0 (nessun callback registrato per
      // quell'evento): è lasciato per non perdere il comportamento se il flag
      // viene riattivato.
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
// RIARMO MASK PROBE REQUEST (supporto recovery SoftAP)
// ============================================================
// Estratto dalla sequenza di initWifiMonitor() per essere riusato quando il
// SoftAP viene ricreato (softAPdisconnect() -> softAPConfig() -> softAP()):
// il deinit/reinit del driver riporta la mask eventi al default, quindi con la
// feature attiva la ricezione delle probe andrebbe riabilitata.
// Si azzera SOLO il bit WIFI_EVENT_MASK_AP_PROBEREQRECVED: gli altri bit
// restano invariati.
// NON registra callback: la registrazione resta esclusiva di initWifiMonitor(),
// così una chiamata ripetuta (es. dopo ogni recovery) non può duplicare gli
// handler né raddoppiare i contatori di telemetria.
//
// CON WIFI_MONITOR_ENABLE_PROBE_REQUEST = 0 (configurazione in esercizio) tutta
// la funzione è un NO-OP: si legge la mask solo per poterla riportare nei log e
// si restituisce true senza chiamare MAI esp_wifi_set_event_mask(). Il bit di
// probe resta quindi al default del driver, che continua a filtrare l'evento:
// il recovery non riabilita nulla e non perde nulla, perché il contatore non
// era alimentato in partenza.
bool wifiMonitorRearmProbeEventMask(uint32_t *maskBeforeOut, uint32_t *maskAfterOut) {
  uint32_t maskBefore = 0;

  if (esp_wifi_get_event_mask(&maskBefore) != ESP_OK) {
    // Difensivo (getter non disponibile): nel driver la mask di default ha come
    // unico bit WIFI_EVENT_MASK_AP_PROBEREQRECVED, quindi azzerare la mask
    // equivale a rimuovere il solo filtro delle probe request.
    maskBefore = 0;
  }

  if (maskBeforeOut != NULL) *maskBeforeOut = maskBefore;

#if WIFI_MONITOR_ENABLE_PROBE_REQUEST
  const uint32_t maskAfter = maskBefore & ~((uint32_t)WIFI_EVENT_MASK_AP_PROBEREQRECVED);
  if (maskAfterOut != NULL) *maskAfterOut = maskAfter;

  // Solo mask eventi: qui non esiste alcuna WiFi.onEvent().
  return (esp_wifi_set_event_mask(maskAfter) == ESP_OK);
#else
  // Feature disabilitata: nessuna scrittura, la mask in vigore resta maskBefore.
  if (maskAfterOut != NULL) *maskAfterOut = maskBefore;
  return true;
#endif
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

  // ---- Ricezione delle probe request (solo se abilitata a compilazione) ----
  // Con IDF v4.4 (Arduino ESP32 2.0.17) il bit WIFI_EVENT_MASK_AP_PROBEREQRECVED
  // è la mask di default del driver: finché è impostato, WIFI_EVENT_AP_PROBEREQRECVED
  // non viene consegnato al system event handler e quindi non raggiunge nemmeno
  // il core Arduino (che pure sa tradurlo in ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED).
  // Con WIFI_MONITOR_ENABLE_PROBE_REQUEST = 0 (configurazione in esercizio) NON
  // si tocca la mask e NON si registra il callback: la funzione è un no-op che
  // si limita a riportare la mask letta. Con il flag a 1 si azzera il SOLO bit,
  // centralizzato in wifiMonitorRearmProbeEventMask() (stessa funzione usata dal
  // recovery del SoftAP dopo softAPdisconnect()/softAP()): una sola
  // implementazione, nessuna divergenza tra init e recovery. In entrambi i casi
  // cambia unicamente la NOTIFICA degli eventi al software: nessun tocco ad AP,
  // beacon/probe response, SSID, canale, potenza, DHCP, captive portal o
  // comportamento di rete. L'API è utilizzabile a runtime (richiede solo
  // esp_wifi_init, già avvenuto con WiFi.softAP()).
  uint32_t maskBefore = 0;
  uint32_t maskAfter  = 0;

#if WIFI_MONITOR_ENABLE_PROBE_REQUEST
  const bool maskOk = wifiMonitorRearmProbeEventMask(&maskBefore, &maskAfter);
  Serial.printf("[WIFI-MON] probe request: abilitata, mask 0x%08x -> 0x%08x (%s)\n",
                (unsigned)maskBefore, (unsigned)maskAfter,
                maskOk ? "OK" : "set_event_mask FALLITA");
#else
  // Feature disabilitata: la funzione è un no-op (legge la mask, non la
  // scrive) e soprattutto NON registra il callback di probe qui sotto.
  (void)wifiMonitorRearmProbeEventMask(&maskBefore, &maskAfter);
  Serial.printf("[WIFI-MON] probe request: DISABILITATA (flag di compilazione), "
                "mask invariata 0x%08x, filtro probe %s\n",
                (unsigned)maskBefore,
                (maskBefore & (uint32_t)WIFI_EVENT_MASK_AP_PROBEREQRECVED)
                  ? "attivo (driver scarta l'evento)" : "rimosso");
#endif

  // ---- Registrazione degli eventi SoftAP (sola notifica) ----
  // WiFi.onEvent() aggiunge un handler alla catena già gestita dal core: non
  // riconfigura l'AP, non tocca il DHCP server, non tocca il captive portal.
  // Registrazioni filtrate sullo stesso callback: evitano di essere invocati per
  // gli altri eventi WiFi.
  // La registrazione avviene poche decine di ms dopo la creazione del SoftAP:
  // nessuna associazione può andare persa, quindi i contatori vanno letti come
  // "da telemetria attiva", non come totale dall'accensione.
  WiFi.onEvent(wifiMonitorEventHandler, ARDUINO_EVENT_WIFI_AP_STACONNECTED);
  WiFi.onEvent(wifiMonitorEventHandler, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);
  WiFi.onEvent(wifiMonitorEventHandler, ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED);
#if WIFI_MONITOR_ENABLE_PROBE_REQUEST
  WiFi.onEvent(wifiMonitorEventHandler, ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED);
#endif

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

  // ---- Stato immediato AP e disponibilità memoria DMA ----
  // Query in sola lettura, eseguite FUORI dalla sezione critica e senza alcuna
  // allocazione: sono il "quadro" del guasto "SSID visibile ma il telefono non
  // si collega".
  //   activeStaNum = 0 con staConnectedCount > 0  -> associazione avvenuta e poi
  //                                                   caduta (vedi lastDisconnMs)
  //   activeStaNum = 0 con staConnectedCount = 0  -> mai associato
  //   dmaLargestBlock vicino a 0                   -> il driver WiFi non ha un
  //                                                   blocco contiguo da cui
  //                                                   allocare i buffer RX
  out->activeStaNum = (uint8_t)WiFi.softAPgetStationNum();
  out->dmaMinFree   = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
  out->dmaLargestBlock =
      (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
}

// ============================================================
// ELENCO MAC DELLE STAZIONI ATTIVE (facoltativo)
// ============================================================
// esp_wifi_ap_get_sta_list() chiede la lista al task WiFi e ATTENDE la risposta
// (è bloccante, ~un round-trip): per questo NON può stare in una sezione
// critica (taskENTER_CRITICAL disabiletterebbe gli interrupt per tutta la
// durata dell'attesa).
// I due caller sono task distinti (loop() per /last_diag.txt, task del
// WebServer per /api/boot) e possono sovrapporsi, quindi la wifi_sta_list_t
// condivisa (~200 byte: ESP_WIFI_MAX_CONN_NUM = 15 voci da 8+ byte) è protetta
// da un mutex FreeRTOS. Il buffer è statico (niente peso sullo stack dei
// caller, che sono path con attenzione esplicita allo stack) e la formattazione
// avviene DOPO la copia locale delle MAC, fuori dal mutex.
// Se il mutex non è disponibile si esce con lista vuota: è telemetria
// facoltativa, non vale la pena insistere né aspettare.
uint8_t wifiMonitorGetStaMacList(char *dst, size_t dstLen) {
  if (dst == NULL || dstLen == 0) return 0;
  dst[0] = '\0';

#if WIFI_MONITOR_ENABLE_STA_MAC_LIST
  static SemaphoreHandle_t s_staListMtx = NULL;
  static wifi_sta_list_t   s_staList;
  // Inizializzazione pigra: la prima chiamata (da un task, mai da una ISR)
  // crea il mutex. La corsia è risolta dal fatto che la creazione avviene una
  // sola volta e il risultato è ripubblicato solo a scrittura atomica.
  if (s_staListMtx == NULL) {
    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (created == NULL) return 0;   // esaurimento memoria: lista non disponibile
    s_staListMtx = created;
  }

  if (xSemaphoreTake(s_staListMtx, pdMS_TO_TICKS(50)) != pdTRUE) {
    return 0;  // altro caller sta interrogando il driver: lista vuota
  }

  // Copia locale delle MAC dentro il mutex, così la formattazione (che è la
  // parte lenta) avviene senza bloccare nessuno.
  uint8_t macs[WIFI_MONITOR_MAX_STA_MAC][6];
  uint8_t n = 0;
  const esp_err_t err = esp_wifi_ap_get_sta_list(&s_staList);
  if (err == ESP_OK) {
    // num è validato dal driver, ma resta un valore letto da codice nativo:
    // si blocca comunque all'intervallo utile [0, WIFI_MONITOR_MAX_STA_MAC].
    int num = s_staList.num;
    if (num < 0) num = 0;
    if (num > WIFI_MONITOR_MAX_STA_MAC) num = WIFI_MONITOR_MAX_STA_MAC;
    for (int i = 0; i < num; i++) {
      memcpy(macs[i], s_staList.sta[i].mac, 6);
    }
    n = (uint8_t)num;
  }

  xSemaphoreGive(s_staListMtx);

  if (err != ESP_OK) return 0;  // lista non disponibile: stringa vuota

  // Formattazione fuori dal mutex: snprintf su buffer piccolo e noto (max 8
  // voci da 17 caratteri + separatore = 143 caratteri).
  uint8_t written = 0;
  for (uint8_t i = 0; i < n; i++) {
    // 18 = 17 caratteri di "aa:bb:cc:dd:ee:ff" + separatore, più il terminatore.
    if ((size_t)written + 18u + 1u > dstLen) break;
    const int k = snprintf(dst + written, dstLen - written, "%s%02x:%02x:%02x:%02x:%02x:%02x",
                           (i == 0) ? "" : ",",
                           (unsigned)macs[i][0], (unsigned)macs[i][1],
                           (unsigned)macs[i][2], (unsigned)macs[i][3],
                           (unsigned)macs[i][4], (unsigned)macs[i][5]);
    if (k <= 0) break;
    written += (uint8_t)k;
  }
  return written;
#else
  // Elenco non compilato: activeStaNum resta comunque disponibile.
  return 0;
#endif
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
