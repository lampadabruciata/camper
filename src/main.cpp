// =======================================================
// ===============  INCLUDE E CONFIGURAZIONE  ============
// =======================================================
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <Wire.h>
#include <RTClib.h>
#include <TimeLib.h>
#include <U8g2lib.h>
#include "HX711.h"
#include <Adafruit_ADS1X15.h>
#include <Preferences.h>
#include <Adafruit_Sensor.h>
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include <Adafruit_LIS3DH.h>
#include <math.h>
#include <freertos/semphr.h>

#include "gas/gas_manager.h"
#include "water/water_manager.h"
#include "level/level_manager.h"
#include "time/time_manager.h"
#include "web/web_manager.h"
#include "storage/graph_storage.h"
#include "storage/crash_logger.h"
#include "dht/dht_manager.h"

SemaphoreHandle_t sdMutex;
SemaphoreHandle_t i2cMutex; 
SemaphoreHandle_t hxMutex;     
SemaphoreHandle_t prefsMutex;  
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;

// === HEARTBEAT DIAGNOSTICI (per individuare task bloccati prima di INT_WDT) ===
volatile uint32_t hbSensorLoop = 0;
volatile uint32_t hbLevelTask = 0;
volatile uint32_t hbDisplayTask = 0;
volatile uint32_t hbLoop = 0;

// === TIMESTAMP TASK (ultimo heartbeat in ms, aggiornati ad ogni loop) ===
volatile unsigned long lastSensorLoopMs = 0;
volatile unsigned long lastLevelTaskMs  = 0;
volatile unsigned long lastDisplayTaskMs = 0;
volatile unsigned long lastLoopMs       = 0;

// === TEMPI MASSIMI DI ESECUZIONE TASK (massimo storico, in µs) ===
volatile uint32_t sensorLoopMaxUs = 0;
volatile uint32_t levelTaskMaxUs  = 0;
volatile uint32_t displayTaskMaxUs = 0;
volatile uint32_t loopMaxUs       = 0;

// === TEMPI MASSIMI BLOCCHI SENSORLOOP (massimo storico, in µs) ===
volatile uint32_t updateCurrentAndVoltageMaxUs = 0;
volatile uint32_t readSolarCurrentMaxUs        = 0;
volatile uint32_t updateDHTMaxUs               = 0;
volatile uint32_t updateGas1MaxUs              = 0;
volatile uint32_t updateGas2MaxUs              = 0;
volatile uint32_t updateLevelMaxUs             = 0;
volatile uint32_t saveAhStateToSDMaxUs         = 0;
volatile uint32_t saveAhToSDMaxUs              = 0;
volatile uint32_t saveGraphToSDMaxUs           = 0;

void i2cRecoverBus(void);  // forward declaration per il recovery I2C
void DisplayTask(void *param);  // forward declaration

#define STATE_ENTER() taskENTER_CRITICAL(&stateMux)
#define STATE_EXIT()  taskEXIT_CRITICAL(&stateMux)

// === CONFIGURAZIONE TASK DHT (task dedicato, bassa priorità) ===
#define ENABLE_DHT_TASK true   // RIATTIVATO: task dedicato (10 s, prio 1) — nessuna lettura DHT in SensorLoop

// ==== QUEUE PER SCRITTURE NVS (Preferences) ====
QueueHandle_t prefsQueue;


enum PrefsCmdType {
  CMD_SAVE_AH_NOW,
  CMD_SAVE_AH_MAX,
  CMD_SAVE_EFF,
  CMD_SAVE_SOLAR_OFFSET,
  CMD_SAVE_LEVEL_R0,
  CMD_SAVE_LEVEL_P0
};


struct PrefsMessage {
  PrefsCmdType cmd;
  float fvalue;  // per i valori float
  int   ivalue;  // per valori interi (es. sign)
};

// Prototipo task dedicato alla NVS
void PrefsTask(void *param);


#include <DNSServer.h>
DNSServer dnsServer;
RTC_DS3231 rtc;

// ---- TEMPO BASE RTC + MILLIS ----
time_t rtcBaseEpoch = 0;         
unsigned long rtcBaseMillis = 0;  
bool timeValid = false;

// ---- FUSO ORARIO AUTOMATICO (Bergamo: CET/CEST) ----
int8_t tzOffsetHours = 1;       // UTC+1 inverno, UTC+2 estate
bool tzIsDST = false;           // true quando è ora legale


// =======================================================
// ======================  DEBUG I/O  =====================
// =======================================================
// Euristico: su ESP32 con bridge USB-UART non possiamo sapere con certezza
// se il terminale è aperto; questo controllo è NON bloccante e "safe".
static inline bool serialConnected() {
  return Serial && (Serial.availableForWrite() > 0);
}

// Macro comode: stampano solo se la seriale è "connessa"
#define DBG_PRINTLN(...)  do { if (serialConnected()) Serial.println(__VA_ARGS__); } while(0)
#define DBG_PRINTF(...)   do { if (serialConnected()) Serial.printf(__VA_ARGS__); } while(0)

// Dichiarazioni esterne (definite in gas_manager.cpp)
extern bool takeMutexWarn(SemaphoreHandle_t m, uint32_t timeoutMs, const char *name);
extern void debugSDWriteResult(const char* file, bool ok, size_t written, uint32_t microsTime);


// =======================================================
// ===================  PIN DEFINITIONS  =================
// =======================================================

// I2C (ADS1115 + RTC + LIS3DH)
#define I2C_SDA 21
#define I2C_SCL 22

// Sensore flusso acqua
#define FLOW_SENSOR_PIN 27

// SD card (VSPI)
#define SD_CS 15   // CS; altri pin VSPI default: MOSI=23, MISO=19, SCK=18

// LED integrato
const int ledPin = 2;

// Cambio pagina display
#define BUTTON_PIN 13   // pulsante per cambiare pagina 
const uint8_t BRIGHT_LOW  = 0x05;    // luminosità bassa
const uint8_t BRIGHT_HIGH = 0x7F;   // luminosità alta

volatile int currentPage = 0;      // 0 = home
volatile unsigned long lastInteraction = 0; // timer inattività

// =================== LIVELLA (LIS3DH) ===================
Adafruit_LIS3DH lis;           // accelerometro per livellamento
#define LIS3DH_ADDR 0x19      

// Valori filtrati (g) e angoli (°)
volatile float lvl_ax = 0.0f, lvl_ay = 0.0f, lvl_az = 1.0f;
volatile float lvl_roll = 0.0f;   // rotazione su asse X (°)
volatile float lvl_pitch = 0.0f;  // rotazione su asse Y (°)
volatile bool  levelReady = false;
volatile bool adsPresent = false;
volatile bool lisPresent = false;
volatile bool levelExclusiveMode = false;
volatile unsigned long lastLevelRequest = 0;
bool freezeSensors = false;   // congela sensori I2C quando la livella è aperta
bool lisInSleep = false;

// Parametri filtro/aggiornamento
const float alphaLevel = 0.85f;         // IIR (0..1)
const uint16_t LEVEL_PERIOD_MS = 200;  // 5 Hz

// Zero (offset) salvati in NVS
float lvl_roll_zero = 0.0f;    // ° da sottrarre a roll
float lvl_pitch_zero = 0.0f;   // ° da sottrarre a pitch

// Soglia "in bolla"
const float LEVEL_OK_DEG = 0.8f;

// HX711 Gas 1
#define HX711_1_DT 32
#define HX711_1_SCK 33

// HX711 Gas 2
#define HX711_2_DT 25
#define HX711_2_SCK 26

// =======================================================
// =====================  DISPLAY OLED  ==================
// =======================================================


U8G2_SH1107_SEEED_128X128_F_4W_HW_SPI display(
    U8G2_R0,
    5,    // CS
    16,   // DC
    17    // RST
);



// =======================================================
// ======================  ADS1115  ======================
// =======================================================
Adafruit_ADS1115 ads;

// --- Secondo ADS1115 dedicato al solare ---
Adafruit_ADS1115 adsSolar;
bool adsSolarPresent = false;


// Gain per canale shunt e batteria
const adsGain_t GAIN_SHUNT = GAIN_SIXTEEN; // ±0.256 V
const adsGain_t GAIN_BATT  = GAIN_TWO;     // ±2.048 V

// Shunt e partitore batteria
const float SHUNT_R = 0.0015f;   // 1.5 mΩ

const float DIV_RATIO = 0.09259f; //partitore resistivo R1 e R2

// Preferenze (offset e calibrazione)
Preferences prefs;
float offset = 0.0f;           // in Ampere equivalenti
float calibFactor = 1.0f;      // fattore di calibrazione

// ============================================================
// DIAGNOSTICA REBOOT — FONTE DI VERITÀ = crashlog.txt
// ============================================================
// Il contatore di riavvii non usa più l'NVS (bootCount). Deriva
// esclusivamente dal numero di eventi in /crashlog.txt (crash logger).
// Il concetto "già visto" vive nel localStorage del browser.
volatile float currentFiltered = 0.0f;  // corrente filtrata per visualizzazione
const float alphaCurrent = 0.6f;

// Campioni per media differenziale
const int SAMPLES_READ = 40;

volatile float solarCurrentFiltered = 0.0f;
const float alphaSolar = 0.6f;   // stesso filtro della corrente batteria




// ====================== TEMPERATURE =========================


// Le istanze fisiche DHT (dhtExt, dhtInt) sono definite in
// dht_manager.cpp e dichiarate extern in dht_manager.h.

// =======================================================
// ===================  GAS (HX711)  =====================
// =======================================================
HX711 scale1;  // bombola 1
HX711 scale2;  // bombola 2

float GAS_SCALE1 = 27550.0;   // gas1 calibrazione
float GAS_SCALE2 = 25760.0;   // gas2 calibrazione

// Capacità nominale bombole (kg)
const float GAS_MAX_1 = 10.0f;
const float GAS_MAX_2 = 10.0f;

// NUOVA LOGICA GAS — variabili cumulative
float gasInitial1 = 0.0f;
float gasConsumed1 = 0.0f;
float gasResidual1 = 0.0f;
float lastScale1Reading = 0.0f;

float gasInitial2 = 0.0f;
float gasConsumed2 = 0.0f;
float gasResidual2 = 0.0f;
float lastScale2Reading = 0.0f;

unsigned long lastGasSave1 = 0;
unsigned long lastGasSave2 = 0;

bool scale1Ready = false;
bool scale2Ready = false;


// =======================================================
// ==================  ACQUA (YF-S402C)  =================
// =======================================================
volatile unsigned long pulseCount = 0;
volatile unsigned long lastPulseMicros = 0;

const float calibrationFactor = 450.0f;   // impulsi/L
float tankCapacity = 100.0f;        // capacità serbatoio (L) - accessibile da water_manager

volatile float totalLitersConsumed = 0.0f;
volatile float lastFlowLmin = 0.0f;
volatile float remainingLiters = tankCapacity;

// =======================================================
// ==========  LOG GRAFICO TENSIONE (24h)  ===============
// =======================================================
const int numSamples = 1440;     // 24h a 1 punto/min
float  voltageSamples[numSamples];
char timeLabels[numSamples][6];
int sampleIndex = 0;
float voltageSum  = 0.0f;
uint32_t epochMinutes[numSamples];
time_t lastSampleMinute = 0;

// ==========  LOG GRAFICO CORRENTE (24h) =================
float currentSamples[numSamples];
float currentSum  = 0.0f;

// === LOG GRAFICO CORRENTE SOLARE (24h) ===
float solarCurrentSamples[numSamples];
float solarCurrentSum = 0.0f;

int currentHour   = 0;
int currentMinute = 0;
int currentDay = 1;
int currentMonth = 1;
int currentYear = 2025;


volatile float latestVoltage = 0.0f;
volatile float vBattery      = 0.0f;
volatile float solarAmps     = 0.0f;
volatile float solarWatts    = 0.0f;
volatile float solarAhToday  = 0.0f;
volatile float solarMilliVolts = 0.0f; 

Preferences prefsSolar;
float solarOffsetV = 0.0f;   // offset in Volt (tensione letta a 0A)


// =======================================================
// ======================  WIFI/WEB  =====================
// =======================================================
const char* ssid = "camper_lamini";
const char* password = "12345678";

WebServer server(80);

// =======================================================
// =========================  SD  ========================
// =======================================================
File logFile;
bool sdOk = false;
volatile bool graphRestored = false;

struct AhStateFile {
    uint32_t magic;
    uint32_t epoch;
    float ahNow;
    float battVolt;
};

#define AH_STATE_MAGIC 0x41485354UL
const char *AH_STATE_FILE = "/ah_state.bin";

// ===== AH DAY FILE - Header con data =====
struct AhDayHeader {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
};

// ===== SD DEFERRED WRITE SYSTEM =====
volatile bool sdDeferred = false;
volatile unsigned long lastWebActivity = 0;


bool initSD() {

    // 🔒 Mutex SD (safe anche se chiamata prestissimo)
    if (sdMutex == NULL) {
      sdMutex = xSemaphoreCreateMutex();
      if (sdMutex == NULL) return false;
    }
    // Usa un take "raw" qui per evitare logCrash ricorsivi
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(500)) != pdTRUE)
      return false;

    // Se non è già inizializzata → inizializza con retry multipli
    if (!sdOk) {
        Serial.println("[SD] Inizializzazione SD...");

        bool ok = false;

        // ✅ 20 tentativi = circa 2 secondi totali
        for (int i = 0; i < 20; i++) {
            ok = SD.begin(SD_CS);
            if (ok) {
                Serial.printf("[SD] SD pronta al tentativo %d\n", i + 1);
                break;
            }
            delay(100); // attesa tra i tentativi
        }

        sdOk = ok;

        if (!sdOk) {
            Serial.println("[SD] ERRORE: impossibile inizializzare la SD dopo 20 tentativi!");
        }
    }

    xSemaphoreGive(sdMutex);
    return sdOk;
}

// >>> BLOCCO AH #1 — Variabili globali energia (Ah)
Preferences prefsAh;
TaskHandle_t SensorTask;


// === DISPLAY TASK (nuovo, thread separato) ===
TaskHandle_t DisplayTaskHandle = NULL;
volatile bool displayNeedUpdate = false;

// === ALTRE TASK: handle per diagnostica stack (solo lettura) ===
TaskHandle_t PrefsTaskHandle      = NULL;
TaskHandle_t SDWatchdogTaskHandle = NULL;
TaskHandle_t LevelTaskHandle      = NULL;


float batteryAh_max = 50.0f;     // Ah totali disponibili (valore iniziale)
float batteryAh_now = 50.0f;     // Ah residui correnti
bool freezeAhIntegration = false;
float chargeEfficiency = 0.85f;  // Efficienza di carica AGM
unsigned long lastAhUpdateMillis = 0;

// >>> DISPLAY RENDERING CONTROL — single writer
volatile bool displayDirty = false;
volatile uint8_t desiredContrast = BRIGHT_LOW;
volatile bool    isDimmed        = false;       // true quando siamo in luminosità bassa
volatile unsigned long nextDimAt = 0;           // timestamp per passare a LOW
volatile bool forceGasSave1 = false;
volatile bool forceGasSave2 = false;

// >>> BLOCCO AH ORARIO — Variabili

float ahHourlyToday[24];
float ahHourlyYesterday[24];
time_t lastHourlyDayEpoch = 0;  // ✅ traccia l'ultima data (epoch) per rollover corretto

// ===== AH ISTOGRAMMI (NUOVO SISTEMA) =====

// >>> DEADBAND ANTI-RUMORE AH (punto unico: tick del minuto) <<<
// Soglia sotto la quale il minuto NON viene pubblicato in barre/ore/totali.
// Motivazione: rumore residuo che passa i gate da 10/20 mA genera micro-barre
// tipiche di 1e-5...3e-4 Ah/min; 0.0005 Ah/min (= 30 mA medi per minuto pieno)
// sta >=1.6x sopra il peggior rumore e ~1.7x sotto il piu' piccolo carico
// reale continuo (50 mA -> 8.3e-4 Ah/min). Il SOC non e' influenzato.
const float AH_MINUTE_DEADBAND_AH = 0.0005f;   // 0,5 mAh/min

// ultimi 60 minuti
int minuteIndex = 0;

// accumulo del minuto corrente
float ahMinuteDischargeAccum = 0.0f;
float ahMinuteChargeAccum    = 0.0f;

float ahMinuteDischarge[60] = {0};
float ahMinuteCharge[60]    = {0};

// tracking minuto
time_t lastMinuteEpoch = 0;



// Ah caricati (corrente negativa) per ora
float ahChargeHourlyToday[24];
float ahChargeHourlyYesterday[24];

float ahTodayTotal_global = 0.0f;
float ahYesterdayTotal_global = 0.0f;
float ahChargeTodayTotal_global = 0.0f;
float ahChargeYesterdayTotal_global = 0.0f;

// === SOLARE: Ah prodotti per ora ===
float solAhHourlyToday[24];
float solAhHourlyYesterday[24];

// Totali solari
float solAhTodayTotal_global = 0.0f;
float solAhYesterdayTotal_global = 0.0f;

// >>> DEADBAND ANTI-RUMORE AH — accumulatore RAW del minuto solare <<<
// Il solare NON viene più pubblicato per secondo: qui si accumula il RAW,
// la pubblicazione (istogramma FV / totali) avviene nel tick del minuto
// attraverso lo stesso deadband usato per Ah consumati/caricati.
float solAhMinuteAccum = 0.0f;

// =======================================================
// ============ PROTOTIPI CHE USEREMO DOPO ===============
// =======================================================

void saveHourlyAhToSD();
void saveAhToSD();

bool saveAhStateToSD();
bool restoreAhStateFromSD();

void restoreHourlyAhFromSD();
void restoreAhDayFromSD();


void i2cScan() {
  Serial.println("🔍 Scansione I2C con riconoscimento componenti...");
  uint8_t found = 0;

  for (uint8_t address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    uint8_t error = Wire.endTransmission();

    if (error == 0) {
      found++;

      // ---- RICONOSCIMENTO AUTOMATICO ----
      const char* name = "Sconosciuto";

      switch (address) {
        case 0x18: name = "LIS3DH (accelerometro)"; break;
        case 0x19: name = "LIS3DH alt. (SA0=HIGH)"; break;
        case 0x38: name = "AHT10 / AHT20"; break;
        case 0x48: name = "ADS1115 (ADDR->GND)"; break;
        case 0x49: name = "ADS1115 (ADDR->VCC)"; break;
        case 0x4A: name = "ADS1115 (ADDR->SDA)"; break;
        case 0x4B: name = "ADS1115 (ADDR->SCL)"; break;
        case 0x68: name = "RTC DS3231"; break;
        case 0x57: name = "RTC EEPROM AT24C32"; break;

        default:
          name = "Dispositivo I2C non riconosciuto";
      }

      Serial.printf(" - 0x%02X  →  %s\n", address, name);
    }
  }

  if (found == 0)
    Serial.println("⚠ Nessun device I2C rilevato.");
  else
    Serial.printf("✓ Totale device trovati: %u\n", found);
}


void LevelTask(void *param);

// Livella LIS3DH
bool  initLIS3DH();
void  updateLevel();                   // lettura + calcolo roll/pitch


// Ah
void restoreAhFromPrefs();
void saveAhNowToPrefs();
void saveAhConfigToPrefs();
void saveAhConfigImmediateToPrefs();   // persistenza immediata (config rare: handleSetAh/handleSetAhMax)
void integrateAh(float amps, float dt_hours);
void handleSetAh();
void handleSetAhMax();
void handleSetEff();

// ADC / sensori
float readDifferentialAverage(int n);
void  updateCurrentAndVoltage();

// Acqua
void  saveLastLitersToSD(float liters);
float loadLastLitersFromSD();

// Gas

void  saveGasToSD(const char* filename, float kg);
float loadGasFromSD(const char* filename);


// Web
void  LateRestoreTask(void *param);
void  SDWatchdogTask(void *param);
void  handleManualGas1();
void  handleManualGas2();
//void  handleData();

// Display/sensori
void  drawOLED();
void  SensorLoop(void *pvParameters);

// =======================================================
// =============== DISPLAY TIMEOUT HANDLER ================
// =======================================================
void handleDisplayTimeout() {
    // --- Timeout inattività: UNA SOLA transizione a pagina 0 con luce bassa ---
    if (!isDimmed && millis() >= nextDimAt) {
        isDimmed = true;
        desiredContrast = BRIGHT_LOW;

        displayDirty = true;

        if (currentPage != 0) {
            currentPage = 0;
            if (DisplayTaskHandle) xTaskNotifyGive(DisplayTaskHandle);
        } else {
            // Forza refresh anche se già in pagina 0, per applicare il contrasto
            if (DisplayTaskHandle) xTaskNotifyGive(DisplayTaskHandle);
        }
    }
}


// Interrupt flussometro (deve stare prima di setup)
void IRAM_ATTR pulseCounter();





// RECOVERY BUS I2C  
void i2cRecoverBus(void) {
  if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    Serial.println("[I2C] Recovery skipped (mutex busy)");
    return;
  }
  Wire.end();
  pinMode(I2C_SCL, OUTPUT);
  pinMode(I2C_SDA, INPUT_PULLUP);
  for (int i = 0; i < 9; i++) {
    digitalWrite(I2C_SCL, LOW);  delayMicroseconds(5);
    digitalWrite(I2C_SCL, HIGH); delayMicroseconds(5);
    if (digitalRead(I2C_SDA) == HIGH) break;
  }
  digitalWrite(I2C_SCL, HIGH); delayMicroseconds(5);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(50000);
  Serial.println("[I2C] Bus recovery eseguito");
  if (i2cMutex) xSemaphoreGive(i2cMutex);
}

// =======================================================
// ================  FUNZIONI HX711 GAS  =================
// =======================================================

// =======================================================
// ==================  GESTIONE ACQUA  ===================
// =======================================================

void IRAM_ATTR pulseCounter() {
  unsigned long t = micros();
  if (t - lastPulseMicros > 1000) {   // evita rimbalzi
    pulseCount++;
  }
  lastPulseMicros = t;

}




// =======================================================
// ===============  TASK DHT DEDICATO  ===================
// =======================================================
// Task a bassa priorità (1) sul Core 0, scollegato da
// SensorLoop: le letture DHT non toccano mai il percorso
// critico. Periodo 10 s (DHT21 richiede ≥ 2 s tra letture).
void DHTTask(void *param) {
  esp_task_wdt_add(NULL);
  for (;;) {
    uint32_t start = micros();

    // Aggiorna temp/hum + gestione quarantena per sensore
    updateDHT();
    uint32_t dt = micros() - start;
    if (dt > updateDHTMaxUs) updateDHTMaxUs = dt;

    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(10000));  // periodo 10 s
  }
}


// =======================================================
// ====================  ADS1115  ========================
// =======================================================
//
//   Funzione: readDifferentialAverage(int n)
//   Lettura differenziale ADS1115 con:
//     - mediana a blocchi (robusta contro rumore)
//     - budget dinamico (ridotto sotto livella)
//     - mutex I2C per task concorrenti
//     - supporto live alla livella (LIS3DH → display)
// =======================================================

float readDifferentialAverage(int n) {
    if (!adsPresent) return 0.0f;

    // ---------------------------------------------------
    // Tentativo di prendere il bus I2C (massimo 200 ms)
    // ---------------------------------------------------
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(200)) != pdTRUE)
        return 0.0f;

    unsigned long tStart = millis();

    // ---------------------------------------------------
    // BLOCCO BUDGET DINAMICO
    // ---------------------------------------------------
    // Se la pagina livella è attiva (ultimi 1000 ms),
    // allora riduciamo il BUDGET totale del ciclo ADS.
    //
    // Questo NON blocca ADS → continua a leggere,
    // ma lascia più tempo libero a:
    //   - LIS3DH (aggiornamento livella)
    //   - Display (anche se poi verrà congelato)
    //
    bool levelActiveNow = (millis() - lastLevelRequest < 1000);

    // Budget temporale per questo ciclo di acquisizione
    const uint32_t BUDGET_MS = levelActiveNow ? 60 : 100;

    // ---------------------------------------------------
    // Numero blocchi per la mediana
    // ---------------------------------------------------
    const int blk = 5;      // campioni per blocco
    int blocks = n / blk;
    if (blocks < 3) blocks = 3;

    const int MAX_BLOCKS = 20;
    if (blocks > MAX_BLOCKS) blocks = MAX_BLOCKS;

    float blockMeans[MAX_BLOCKS];

    // ---------------------------------------------------
    // Imposta il gain per lo shunt
    // ---------------------------------------------------
    ads.setGain(GAIN_SHUNT);
    delayMicroseconds(3000);   // conversion-ready dopo cambio gain

    // ---------------------------------------------------
    // Acquisizione blocchi sequenziali (rispettando BUDGET)
    // ---------------------------------------------------
    for (int b = 0; b < blocks; b++) {

        float sum = 0.0f;

        for (int i = 0; i < blk; i++) {

            int16_t raw = ads.readADC_Differential_0_1();
            float v = ads.computeVolts(raw);
            sum += v;

            vTaskDelay(1);
        }

        blockMeans[b] = sum / blk;

        // Rispetta il budget
        if (millis() - tStart > BUDGET_MS) {
            blocks = b + 1;   // tieni i blocchi raccolti finora
            break;
        }
    }

    // ---------------------------------------------------
    // Ordina i blocchi per ottenere la mediana
    // ---------------------------------------------------
    for (int i = 0; i < blocks - 1; i++) {
        for (int j = i + 1; j < blocks; j++) {
            if (blockMeans[j] < blockMeans[i]) {
                float tmp = blockMeans[i];
                blockMeans[i] = blockMeans[j];
                blockMeans[j] = tmp;
            }
        }
    }

    // ---------------------------------------------------
    // Calcolo della mediana dei blocchi
    // ---------------------------------------------------
    float result;
    if (blocks % 2 == 1) {
        result = blockMeans[blocks / 2];
    } else {
        result = (blockMeans[blocks / 2 - 1] +
                  blockMeans[blocks / 2]) * 0.5f;
    }

    xSemaphoreGive(i2cMutex);
    return result;
}


// =======================================================
// ========== LETTURA CORRENTE SOLARE (ADS2 A0-A1) =======
// =======================================================

#define SHUNT_SOL_R 0.0025f   // FL-2C: 30A/75mV → 2.5 mΩ

float readSolarCurrent() {

    if (!adsSolarPresent) return 0.0f;
    if (i2cMutex == NULL) return 0.0f;

    // Tentativo rapido (80 ms max) → evita blocchi lunghi
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(80)) != pdTRUE)
        return 0.0f;

    adsSolar.setGain(GAIN_SIXTEEN);

    // ✅ Alleggerito: 3 letture invece di 9 (‑66% carico I2C)
    const int N = 3;
    float buf[N];

    for (int i = 0; i < N; i++) {
        vTaskDelay(1);  // ✅ evita starvation I2C
        int16_t raw = adsSolar.readADC_Differential_0_1();
        buf[i] = adsSolar.computeVolts(raw);
        // ❌ RIMOSSI tutti i delay(3) che bloccavano il bus
    }

    xSemaphoreGive(i2cMutex);

    // ✅ Mediana veloce
    for (int i = 0; i < N - 1; i++)
        for (int j = i + 1; j < N; j++)
            if (buf[j] < buf[i]) {
                float t = buf[i];
                buf[i] = buf[j];
                buf[j] = t;
            }

    float vMed = buf[N / 2];

    // ✅ Applico offset
    float vEff = vMed - solarOffsetV;
    solarMilliVolts = vEff * 1000.0f;

    // ✅ Sempre positivo
    float amps = fabs(vEff / SHUNT_SOL_R);

    // Mini‑soglia
    if (amps < 0.01f)
        amps = 0.0f;

    return amps;
}

// =======================================================
// ====== UPDATE CORRENTE + BATTERIA (Versione A) ========
// =======================================================
void updateCurrentAndVoltage() {

    // Se l’ADS principale è assente → fallback
    if (!adsPresent) {
        static bool once = false;
        if (!once) { vBattery = 12.50f; once = true; }
        currentFiltered = 0.0f;
        latestVoltage = vBattery;
        return;
    }

    float amps_raw = 0.0f;

    // ------------------------------------------------------------------
    // 1) LETTURA TENSIONE BATTERIA (più leggera e molto più veloce)
    // ------------------------------------------------------------------
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(80)) == pdTRUE) {

        ads.setGain(GAIN_BATT);
        delayMicroseconds(3000); 
        
        // Leggiamo solo 3 campioni → 40% del carico originale
        const int N_v = 3;
        float sum = 0.0f;

        for (int i = 0; i < N_v; i++) {
            vTaskDelay(1);  // ✅ lascia respirare sistema
            int16_t raw = ads.readADC_SingleEnded(2);
            sum += ads.computeVolts(raw);
            // NESSUN delayMicroseconds qui (causava blocchi)
        }

        xSemaphoreGive(i2cMutex);

        float v_adc = sum / N_v;
        const float V_OFFSET = 0.00f;
        float v_batt = (v_adc / DIV_RATIO) + V_OFFSET;

        // Filtro IIR alleggerito
        static bool firstVoltage = true;
        if (firstVoltage) {
            vBattery = v_batt;
            firstVoltage = false;
        } else {
            const float alpha = 0.85f;
            vBattery = alpha * vBattery + (1.0f - alpha) * v_batt;
        }
        // ✅ Soglia tensione (stessa logica della corrente)
        if (fabs(vBattery) < 0.01f)
          vBattery = 0.0f;
    }

    // ------------------------------------------------------------------
    // 2) LETTURA CORRENTE (versione SUPER alleggerita)
    // ------------------------------------------------------------------
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(80)) == pdTRUE) {

        ads.setGain(GAIN_SHUNT);

        // Ridotto da 15 → 5 campioni (‑66% carico I2C)
        const int N_i = 5;
        float buf[N_i];

        for (int i = 0; i < N_i; i++) {
            vTaskDelay(1);  // ✅ fondamentale
            int16_t raw = ads.readADC_Differential_0_1();
            buf[i] = ads.computeVolts(raw);
        }

        xSemaphoreGive(i2cMutex);

        // Mediana veloce
        for (int i = 0; i < N_i - 1; i++)
            for (int j = i + 1; j < N_i; j++)
                if (buf[j] < buf[i]) {
                    float t = buf[i];
                    buf[i] = buf[j];
                    buf[j] = t;
                }

        float vmed = buf[N_i / 2];
        float amps = (vmed / SHUNT_R) - offset;
        amps_raw = amps * calibFactor;

        if (fabs(amps_raw) < 0.02f) amps_raw = 0.0f;

        currentFiltered =
            alphaCurrent * currentFiltered +
            (1.0f - alphaCurrent) * amps_raw;

        if (fabs(currentFiltered) < 0.01f)
            currentFiltered = 0.0f;
    }

    latestVoltage = vBattery;
}

// >>> BLOCCO AH #3 — Implementazione funzioni Ah

void restoreAhFromPrefs() {
  prefsAh.begin("energy", false);

  batteryAh_max    = prefsAh.getFloat("ah_max", 50.0f);
  batteryAh_now    = prefsAh.getFloat("ah_now", batteryAh_max);
  chargeEfficiency = prefsAh.getFloat("eff", 0.85f);

  prefsAh.end();

  // 🔒 CLAMP ASSOLUTO: evitare qualsiasi divisione per zero o SOC infinito
  if (batteryAh_max < 1.0f) batteryAh_max = 1.0f;
  if (batteryAh_now > batteryAh_max) batteryAh_now = batteryAh_max;
  if (batteryAh_now < 0) batteryAh_now = 0;

  if (chargeEfficiency < 0.5f || chargeEfficiency > 1.0f)
      chargeEfficiency = 0.85f;
}

void saveAhNowToPrefs() {
  PrefsMessage m;
  m.cmd = CMD_SAVE_AH_NOW;
  m.fvalue = batteryAh_now;
  m.ivalue = 0;
  xQueueSend(prefsQueue, &m, pdMS_TO_TICKS(200));
}

void saveAhConfigToPrefs() {
  // Salva ah_max
  PrefsMessage m1;
  m1.cmd = CMD_SAVE_AH_MAX;
  m1.fvalue = batteryAh_max;
  m1.ivalue = 0;
  xQueueSend(prefsQueue, &m1, pdMS_TO_TICKS(200));

  // Salva ah_now (mantieni consistenza se è cambiato)
  PrefsMessage m2;
  m2.cmd = CMD_SAVE_AH_NOW;
  m2.fvalue = batteryAh_now;
  m2.ivalue = 0;
  xQueueSend(prefsQueue, &m2, pdMS_TO_TICKS(200));

  // Salva efficienza di carica
  PrefsMessage m3;
  m3.cmd = CMD_SAVE_EFF;
  m3.fvalue = chargeEfficiency;
  m3.ivalue = 0;
  xQueueSend(prefsQueue, &m3, pdMS_TO_TICKS(200));
}

// 🔒 PERSISTENZA IMMEDIATA (senza debounce)
// Le operazioni di configurazione (handleSetAh / handleSetAhMax) sono rare:
// scrivono subito in NVS in modo sincrono, così un riavvio immediato non perde
// i valori appena impostati. Il normale funzionamento (integrazione Ah) continua
// invece a usare il canale asincrono con debounce di SAVE_DEBOUNCE_MS nel PrefsTask.
void saveAhConfigImmediateToPrefs() {
  if (prefsMutex == NULL) return;
  if (xSemaphoreTake(prefsMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

  // Handle locale di Preferences: evita di interferire con il prefsAh longevo del PrefsTask.
  Preferences p;
  p.begin("energy", false);
  p.putFloat("ah_max", batteryAh_max);
  p.putFloat("ah_now", batteryAh_now);
  p.end();

  xSemaphoreGive(prefsMutex);
}

void integrateAh(float amps, float dt_hours) {

  if (dt_hours <= 0) return;

  float deltaAh = 0.0f;

  STATE_ENTER();

  if (amps > 0) {
    deltaAh = amps * dt_hours;  // scarica
    batteryAh_now -= deltaAh;
  } else {
    deltaAh = amps * dt_hours * chargeEfficiency;  // carica
    batteryAh_now -= deltaAh;
  }

  if (batteryAh_now > batteryAh_max) batteryAh_now = batteryAh_max;
  if (batteryAh_now < 0)             batteryAh_now = 0;

  STATE_EXIT();

  // ✅ accumulo separato corretto (professionale)

  if (deltaAh > 0) {
      // scarica
      ahMinuteDischargeAccum += deltaAh;
  }
  else if (deltaAh < 0) {
      // carica (deltaAh negativo → lo rendo positivo)
      ahMinuteChargeAccum += -deltaAh;
  }

}




// =======================================================
// ====================  WEB SERVER  =====================
// =======================================================

// --- Bombola 1 ---
void handleManualGas1() {
  if (!server.hasArg("kg")) {
    server.send(400, "text/plain", "errore");
    return;
  }

  float kg = server.arg("kg").toFloat();

  // Aggiorna immediatamente logica cumulativa
  gasInitial1 = kg;
  gasConsumed1 = 0.0f;
  gasResidual1 = gasInitial1;

  // Stabilizza la cella in modo thread-safe
  scaleSafeTare(scale1);
  float startReading = 0.0f;
  if (scaleSafeGetUnits(scale1, 10, startReading)) {
    lastScale1Reading = startReading;
    scale1Ready = true; // opzionale: dichiara pronta dopo set
  }

  // Salva subito su SD
  forceGasSave1 = true;

  // Risposta al client
  server.send(200, "text/plain", "ok");
}

// --- Bombola 2 ---
void handleManualGas2() {
  if (!server.hasArg("kg")) {
    server.send(400, "text/plain", "errore");
    return;
  }

  float kg = server.arg("kg").toFloat();

  gasInitial2 = kg;
  gasConsumed2 = 0.0f;
  gasResidual2 = gasInitial2;

  scaleSafeTare(scale2);
float startReading2 = 0.0f;
if (scaleSafeGetUnits(scale2, 10, startReading2)) {
  lastScale2Reading = startReading2;
  scale2Ready = true;
}

  forceGasSave2 = true;

  server.send(200, "text/plain", "ok");
}



// handleDataAh24h() e handleDataAhLive() spostate in web_manager.cpp
// handleDataSolar() e handleDataBattery() spostate in web_manager.cpp

// =======================================================
// ========================= SETUP ========================
// =======================================================


void handleSetAh() {
    if (!server.hasArg("val")) {
        server.send(400, "text/plain", "missing val");
        return;
    }

    float v = server.arg("val").toFloat();
    if (v < 1) v = 1;  // capacità minima accettabile

    // ✅ Interpretiamo "val" come NUOVA CAPACITÀ DEL PACCO BATTERIA
    STATE_ENTER();
    batteryAh_max = v;     // nuova capacità totale
    batteryAh_now = v;     // batteria piena al 100%
    freezeAhIntegration = false;  // NON congelare l’integrazione AH
    currentFiltered = 0;   // reset corrente
    solarCurrentFiltered = 0;
    STATE_EXIT();

    // ✅ Persistenza IMMEDIATA: evita perdita su riavvio immediato
    saveAhConfigImmediateToPrefs();

    // ✅ Salva tutto in NVS (canale asincrono con debounce, come da architettura)
    saveAhConfigToPrefs();

    // ✅ Reset grafici orari
    for (int i = 0; i < 24; i++) {
        ahHourlyToday[i] = 0.0f;
        ahChargeHourlyToday[i] = 0.0f;
    }

    server.send(200, "text/plain", "ok");
    Serial.printf(">>> NUOVA BATTERIA: %0.1f Ah (100%% carica)\n", v);
}


void handleSetAhMax() {
  if (!server.hasArg("val")) { server.send(400, "text/plain", "missing val"); return; }
  float v = server.arg("val").toFloat();
  if (v < 1) v = 1;

  STATE_ENTER();
  batteryAh_max = v;
  if (batteryAh_now > batteryAh_max) batteryAh_now = batteryAh_max;
  STATE_EXIT();

  // ✅ Persistenza IMMEDIATA: salva ah_max e ah_now (già clampato) senza debounce
  saveAhConfigImmediateToPrefs();

  // ✅ Salva tutto in NVS (canale asincrono con debounce, come da architettura)
  saveAhConfigToPrefs();
  server.send(200, "text/plain", "ok");
}

void handleSetEff() {
  if (!server.hasArg("val")) { server.send(400, "text/plain", "missing val"); return; }
  float e = server.arg("val").toFloat();
  if (e < 0.5f) e = 0.5f;
  if (e > 1.0f) e = 1.0f;

  STATE_ENTER();
  chargeEfficiency = e;
  STATE_EXIT();

  saveAhConfigToPrefs();
  server.send(200, "text/plain", "ok");
}

void setup() {

// --- Scanner I2C con riconoscimento componenti ---

  Serial.begin(115200);
  delay(300);                 // dà tempo al monitor seriale di aprirsi dopo l'upload

  esp_reset_reason_t rsn = esp_reset_reason();
  const char* rsn_s =
    (rsn==ESP_RST_POWERON)?"POWERON":
    (rsn==ESP_RST_EXT)?"EXT":
    (rsn==ESP_RST_SW)?"SW":
    (rsn==ESP_RST_PANIC)?"PANIC":
    (rsn==ESP_RST_INT_WDT)?"INT_WDT":
    (rsn==ESP_RST_TASK_WDT)?"TASK_WDT":
    (rsn==ESP_RST_WDT)?"WDT":
    (rsn==ESP_RST_DEEPSLEEP)?"DEEPSLEEP":
    (rsn==ESP_RST_BROWNOUT)?"BROWNOUT":
    (rsn==ESP_RST_SDIO)?"SDIO":"OTHER";
  Serial.printf("Reset reason: %s (%d)\n", rsn_s, (int)rsn);

  pinMode(ledPin, OUTPUT);

  // ✅ CREA SUBITO I MUTEX (servono a initSD/logCrash)
  sdMutex   = xSemaphoreCreateMutex();
  i2cMutex  = xSemaphoreCreateMutex();
  hxMutex   = xSemaphoreCreateMutex();
  prefsMutex= xSemaphoreCreateMutex();

  // ✅ Queue e PrefsTask possono nascere ora
  prefsQueue = xQueueCreate(10, sizeof(PrefsMessage));
  xTaskCreatePinnedToCore(PrefsTask, "PrefsTask", 4096, NULL, 1, &PrefsTaskHandle, 0);

  // ---- Crash log su SD in caso di reset anomalo ----
  
  switch (rsn) {
   case ESP_RST_POWERON:   logCrash("RESET: POWERON"); break;
   case ESP_RST_EXT:       logCrash("RESET: EXT (USB/RESET)"); break;
   case ESP_RST_SW:        logCrash("RESET: SW"); break;
   case ESP_RST_PANIC:     logCrash("RESET: PANIC"); break;
   case ESP_RST_INT_WDT:   logCrash("RESET: INT_WDT"); break;
   case ESP_RST_TASK_WDT:  logCrash("RESET: TASK_WDT"); break;
   case ESP_RST_WDT:       logCrash("RESET: WDT"); break;
   case ESP_RST_DEEPSLEEP: logCrash("RESET: DEEPSLEEP"); break;
   case ESP_RST_BROWNOUT:  logCrash("RESET: BROWNOUT"); break;
   case ESP_RST_SDIO:      logCrash("RESET: SDIO"); break;
   default:                logCrash("RESET: UNKNOWN"); break;
  }


  // ------------------------
  // I2C (ADS1115 + OLED)
  // ------------------------

  // ✅ RECOVERY HARD I2C ALL'AVVIO
  pinMode(I2C_SCL, OUTPUT);
  pinMode(I2C_SDA, INPUT_PULLUP);

  for (int i = 0; i < 10; i++) {
      digitalWrite(I2C_SCL, LOW);
      delayMicroseconds(5);
      digitalWrite(I2C_SCL, HIGH);
      delayMicroseconds(5);
  }
  
  
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setTimeout(50);
  Wire.setClock(50000);     // 50 kHz per cavi lunghi
  
  // Soft reset del bus I2C per pulire eventuali stati "appesi"
  Wire.end();
  delay(5);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(50000);

  i2cScan();

  // ✅ SE NON TROVA NULLA → RECOVERY AUTOMATICO
  delay(50);
  if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      Serial.println("[I2C] check devices...");
      xSemaphoreGive(i2cMutex);
  }

  if (!adsPresent && !lisPresent) {
      Serial.println("[I2C] Nessun device → provo recovery");

      i2cRecoverBus();
      delay(50);

      i2cScan();   // riprova
  }



  // ----------- RTC DS3231 -----------
  if (rtc.begin()) {
    DateTime now = rtc.now();
    if (now.year() >= 2023) { // tempo plausibile
      rtcBaseEpoch = now.unixtime();
      rtcBaseMillis = millis();
      timeValid = true;
      // ✅ Inizializza le variabili globali con i valori del RTC
      currentYear = now.year();
      currentMonth = now.month();
      currentDay = now.day();
      currentHour = now.hour();
      currentMinute = now.minute();
      Serial.printf("[RTC] Avvio da RTC: %04d-%02d-%02d %02d:%02d\n",
        now.year(), now.month(), now.day(), now.hour(), now.minute());
    } else {
      Serial.println("[RTC] RTC presente ma ora NON valida");
    }
  } else {
    Serial.println("[RTC] RTC non trovato");
  }

  display.begin();
  display.setBusClock(4000000); // più stabile

 

  // ----------- Sensori DHT (interno + esterno) -----------
  initDHT();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // ------------------------
  // Sensore ACQUA (interrupt)
  // ------------------------
  pinMode(FLOW_SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), pulseCounter, FALLING);

  // ------------------------
  // HX711 — GAS 1
  // ------------------------
  scale1.begin(HX711_1_DT, HX711_1_SCK);
  scale1.set_scale(GAS_SCALE1);
  scale1.tare();   // azzeramento

  // ------------------------
  // HX711 — GAS 2
  // ------------------------
  scale2.begin(HX711_2_DT, HX711_2_SCK);
  scale2.set_scale(GAS_SCALE2);
  scale2.tare();

  // Ripristino valori gas da SD
  restoreGasFromSD();

  // ----------- LIVELLA (LIS3DH) -----------
  Preferences prefsLevel;
  prefsLevel.begin("level", false);
  lvl_roll_zero  = prefsLevel.getFloat("r0", 0.0f);
  lvl_pitch_zero = prefsLevel.getFloat("p0", 0.0f);
  prefsLevel.end();

  bool lisOk = initLIS3DH();
  lisPresent = lisOk;
  
  if (!lisOk) {
    Serial.println("LIS3DH non trovato (livellamento disattivato)");
  } else {
    Serial.println("LIS3DH OK (livella a 30 Hz in fast / 3 Hz in slow)");
  }

  // ------------------------
  // ADS1115 — corrente + batteria
  // ------------------------
  prefs.begin("shunt", false);
  
  // --- Calibrazione SOLARE ---
  prefsSolar.begin("solar", false);
  solarOffsetV = prefsSolar.getFloat("offV", 0.0f);
  prefsSolar.end();
    
  calibFactor = prefs.getFloat("cal", 1.0f);
  offset      = prefs.getFloat("off", 0.0f);
  prefs.end();


bool adsOk = false;


if (i2cMutex != NULL && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
  adsOk = ads.begin();
  if (adsOk) {
    ads.setGain(GAIN_SHUNT);   // gain iniziale per shunt batteria
  }
  xSemaphoreGive(i2cMutex);
} else {
  adsOk = ads.begin();
  if (adsOk) {
    ads.setGain(GAIN_SHUNT);
  } else {
    Serial.println("⚠ ADS MAIN: begin() fallito (mutex assente o bus non pronto)");
  }
}

adsPresent = adsOk;
if (!adsOk) {
  Serial.println("⚠ ADS1115 NON trovato — continuo senza ADC (fallback attivo)");
} else {
  Serial.println("ADS MAIN OK (0x48)");
}

 // ------------------------
// SECONDO ADS1115 — solare (differenziale A0-A1)
// ------------------------
bool adsSolarOk = false;

// Prima scelta: se il mutex I2C esiste, usalo
if (i2cMutex != NULL && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(200)) == pdTRUE) {

    adsSolarOk = adsSolar.begin(0x49);      
    if (adsSolarOk) {
        adsSolar.setGain(GAIN_SIXTEEN);  // ±256 mV → ideale per shunt solare
    }

    xSemaphoreGive(i2cMutex);

} else {
    adsSolarOk = adsSolar.begin();
    if (adsSolarOk) {
        adsSolar.setGain(GAIN_SIXTEEN);
    } else {
        Serial.println("⚠ ADS SOLARE: begin() fallito (mutex assente o bus non pronto)");
    }
}

adsSolarPresent = adsSolarOk;

if (!adsSolarOk) {
    Serial.println("⚠ ADS SOLARE NON trovato!");
} else {
    Serial.println("ADS SOLARE OK (0x49)");
}


  // ------------------------
  // WiFi Access Point
  // ------------------------
  IPAddress local_IP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);

  WiFi.softAPdisconnect(true);
  delay(100);

  WiFi.setHostname("camper");
  WiFi.persistent(false);

  WiFi.softAPConfig(local_IP, gateway, subnet);

  WiFi.softAP(ssid, password, 6, false, 8);

  WiFi.setTxPower(WIFI_POWER_15dBm);

  dnsServer.start(53, "*", local_IP);

  // ------------------------
  // SPIFFS
  // ------------------------
  SPIFFS.begin(true);

  // Livella: prova a caricare zero da SD (se presente). Se non c'è, restano i valori di NVS.
  {
    float r0 = 0, p0 = 0;
    if (loadLevelZeroFromSD(r0, p0)) {
      lvl_roll_zero  = r0;
      lvl_pitch_zero = p0;
      Serial.printf("Level zero ripristinato da SD: r0=%.3f°, p0=%.3f°\n", r0, p0);
    } else {
      Serial.printf("Level zero da NVS: r0=%.3f°, p0=%.3f°\n", lvl_roll_zero, lvl_pitch_zero);
    }
  }


  // ------------------------
  // Litri consumati (ripristino)
  // ------------------------
  restoreLitersFromSD();
 
 
  // >>> BLOCCO AH #5 — Ripristino Ah
  restoreAhFromPrefs();      // ✅ SEMPRE: batteryAh_max + chargeEfficiency da NVS
  restoreAhStateFromSD();    // ✅ batteryAh_now da SD (se disponibile), poi CLAMP
  restoreHourlyAhFromSD();
  restoreAhDayFromSD();
  
  // ✅ Inizializza lastHourlyDayEpoch per il rollover basato su data
  if (timeValid) {
    lastHourlyDayEpoch = nowEpoch() / 86400;
  }
  
  lastAhUpdateMillis = millis();
  freezeAhIntegration = false;   // ✅ assicurati che l'integrazione sia ATTIVA all'avvio
  
 
  // ------------------------
  // WebServer routing
  // ------------------------
  server.on("/",            handleIndex);
  server.on("/chart.min.js", handleChartJs);
  server.on("/setTime",     handleSetTime);
 
  // ✅ Nuovi endpoint separati
  server.on("/data/live", handleDataLive);
  server.on("/data/battery", handleDataBattery);
  server.on("/data/solar", handleDataSolar);
  server.on("/data/water", handleDataWater);
  server.on("/data/gas", handleDataGas);
  server.on("/data/ah24h", handleDataAh24h); 
  server.on("/data/ahLive", handleDataAhLive); 
  server.on("/api/boot",   handleDataBoot);
  server.on("/api/crash",  handleDataCrashList);
  server.on("/api/crashdetail", handleDataCrashDetail);
  server.on("/manualGas1",  handleManualGas1);
  server.on("/manualGas2",  handleManualGas2);
  server.on("/setTank",     handleSetTank);

  server.on("/data",                []() {
    server.send(410, "text/plain", "Endpoint /data dismesso");
  });

  server.on("/generate_204",        []() {
    server.send(200, "text/plain", "OK");
  });
  server.on("/gen_204",             []() {
    server.send(200, "text/plain", "OK");
  });
  server.on("/hotspot-detect.html", []() {
    server.send(200, "text/plain", "OK");
  });
  server.on("/connecttest.txt",     []() {
    server.send(200, "text/plain", "OK");
  });
  server.on("/ncsi.txt",            []() {
    server.send(200, "text/plain", "OK");
  });
  server.on("/favicon.ico",         []() {
    server.send(204, "image/x-icon", "");
  });

  // Energia / Ah
  server.on("/setAh",    handleSetAh);
  server.on("/setAhMax", handleSetAhMax);
  server.on("/setEff",   handleSetEff);


// Forza reload calibrazione da SD
server.on("/levelZeroLoad",         []() {
  float r0 = 0, p0 = 0;
  if (loadLevelZeroFromSD(r0, p0)) {
    lvl_roll_zero  = r0;
    lvl_pitch_zero = p0;

    // aggiorna anche NVS per coerenza
    PrefsMessage mr, mp;
    mr.cmd    = CMD_SAVE_LEVEL_R0;  mr.fvalue = r0;  mr.ivalue = 0;
    mp.cmd    = CMD_SAVE_LEVEL_P0;  mp.fvalue = p0;  mp.ivalue = 0;
    xQueueSend(prefsQueue, &mr, 0);
    xQueueSend(prefsQueue, &mp, 0);

    server.send(200, "text/plain", "OK: level zero ricaricato da SD");
  } else {
    server.send(404, "text/plain", "nessun level zero su SD");
  }
});

// Cancella calibrazione su NVS e SD
server.on("/levelZeroClear",         []() {
  // RAM
  lvl_roll_zero  = 0.0f;
  lvl_pitch_zero = 0.0f;

  // NVS
  PrefsMessage mr, mp;
  mr.cmd    = CMD_SAVE_LEVEL_R0;  mr.fvalue = 0.0f;  mr.ivalue = 0;
  mp.cmd    = CMD_SAVE_LEVEL_P0;  mp.fvalue = 0.0f;  mp.ivalue = 0;
  xQueueSend(prefsQueue, &mr, 0);
  xQueueSend(prefsQueue, &mp, 0);

  // SD
  clearLevelZeroOnSD();

  server.send(200, "text/plain", "OK: level zero cancellato (NVS+SD)");
});


  // Calibrazione zero livella: memorizza roll/pitch attuali come 0°
  server.on("/levelZero",           []() {
    captureLevelZero();
    server.send(200, "text/plain", "OK: level zero salvato");
  });

// Endpoint leggero per livella (poll a 5 Hz dal telefono)

server.on("/level", []() {

    // Segna accesso per exclusive mode
    lastLevelRequest = millis();
    Serial.println(">>> /level hit");

    // --- THROTTLE: massimo ~30 Hz (33 ms) ---
    static unsigned long lastLevelReply = 0;
    unsigned long nowL = millis();
    if (nowL - lastLevelReply < 100) {
        DynamicJsonDocument j(128);
        j["roll"]  = lvl_roll;
        j["pitch"] = lvl_pitch;
        j["ok"]    = (fabs(lvl_roll) <= LEVEL_OK_DEG) && (fabs(lvl_pitch) <= LEVEL_OK_DEG);
        String out;
        serializeJson(j, out);
        server.send(200, "application/json", out);
        return;
    }
    lastLevelReply = nowL;

    // Risposta completa
    DynamicJsonDocument j(256);
    j["roll"]  = lvl_roll;
    j["pitch"] = lvl_pitch;

    bool ok = (fabs(lvl_roll) <= LEVEL_OK_DEG) && (fabs(lvl_pitch) <= LEVEL_OK_DEG);
    j["ok"] = ok;
  
    String out;
    serializeJson(j, out);
    server.send(200, "application/json", out);
});
  
 server.on("/calSolarZero", []() {

  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(300)) == pdTRUE) {

    // Lettura offset dal SECONDO ADS in differenziale A0-A1
    adsSolar.setGain(GAIN_SIXTEEN);
    delay(3);

    int16_t raw = adsSolar.readADC_Differential_0_1();
    float v = adsSolar.computeVolts(raw);

    xSemaphoreGive(i2cMutex);

    solarOffsetV = v;   // salva l’offset differenziale reale

    PrefsMessage mq;
    mq.cmd = CMD_SAVE_SOLAR_OFFSET;
    mq.fvalue = solarOffsetV;
    mq.ivalue = 0;
    xQueueSend(prefsQueue, &mq, 0);

    server.send(200, "text/plain", "OK: offset solare aggiornato");

  } else {
    server.send(500, "text/plain", "errore mutex i2c");
  }

});


  server.begin();
  
  xTaskCreatePinnedToCore(
  LateRestoreTask,
  "LateRestore",
  4096,
  NULL,
  1,
  NULL,
  0   // ✅ Core 0 (NON Wi‑Fi)
  );
  
  Serial.println(">>> WebServer STARTED <<<");
  
  xTaskCreatePinnedToCore(SDWatchdogTask, "SDWatchdog", 2048, NULL, 1, &SDWatchdogTaskHandle, 0);

  // ------------------------
  // Inizializzazione array grafico (24h)
  // ------------------------
  if (!graphRestored) {
   for (int i = 0; i < numSamples; i++) {
     voltageSamples[i] = 0.0f;
     currentSamples[i] = 0.0f;
     snprintf(timeLabels[i], 6, "--:--");
     epochMinutes[i] = 0;
    }
  }
  
  xTaskCreatePinnedToCore(
    SensorLoop,
    "SensorLoop",
    12288,
    NULL,
    2,            // priorità
    &SensorTask,
    0
  );


  // === Avvio DisplayTask sul Core 1 ===
  xTaskCreatePinnedToCore(
      DisplayTask,
      "DisplayTask",
      4096,
      NULL,
      3,
      &DisplayTaskHandle,
      1
  );

  xTaskCreatePinnedToCore(
  LevelTask,
  "LevelTask",
  4096,
  NULL,
  4,
  &LevelTaskHandle,
  1   // esegui sul Core 1, separato dal SensorLoop
  );
  
  // Task dedicato al DHT (Core 0, priorità 1, periodo 10 s).
  // Non condivide il percorso critico con SensorLoop.
  #if ENABLE_DHT_TASK
  xTaskCreatePinnedToCore(
    DHTTask,
    "DHTTask",
    4096,
    NULL,
    1,
    NULL,
    0
  );
  #endif

  // Primo redraw all'avvio
  displayDirty = true;
  if (DisplayTaskHandle) xTaskNotifyGive(DisplayTaskHandle);


  desiredContrast = BRIGHT_HIGH;
  isDimmed  = false;
  nextDimAt = millis() + 30000UL;  // tra 30s si spegne la luce se non interagisci

  logBoot();

  DBG_PRINTLN("Setup completato (versione A).");

}

//============================
// TASK SENSORI + DISPLAY
//===========================
void SensorLoop(void *pvParameters) {
    
    esp_task_wdt_add(NULL);

    for (;;) {
        uint32_t loopStartUs = micros();
        hbSensorLoop++;
        lastSensorLoopMs = millis();

        unsigned long now = millis();

        // ---- ORA CORRENTE DA RTC (convertita in ora locale) ----
        if (timeValid) {
          // Aggiorna DST periodicamente
          updateDST();

          time_t tLocal = utcToLocal(nowEpoch());
          tmElements_t tm;
          breakTime(tLocal, tm);
          currentHour = tm.Hour;
          currentMinute = tm.Minute;
          currentDay = tm.Day;
          currentMonth = tm.Month;
          currentYear = tm.Year + 1970;
        }

        // ============================================================
        // DHT — gestito SOLO da DHTTask (task dedicato, 10 s, prio 1):
        // nessuna lettura DHT nel percorso critico di SensorLoop.
        // ============================================================

        // ============================================================
        // LIVELLA EXCLUSIVE MODE (se la pagina /level è aperta)
        // ============================================================
        bool levelHighPriority = (now - lastLevelRequest < 2000);
        freezeSensors = levelHighPriority;

        // ✅ aiuta a scaricare pressione I2C quando la livella è attiva
        if (freezeSensors) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }


        // Log stato freeze
        static bool wasFreeze = false;
        if (wasFreeze != freezeSensors) {
            Serial.printf("freezeSensors=%d\n", freezeSensors ? 1 : 0);
            wasFreeze = freezeSensors;
        }

       
        // ============================================================
        // 2) CAMBIO GIORNO AH (basato su data RTC, non solo ora)
        // ============================================================
        bool dayRolled = false;
        
        if (timeValid) {
          // Calcola l'epoch del giorno corrente (mezzanotte)
          time_t currentDayEpoch = nowEpoch() / 86400;
          
          if (lastHourlyDayEpoch != 0 && lastHourlyDayEpoch != currentDayEpoch) {
            dayRolled = true;
          }
          lastHourlyDayEpoch = currentDayEpoch;
        }

        // ===== SALVATAGGIO ORARIO AH =====
        static int lastHourSaved = -1;

        if (currentHour != lastHourSaved) {
            lastHourSaved = currentHour;
            saveHourlyAhToSD();
        }

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
            ahYesterdayTotal_global = ahTodayTotal_global;       // ✅ trasferisci totale ieri
            ahChargeYesterdayTotal_global = ahChargeTodayTotal_global;  // ✅ trasferisci totale ieri
            ahTodayTotal_global = 0;
            ahChargeTodayTotal_global = 0;
            solAhYesterdayTotal_global = solAhTodayTotal_global;  // ✅ trasferisci totale ieri
            solAhTodayTotal_global = 0;
        }

        

  
        // ============================================================
        // 3) SENSORI LENTI + PRIORITÀ LIVELLA (VERSIONE STABILE)
        // ============================================================

        if (!freezeSensors) {

            // =========================
            // MODALITÀ NORMALE
            // =========================

            // Lettura completa sensori elettrici
            
            // [DIAG] misura tempo updateCurrentAndVoltage()
            uint32_t tUpdV = micros();
            updateCurrentAndVoltage();
            uint32_t dtUpdV = micros() - tUpdV;
            if (dtUpdV > updateCurrentAndVoltageMaxUs) updateCurrentAndVoltageMaxUs = dtUpdV;

            vTaskDelay(2);
            // [DIAG] misura tempo readSolarCurrent()
            uint32_t tSolar = micros();
            float rawSolar = readSolarCurrent();
            uint32_t dtSolar = micros() - tSolar;
            if (dtSolar > readSolarCurrentMaxUs) readSolarCurrentMaxUs = dtSolar;

            solarCurrentFiltered =
                alphaSolar * solarCurrentFiltered +
                (1.0f - alphaSolar) * rawSolar;

            solarAmps = solarCurrentFiltered;

            // 🔥 Soglia anti-rumore: elimina valori piccolissimi
            if (fabs(solarAmps) < 0.005f)   // 5 mA
                solarAmps = 0.0f;
            
            solarWatts = solarAmps * vBattery;

        } else {

            // =========================
            // MODALITÀ LIVELLA ATTIVA
            // =========================

            // ❗ NON usare I2C qui
            // ❗ NON leggere ADS
            // ❗ NON interferire con LIS
        
            // Mantieni ultimi valori validi (no buchi grafici)

            // Piccola pausa per:
            // - lasciare CPU libera
            // - dare priorità al LevelTask
            vTaskDelay(pdMS_TO_TICKS(8));
        }



        // ============================================================
        // 4) OGNI SECONDO
        // ============================================================
        static unsigned long lastSecondTick = 0;
        if (now - lastSecondTick >= 1000) {
            lastSecondTick = now;
            esp_task_wdt_reset();

            // 4.1 AH
            const float dt_h = 1.0f / 3600.0f;
            
            if (!freezeAhIntegration) {
              float iForAh = currentFiltered;
              if (fabs(iForAh) < 0.01f)
                iForAh = 0.0f;

                integrateAh(iForAh, dt_h);
            }


            // ★ SALVA GLI Ah IN NVS
            
            static float lastSavedAh = -999;
            if (fabs(batteryAh_now - lastSavedAh) >= 0.05f) {
                saveAhNowToPrefs();      // per ora lasciamolo
                // [DIAG] misura tempo saveAhStateToSD()
                uint32_t tAhState = micros();
                saveAhStateToSD();       // nuovo salvataggio SD
                uint32_t dtAhState = micros() - tAhState;
                if (dtAhState > saveAhStateToSDMaxUs) saveAhStateToSDMaxUs = dtAhState;
                lastSavedAh = batteryAh_now;
            }


            // Solar — solo ACCUMULO RAW del minuto (gate esistente 10 mA).
            // Istogramma FV e totali NON sono più scritti qui: vengono pubblicati
            // una volta al minuto nel punto unico di aggregazione (deadband
            // anti-rumore), con lo stesso criterio di Ah consumati/caricati.

            if (solarAmps > 0.01f) {
                solAhMinuteAccum += solarAmps / 3600.0f;
            }

            // Acqua
            unsigned long pulses;
            detachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN));
            pulses = pulseCount;
            pulseCount = 0;
            attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), pulseCounter, FALLING);

            lastFlowLmin = (pulses / calibrationFactor) * 60.0f;
            totalLitersConsumed += pulses / calibrationFactor;
            if (totalLitersConsumed > tankCapacity) totalLitersConsumed = tankCapacity;
            remainingLiters = tankCapacity - totalLitersConsumed;

            // GAS
            // [DIAG] misura tempo updateGas() bilancia 1
            uint32_t tGas1 = micros();
            updateGas(scale1, gasInitial1, gasConsumed1, gasResidual1,
                      lastScale1Reading, "/last_gas1.txt", lastGasSave1, forceGasSave1, scale1Ready);
            uint32_t dtGas1 = micros() - tGas1;
            if (dtGas1 > updateGas1MaxUs) updateGas1MaxUs = dtGas1;

            // [DIAG] misura tempo updateGas() bilancia 2
            uint32_t tGas2 = micros();
            updateGas(scale2, gasInitial2, gasConsumed2, gasResidual2,
                      lastScale2Reading, "/last_gas2.txt", lastGasSave2, forceGasSave2, scale2Ready);
            uint32_t dtGas2 = micros() - tGas2;
            if (dtGas2 > updateGas2MaxUs) updateGas2MaxUs = dtGas2;
        }

// ============================================================
// 5) CAMPIONAMENTO MINUTO (GRAFICI) — BASATO SU RTC
// ============================================================

// accumulo continuo (ogni giro di loop)
voltageSum += latestVoltage;
currentSum += currentFiltered;
solarCurrentSum += solarAmps;

// numero di campioni accumulati in questo minuto
static uint16_t accumCount = 0;
accumCount++;

if (timeValid) {
  time_t nowT = nowEpoch();

  // ===== NUOVO SISTEMA MINUTI → AH =====

time_t thisMinuteEpoch = nowT / 60;

if (thisMinuteEpoch != lastMinuteEpoch) {

    lastMinuteEpoch = thisMinuteEpoch;

    // ✅ prendi i valori separati corretti (RAW, pre-filtro)
    float minuteDischargeRaw = ahMinuteDischargeAccum;
    float minuteChargeRaw    = ahMinuteChargeAccum;

    // ✅ DIAGNOSTICA RUMORE (valori RAW prima del deadband)
    // Si stampa SEMPRE ad ogni cambio di minuto, anche se raw = 0.
    Serial.printf("[AH-NOISE]\nminuteDischarge=%.5f\nminuteCharge=%.5f\n", minuteDischargeRaw, minuteChargeRaw);

    // ============================================================
    // ✅ DEADBAND ANTI-RUMORE — PUNTO UNICO DI AGGREGAZIONE
    // I valori sotto AH_MINUTE_DEADBAND_AH NON vengono scritti né
    // nelle barre da 60 minuti, né negli istogrammi orari, né nei
    // totali giornalieri. Ore, totali e barre usano la STESSA
    // variabile filtrata → somma barre = totale visualizzato.
    // (Il SOC in batteryAh_now NON è toccato: integra a parte.)
    // ============================================================
    float minuteDischarge = 0.0f;
    float minuteCharge    = 0.0f;

    if (minuteDischargeRaw >= AH_MINUTE_DEADBAND_AH)
        minuteDischarge = minuteDischargeRaw;

    if (minuteChargeRaw >= AH_MINUTE_DEADBAND_AH)
        minuteCharge = minuteChargeRaw;

    // ✅ salvi nei buffer live (ultimi 60 minuti)
    ahMinuteDischarge[minuteIndex] = minuteDischarge;
    ahMinuteCharge[minuteIndex]    = minuteCharge;
    Serial.printf("MINUTE AH -> DIS: %.5f  CHG: %.5f\n", minuteDischarge, minuteCharge);

    minuteIndex = (minuteIndex + 1) % 60;

    // ✅ accumuli nelle ore (SOLO QUI!)
    ahHourlyToday[currentHour]       += minuteDischarge;
    ahChargeHourlyToday[currentHour] += minuteCharge;
    
    // ✅ accumulo totali giornalieri globali (stesso valore filtrato → coerenza garantita)
    ahTodayTotal_global       += minuteDischarge;
    ahChargeTodayTotal_global += minuteCharge;

    // ============================================================
    // ✅ DEADBAND ANTI-RUMORE anche sul SOLARE ("Ah prodotti")
    // Stesso criterio e stessa costante: istogramma FV, totale globale
    // e running total derivano dalla STESSA variabile filtrata →
    // somma barre = somma ore = totale visualizzato, per costruzione.
    // ============================================================
    float minuteSolarRaw = solAhMinuteAccum;

    float minuteSolar = 0.0f;
    if (minuteSolarRaw >= AH_MINUTE_DEADBAND_AH)
        minuteSolar = minuteSolarRaw;

    solAhHourlyToday[currentHour] += minuteSolar;
    solAhTodayTotal_global        += minuteSolar;
    solarAhToday                  += minuteSolar;   // OLED + JSON "solAhToday"/"solarAhToday"

    // ✅ reset accumulatori minuto
    ahMinuteDischargeAccum = 0.0f;
    ahMinuteChargeAccum    = 0.0f;
    solAhMinuteAccum       = 0.0f;
    // [DIAG] misura tempo saveAhToSD()
    uint32_t tAhDay = micros();
    saveAhToSD();  // ✅ SALVATAGGIO 1 VOLTA AL MINUTO
    uint32_t dtAhDay = micros() - tAhDay;
    if (dtAhDay > saveAhToSDMaxUs) saveAhToSDMaxUs = dtAhDay;
}



  
  time_t thisMinute = nowT / 60;

  // ✅ nuovo minuto reale
  if (thisMinute != lastSampleMinute) {

    // ===============================
    // ✅ GESTIONE SALTO TEMPORALE
    // ===============================
    if (lastSampleMinute != 0) {
        int delta = thisMinute - lastSampleMinute;

        if (delta > 1 && delta < 1440) {

            Serial.printf("[GRAPH] gap rilevato: %d minuti\n", delta);

            for (int i = 1; i < delta; i++) {

                int idx = sampleIndex;

                epochMinutes[idx] = 0;
                voltageSamples[idx] = 0;
                currentSamples[idx] = 0;
                solarCurrentSamples[idx] = 0;
                strcpy(timeLabels[idx], "--:--");

                sampleIndex = (sampleIndex + 1) % numSamples;
            }

        }
    }

    // ✅ aggiorna minuto corrente
    lastSampleMinute = thisMinute;

    // ===============================
    // ✅ CALCOLO MEDIE
    // ===============================
    float avgV = (accumCount > 0) ? (voltageSum / accumCount) : latestVoltage;
    float avgI = (accumCount > 0) ? (currentSum / accumCount) : currentFiltered;
    float avgSolar = (accumCount > 0) ? (solarCurrentSum / accumCount) : solarAmps;

    // 🔥 Soglia anti-rumore anche sulla media
    if (fabs(avgSolar) < 0.005f)
        avgSolar = 0.0f;

    // 🔥 Soglia anti-rumore anche sulla media corrente batteria (solo grafico storico)
    if (fabs(avgI) < 0.01f)
        avgI = 0.0f;

    // ===============================
    // ✅ SCRITTURA NEL BUFFER
    // ===============================
    voltageSamples[sampleIndex] = avgV;
    currentSamples[sampleIndex] = avgI;
    solarCurrentSamples[sampleIndex] = avgSolar;

    // label tempo locale
    time_t tLocal = utcToLocal(nowT);
    tmElements_t tm;
    breakTime(tLocal, tm);
    snprintf(timeLabels[sampleIndex], 6, "%02d:%02d", tm.Hour, tm.Minute);

    // epoch in minuti UTC
    epochMinutes[sampleIndex] = nowT / 60;

    // avanzamento buffer
    sampleIndex = (sampleIndex + 1) % numSamples;

    // reset accumulatori
    voltageSum = 0;
    currentSum = 0;
    solarCurrentSum = 0;
    accumCount = 0;

    // ===============================
    // ✅ SALVATAGGIO SD (come già hai)
    // ===============================
    static unsigned long lastGraphSave = 0;

    Serial.println("[GRAPH] trigger salvataggio");

    if (millis() - lastGraphSave > 300000UL) {
        // [DIAG] misura tempo saveGraphToSD()
        uint32_t tGraph = micros();
        saveGraphToSD();
        uint32_t dtGraph = micros() - tGraph;
        if (dtGraph > saveGraphToSDMaxUs) saveGraphToSDMaxUs = dtGraph;
        lastGraphSave = millis();
    }
}
}

        // ============================================================
        // 6) DISPLAY RENDER
        // ============================================================
        static unsigned long lastDisplayUpdate = 0;
        if (displayDirty) {
        displayNeedUpdate = true;
        displayDirty = false;
        if (DisplayTaskHandle) xTaskNotifyGive(DisplayTaskHandle);
    }


        // ✅ PATCH: REFRESH PERIODICO DEL DISPLAY (ogni 3 secondi)
        static unsigned long lastDisplayForce = 0;
        if (millis() - lastDisplayForce >= 3000) {   // ogni 3 s
            displayDirty = true;
            lastDisplayForce = millis();
            if (DisplayTaskHandle) xTaskNotifyGive(DisplayTaskHandle);
        }

// ============================================================
// I2C RECOVERY PERIODICO (FASE 7)
// ============================================================
{
    static unsigned long lastI2Ccheck = 0;
    unsigned long nowCheck = millis();

    // Ogni 2000 ms (2 secondi)
    if (nowCheck - lastI2Ccheck >= 2000) {
        lastI2Ccheck = nowCheck;

        
    bool sclLow = (digitalRead(I2C_SCL) == LOW);
    bool sdaLow = (digitalRead(I2C_SDA) == LOW);

    // ✅ evita recovery aggressivo
    static uint8_t i2cErrCount = 0;

    if (sclLow || sdaLow) {
        i2cErrCount++;

        if (i2cErrCount >= 3) {
            Serial.println("[I2C] BUS BLOCCATO → RECOVERY");
            i2cRecoverBus();
            i2cErrCount = 0;
        }
    } else {
        i2cErrCount = 0;
    }

    }
}



        // WAIT
        esp_task_wdt_reset();
        uint32_t loopElapsedUs = micros() - loopStartUs;
        if (loopElapsedUs > sensorLoopMaxUs) sensorLoopMaxUs = loopElapsedUs;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// =======================================================
// =============== DISPLAY TASK SEPARATO =================
// =======================================================
void DisplayTask(void *param) {
    esp_task_wdt_add(NULL);

    for (;;) {
        uint32_t displayStartUs = micros();
        hbDisplayTask++;
        lastDisplayTaskMs = millis();

        // attesa evento
        esp_task_wdt_reset(); 
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) {
          esp_task_wdt_reset();  // ✅ evita WDT mentre aspetta notifiche
        }
        esp_task_wdt_reset();   // ✅ EVITA TASK_WDT SUBITO

  
        if (displayNeedUpdate) {
            
            // LOCK SOFT: se la livella è attiva, limita il refresh del display
            static unsigned long lastDraw = 0;

            bool levelActiveNow = (millis() - lastLevelRequest < 1000);

            // Se livella attiva → massimo 5 Hz
            unsigned long refreshPeriod = levelActiveNow ? 200 : 50;

            if (millis() - lastDraw < refreshPeriod) {
              vTaskDelay(pdMS_TO_TICKS(2));
              continue;
            }
            lastDraw = millis();

            displayNeedUpdate = false;
            drawOLED();
            display.sendBuffer();

        }        

        uint32_t displayElapsedUs = micros() - displayStartUs;
        if (displayElapsedUs > displayTaskMaxUs) displayTaskMaxUs = displayElapsedUs;

        vTaskDelay(1);
    }
}


// ====================== LIVELLO: TASK DEDICATO ========================= //

void LevelTask(void *param) {
    esp_task_wdt_add(NULL);

    for (;;) {
        uint32_t levelStartUs = micros();
        hbLevelTask++;
        lastLevelTaskMs = millis();

        // ✅ WATCHDOG SEMPRE RESET
        esp_task_wdt_reset();

        unsigned long now = millis();
        bool lvlActive = (now - lastLevelRequest < 1000);

        if (lvlActive) {

            // ✅ RIATTIVA LIS SE ERA IN SLEEP
            if (lisInSleep) {
                if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    lis.setDataRate(LIS3DH_DATARATE_50_HZ);
                    lisInSleep = false;
                    xSemaphoreGive(i2cMutex);
                }
            }

            // ✅ AGGIORNA LIVELLA
            // [DIAG] misura tempo updateLevel()
            uint32_t tLevel = micros();
            updateLevel();
            uint32_t dtLevel = micros() - tLevel;
            if (dtLevel > updateLevelMaxUs) updateLevelMaxUs = dtLevel;

            // ✅ SE NON PRONTO → NON BLOCCARE MAI
            if (!levelReady) {
                vTaskDelay(pdMS_TO_TICKS(10));
                uint32_t levelElapsedUs = micros() - levelStartUs;
                if (levelElapsedUs > levelTaskMaxUs) levelTaskMaxUs = levelElapsedUs;
                continue;
            }

            // ✅ RIDOTTO STRESS CPU/I2C
            vTaskDelay(pdMS_TO_TICKS(40));

        } else {

            // ✅ METTI LIS IN SLEEP
            if (!lisInSleep) {
                if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    lis.setDataRate(LIS3DH_DATARATE_POWERDOWN);
                    lisInSleep = true;
                    xSemaphoreGive(i2cMutex);
                }
            }

            vTaskDelay(pdMS_TO_TICKS(100));
        }

        uint32_t levelElapsedUs = micros() - levelStartUs;
        if (levelElapsedUs > levelTaskMaxUs) levelTaskMaxUs = levelElapsedUs;
    }
}


// ============================
// TASK PREFERENCES (NVS writer)
// ============================
void PrefsTask(void *param) {
     
    Preferences prefsAh;
    Preferences prefsSolar;
    Preferences prefsLevel;

    prefsAh.begin("energy", false);
    prefsSolar.begin("solar", false);
    prefsLevel.begin("level", false);
    
    const uint32_t SAVE_DEBOUNCE_MS = 30000;   // min tempo tra scritture reali
    const uint32_t QUEUE_RESET_TIMEOUT_MS = 5000; // se nessuna ricezione per 5s → reset queue

    uint32_t lastSaveTs = 0;
    uint32_t lastQueueReceiveTs = millis();

    // Valori da salvare (accumulati)
    float pendingAhNow = NAN;
    float pendingAhMax = NAN;
    float pendingEff   = NAN;
    float pendingSolarOffset = NAN;
    float pendingR0 = NAN;
    float pendingP0 = NAN;

    // Contatori di sicurezza
    uint8_t stormCounter = 0;     // anti-tempesta
    uint32_t lastStormTs = 0;

    for (;;) {

        PrefsMessage msg;
        bool gotMsg = xQueueReceive(prefsQueue, &msg, pdMS_TO_TICKS(200));

        if (gotMsg) {
            lastQueueReceiveTs = millis();  // ✅ queue attiva → ok

            // 🔥 ANTI-TEMPESTA (se ricevi >20 messaggi in 1 secondo → scarta doppioni)
            uint32_t now = millis();
            if (now - lastStormTs < 1000) {
                stormCounter++;
                if (stormCounter > 20) {
                    // troppi messaggi → scarta duplicati
                    switch (msg.cmd) {
                        case CMD_SAVE_AH_NOW: pendingAhNow = msg.fvalue; break;
                        case CMD_SAVE_AH_MAX: pendingAhMax = msg.fvalue; break;
                        case CMD_SAVE_EFF:    pendingEff = msg.fvalue; break;
                        case CMD_SAVE_SOLAR_OFFSET: pendingSolarOffset = msg.fvalue; break;
                        case CMD_SAVE_LEVEL_R0: pendingR0 = msg.fvalue; break;
                        case CMD_SAVE_LEVEL_P0: pendingP0 = msg.fvalue; break;
                    }
                    continue; // non processare altro
                }
            } else {
                stormCounter = 0;
                lastStormTs = now;
            }

            // ✅ Accumulo intelligente (mantiene solo l’ultimo valore)
            switch (msg.cmd) {
                case CMD_SAVE_AH_NOW:       pendingAhNow = msg.fvalue; break;
                case CMD_SAVE_AH_MAX:       pendingAhMax = msg.fvalue; break;
                case CMD_SAVE_EFF:          pendingEff = msg.fvalue; break;
                case CMD_SAVE_SOLAR_OFFSET: pendingSolarOffset = msg.fvalue; break;
                case CMD_SAVE_LEVEL_R0:     pendingR0 = msg.fvalue; break;
                case CMD_SAVE_LEVEL_P0:     pendingP0 = msg.fvalue; break;
            }
        }

        
        // ✅ SALVATAGGIO RATE-LIMITING
        uint32_t now = millis();
        if (now - lastSaveTs >= SAVE_DEBOUNCE_MS) {

            // 🔒 Serializza con eventuali scritture immediate (handleSetAh/handleSetAhMax)
            // sullo stesso namespace NVS usando prefsMutex.
            if (xSemaphoreTake(prefsMutex, pdMS_TO_TICKS(200)) == pdTRUE) {

                // ---- AH NOW ----
                if (!isnan(pendingAhNow)) {
                    prefsAh.putFloat("ah_now", pendingAhNow);
                    pendingAhNow = NAN;
                }

                // ---- AH MAX ----
                if (!isnan(pendingAhMax)) {
                    prefsAh.putFloat("ah_max", pendingAhMax);
                    pendingAhMax = NAN;
                }

                // ---- Efficienza ----
                if (!isnan(pendingEff)) {
                    prefsAh.putFloat("eff", pendingEff);
                    pendingEff = NAN;
                }

                // ---- Solar offset ----
                if (!isnan(pendingSolarOffset)) {
                    prefsSolar.putFloat("offV", pendingSolarOffset);
                    pendingSolarOffset = NAN;
                }

                // ---- Livella R0 ----
                if (!isnan(pendingR0)) {
                    prefsLevel.putFloat("r0", pendingR0);
                    pendingR0 = NAN;
                }

                // ---- Livella P0 ----
                if (!isnan(pendingP0)) {
                    prefsLevel.putFloat("p0", pendingP0);
                    pendingP0 = NAN;
                }

                xSemaphoreGive(prefsMutex);
                lastSaveTs = now;
            }
        }

        // ✅ MICRO-SLEEP per evitare starvation + watchdog
        vTaskDelay(pdMS_TO_TICKS(3));
    }
}

// --- Task: ripristino storico posticipato ---
void LateRestoreTask(void *param) {
  Serial.println(">>> LateRestoreTask avviato <<<");

  // ✅ aspetta sistema stabile (heap + WiFi + task)
  vTaskDelay(pdMS_TO_TICKS(8000));

  if (sdOk) {
    Serial.println("[GRAPH] Late restore...");
    restoreGraphFromSD();

    // ✅ inizializza lastSampleMinute dal buffer
    if (sampleIndex > 0) {
        int lastIdx = (sampleIndex - 1 + numSamples) % numSamples;

        if (epochMinutes[lastIdx] != 0) {
            lastSampleMinute = epochMinutes[lastIdx];

            Serial.printf("[GRAPH] lastSampleMinute ripristinato: %lu\n", lastSampleMinute);
        }
    }

  }

  vTaskDelete(NULL);
}


// --- Task: watchdog SD ogni 5s ---
void SDWatchdogTask(void *param) {
  for (;;) {
    // Intervieni solo se la SD risulta non pronta
    if (!sdOk) { initSD(); }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

void saveHourlyAhToSD() {

  if (!initSD()) return;

  if (!takeMutexWarn(sdMutex, 500, "sd"))
    return;

  File f = SD.open("/ah_hours.bin", FILE_WRITE);

  if (!f) {
    xSemaphoreGive(sdMutex);
    return;
  }

  f.seek(0);
  
  // ✅ salva header con data
  AhDayHeader hdr;
  hdr.year  = currentYear;
  hdr.month = currentMonth;
  hdr.day   = currentDay;
  f.write((uint8_t*)&hdr, sizeof(hdr));
  
  f.write((uint8_t*)ahHourlyToday, sizeof(ahHourlyToday));

  f.flush();
  f.close();

  xSemaphoreGive(sdMutex);

  Serial.println("[AH] salvato su SD");
}

void saveAhToSD() {

  if (!initSD()) return;

  if (!takeMutexWarn(sdMutex, 500, "sd"))
    return;

  SD.remove("/ah_day.bin");

  File f = SD.open("/ah_day.bin", FILE_WRITE);
  if (!f) {
    xSemaphoreGive(sdMutex);
    return;
  }

  f.seek(0);
  
  // ✅ salva header con data
  AhDayHeader hdr;
  hdr.year  = currentYear;
  hdr.month = currentMonth;
  hdr.day   = currentDay;
  f.write((uint8_t*)&hdr, sizeof(hdr));
  
  // ✅ salva array orari
  f.write((uint8_t*)ahHourlyToday, sizeof(ahHourlyToday));
  f.write((uint8_t*)ahChargeHourlyToday, sizeof(ahChargeHourlyToday));
  f.write((uint8_t*)solAhHourlyToday, sizeof(solAhHourlyToday));
  f.write((uint8_t*)solAhHourlyYesterday, sizeof(solAhHourlyYesterday));
  
  // ✅ salva totali
  f.write((uint8_t*)&ahTodayTotal_global, sizeof(ahTodayTotal_global));
  f.write((uint8_t*)&ahYesterdayTotal_global, sizeof(ahYesterdayTotal_global));
  f.write((uint8_t*)&ahChargeTodayTotal_global, sizeof(ahChargeTodayTotal_global));
  f.write((uint8_t*)&ahChargeYesterdayTotal_global, sizeof(ahChargeYesterdayTotal_global));
  f.write((uint8_t*)&solAhTodayTotal_global, sizeof(solAhTodayTotal_global));
  f.write((uint8_t*)&solAhYesterdayTotal_global, sizeof(solAhYesterdayTotal_global));

  f.flush();
  f.close();

  xSemaphoreGive(sdMutex);

  Serial.println("[AH] salvato minuto su SD");
}


void restoreHourlyAhFromSD() {

  if (!initSD()) return;

  if (!takeMutexWarn(sdMutex, 500, "sd"))
    return;

  if (!SD.exists("/ah_hours.bin")) {
    xSemaphoreGive(sdMutex);
    return;
  }

  File f = SD.open("/ah_hours.bin", FILE_READ);

  if (!f) {
    xSemaphoreGive(sdMutex);
    return;
  }

  // ✅ Leggi header con data (primi 4 byte: year, month, day)
  AhDayHeader hdr;
  if (f.read((uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr)) {
    // Verifica se la data del file corrisponde a quella corrente
    if (hdr.year == currentYear && hdr.month == currentMonth && hdr.day == currentDay) {
      // Data corrisponde: ripristina gli array orari
      f.read((uint8_t*)ahHourlyToday, sizeof(ahHourlyToday));
      Serial.println("[AH] ripristinato da SD (stesso giorno)");
    } else {
      // Data non corrisponde: scarta i dati e inizializza a zero
      Serial.printf("[AH] Data file non corrisponde (file: %04d-%02d-%02d, attuale: %04d-%02d-%02d) - inizializzo array a zero\n",
        hdr.year, hdr.month, hdr.day,
        currentYear, currentMonth, currentDay);
      
      // ✅ Inizializza esplicitamente a zero gli array
      for (int i = 0; i < 24; i++) {
        ahHourlyToday[i] = 0.0f;
      }
    }
  } else {
    // File senza header o corrotto: inizializza a zero
    for (int i = 0; i < 24; i++) {
      ahHourlyToday[i] = 0.0f;
    }
  }

  f.close();
  xSemaphoreGive(sdMutex);
}

void restoreAhDayFromSD() {

  if (!initSD()) return;

  if (!takeMutexWarn(sdMutex, 500, "sd"))
    return;

  if (!SD.exists("/ah_day.bin")) {
    xSemaphoreGive(sdMutex);
    return;
  }

  File f = SD.open("/ah_day.bin", FILE_READ);
  if (!f) {
    xSemaphoreGive(sdMutex);
    return;
  }

  // ✅ Leggi header con data
  AhDayHeader hdr;
  if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr)) {
    f.close();
    xSemaphoreGive(sdMutex);
    return;
  }

  // ✅ Verifica data: se non corrisponde, scarta i dati e inizializza a zero
  if (hdr.year != currentYear || hdr.month != currentMonth || hdr.day != currentDay) {
    Serial.printf("[AH] Data non corrisponde (file: %04d-%02d-%02d, attuale: %04d-%02d-%02d) - inizializzo array a zero\n",
      hdr.year, hdr.month, hdr.day,
      currentYear, currentMonth, currentDay);
    
    // ✅ Inizializza esplicitamente a zero gli array
    for (int i = 0; i < 24; i++) {
      ahHourlyToday[i] = 0.0f;
      ahChargeHourlyToday[i] = 0.0f;
      solAhHourlyToday[i] = 0.0f;
      solAhHourlyYesterday[i] = 0.0f;
    }
    ahTodayTotal_global = 0.0f;
    ahYesterdayTotal_global = 0.0f;
    ahChargeTodayTotal_global = 0.0f;
    ahChargeYesterdayTotal_global = 0.0f;
    solAhTodayTotal_global = 0.0f;
    solAhYesterdayTotal_global = 0.0f;
    
    f.close();
    xSemaphoreGive(sdMutex);
    return;
  }

  // ✅ Ripristina array orari
  f.read((uint8_t*)ahHourlyToday, sizeof(ahHourlyToday));
  f.read((uint8_t*)ahChargeHourlyToday, sizeof(ahChargeHourlyToday));
  f.read((uint8_t*)solAhHourlyToday, sizeof(solAhHourlyToday));
  f.read((uint8_t*)solAhHourlyYesterday, sizeof(solAhHourlyYesterday));
  
  // ✅ Ripristina totali Ah
  f.read((uint8_t*)&ahTodayTotal_global, sizeof(ahTodayTotal_global));
  f.read((uint8_t*)&ahYesterdayTotal_global, sizeof(ahYesterdayTotal_global));
  f.read((uint8_t*)&ahChargeTodayTotal_global, sizeof(ahChargeTodayTotal_global));
  f.read((uint8_t*)&ahChargeYesterdayTotal_global, sizeof(ahChargeYesterdayTotal_global));
  f.read((uint8_t*)&solAhTodayTotal_global, sizeof(solAhTodayTotal_global));
  f.read((uint8_t*)&solAhYesterdayTotal_global, sizeof(solAhYesterdayTotal_global));

  f.close();
  xSemaphoreGive(sdMutex);

  Serial.println("[AH] ripristinato da SD");
}

bool saveAhStateToSD()
{
    if (!sdOk) return false;

    if (!takeMutexWarn(sdMutex, 500, "sd"))
        return false;

    AhStateFile s;
    s.magic = AH_STATE_MAGIC;
    s.epoch = nowEpoch();
    s.ahNow = batteryAh_now;
//    s.battVolt = batteryV;

    const char *tmp = "/ah_state.tmp";

    File f = SD.open(tmp, FILE_WRITE);
    if (!f) {
        xSemaphoreGive(sdMutex);
        return false;
    }

    size_t n = f.write((uint8_t *)&s, sizeof(s));
    f.flush();
    f.close();

    if (n != sizeof(s)) {
        SD.remove(tmp);
        xSemaphoreGive(sdMutex);
        return false;
    }

    SD.remove(AH_STATE_FILE);

    if (!SD.rename(tmp, AH_STATE_FILE)) {
        SD.remove(tmp);
        xSemaphoreGive(sdMutex);
        return false;
    }

    xSemaphoreGive(sdMutex);
    return true;
}


bool restoreAhStateFromSD()
{
    if (!sdOk) return false;

    if (!takeMutexWarn(sdMutex, 500, "sd"))
        return false;

    if (!SD.exists(AH_STATE_FILE)) {
        xSemaphoreGive(sdMutex);
        return false;
    }

    File f = SD.open(AH_STATE_FILE);

    if (!f) {
        xSemaphoreGive(sdMutex);
        return false;
    }

    AhStateFile s;

    if (f.read((uint8_t *)&s, sizeof(s)) != sizeof(s)) {
        f.close();
        xSemaphoreGive(sdMutex);
        return false;
    }

    f.close();

    if (s.magic != AH_STATE_MAGIC) {
        xSemaphoreGive(sdMutex);
        return false;
    }

    batteryAh_now = s.ahNow;

    // 🔒 CLAMP ASSOLUTO vs batteryAh_max (già ripristinato da NVS in restoreAhFromPrefs)
    if (batteryAh_now > batteryAh_max) batteryAh_now = batteryAh_max;
    if (batteryAh_now < 0)             batteryAh_now = 0;

    Serial.printf(
        "AH restored from SD: %.2f Ah  saved %lu sec ago\n",
        batteryAh_now,
        (unsigned long)(nowEpoch() - s.epoch));

    xSemaphoreGive(sdMutex);
    return true;
}


// =======================================================
// ===============  GESTIONE PULSANTE  ==================
// =======================================================
void handleButtonInput() {
    // --- Pulsante: debounce + 1 click per pressione completa ---
    static int lastStableState = HIGH; // INPUT_PULLUP → riposo HIGH
    static int lastRawState = HIGH;    // lettura grezza
    static unsigned long lastChangeMs = 0; // per debounce
    static bool pressHandled = false;  // 1 click per pressione

    int raw = digitalRead(BUTTON_PIN);

    // se cambia il livello, reset del timer di debounce
    if (raw != lastRawState) {
        lastChangeMs = millis();
        lastRawState = raw;
    }

    // dopo 120ms consideriamo lo stato "stabile"
    if (millis() - lastChangeMs > 120) {
        if (raw != lastStableState) {
            lastStableState = raw;

            // transizione a "premuto" (HIGH -> LOW): genera UN SOLO click
            if (lastStableState == LOW && !pressHandled) {

                // interazione: luce alta e reset timer
                desiredContrast = BRIGHT_HIGH;
                isDimmed = false;
                lastInteraction = millis();
                nextDimAt = lastInteraction + 30000UL;

                // pagina successiva
                currentPage = (currentPage + 1) % 6;

                // ✅ PATCH: NON riattivare MAI l'integrazione Ah
                // freezeAhIntegration = false;   <-- rimosso

                // chiedi redraw
                displayDirty = true;
                pressHandled = true; // finché non rilasci, non accettiamo altri click
            }

            // transizione a "rilasciato" (LOW -> HIGH): riarmi
            if (lastStableState == HIGH) {
                pressHandled = false;
            }
        }
    }
}

  //============================
  // LOOP PRINCIPALE
  //===========================

// =======================================================
// ===============  WEB SERVER + DNS SERVICE  ==============
// =======================================================
void serviceWebRequests() {
    // Web server + DNS captive portal
    server.handleClient();
    static unsigned long lastDns = 0;
    if (millis() - lastDns > 50) {   // max 20 Hz
      dnsServer.processNextRequest();
      lastDns = millis();
    }

    lastWebActivity = millis();   // ✅ segna attività web
}

void monitorTaskStacks() {
  static unsigned long lastStackLog = 0;
  if (millis() - lastStackLog > 30000) { // ogni 30 secondi
    lastStackLog = millis();

    uint32_t loopWM    = uxTaskGetStackHighWaterMark(NULL);
    uint32_t sensorWM  = SensorTask ? uxTaskGetStackHighWaterMark(SensorTask) : 0;
    uint32_t displayWM = DisplayTaskHandle ? uxTaskGetStackHighWaterMark(DisplayTaskHandle) : 0;

    Serial.printf(
      "[STACK] loop=%u sensor=%u display=%u\n",
      loopWM,
      sensorWM,
      displayWM
    );
  }
}

// ============================================================
// FLIGHT RECORDER DIAGNOSTICO PERSISTENTE
// Salva periodicamente su SD l'ultimo stato del sistema,
// sovrascrivendo sempre /last_diag.txt (diagnosi post-INT_WDT).
// Usa il ciclo existente loop() - nessuna task dedicata.
// ============================================================
void saveDiagSnapshot() {
  static unsigned long lastDiagSave = 0;

  // Aggiorna al massimo ogni 60 secondi
  if (millis() - lastDiagSave < 60000) return;
  lastDiagSave = millis();

  // Se la SD non è disponibile → skip e continuo
  if (!initSD()) {
    Serial.println("[DIAG] save skipped");
    return;
  }

  if (sdMutex == NULL) sdMutex = xSemaphoreCreateMutex();
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    Serial.println("[DIAG] save skipped");
    return;
  }

  // Sovrascrive sempre il file (last_diag.txt)
  File f = SD.open("/last_diag.txt", FILE_WRITE);
  if (!f) {
    xSemaphoreGive(sdMutex);
    Serial.println("[DIAG] save skipped");
    return;
  }

  // ---- Header: tempo + reset reason (alprincipio del file) ----
  char rtcBuf[32] = "NO_TIME";
  unsigned long epochVal = 0;

  if (timeValid) {
    time_t nowUtc = nowEpoch();                    // epoch corrente (UTC)
    epochVal = (unsigned long)nowUtc;
    tmElements_t tm;
    breakTime(nowUtc + (tzOffsetHours * 3600), tm); // ora locale (CET/CEST)
    snprintf(rtcBuf, sizeof(rtcBuf), "%04d-%02d-%02d %02d:%02d:%02d",
             tm.Year + 1970, tm.Month, tm.Day, tm.Hour, tm.Minute, tm.Second);
  }

  esp_reset_reason_t rr = esp_reset_reason();
  const char* rrStr =
    (rr==ESP_RST_POWERON)?"POWERON":
    (rr==ESP_RST_EXT)?"EXT_RESET (USB/BTN)":
    (rr==ESP_RST_SW)?"SW_RESET":
    (rr==ESP_RST_PANIC)?"PANIC":
    (rr==ESP_RST_INT_WDT)?"INT_WDT":
    (rr==ESP_RST_TASK_WDT)?"TASK_WDT":
    (rr==ESP_RST_WDT)?"WDT":
    (rr==ESP_RST_DEEPSLEEP)?"DEEPSLEEP_WAKE":
    (rr==ESP_RST_BROWNOUT)?"BROWNOUT":
    (rr==ESP_RST_SDIO)?"SDIO":"UNKNOWN";

  f.printf("rtc=%s\n",          rtcBuf);
  f.printf("epoch=%lu\n",        epochVal);
  f.printf("millis=%lu\n",       (unsigned long)millis());
  f.printf("resetReason=%s\n",   rrStr);
  f.printf("heap=%lu\n",         (unsigned long)ESP.getFreeHeap());

  // ---- Stack minima residua dei task (water mark) ----
  f.printf("stackLoop=%lu\n",       (unsigned long)uxTaskGetStackHighWaterMark(NULL));
  f.printf("stackSensor=%lu\n",     (unsigned long)(SensorTask ? uxTaskGetStackHighWaterMark(SensorTask) : 0));
  f.printf("stackDisplay=%lu\n",    (unsigned long)(DisplayTaskHandle ? uxTaskGetStackHighWaterMark(DisplayTaskHandle) : 0));
  f.printf("stackLevel=%lu\n",      (unsigned long)(LevelTaskHandle ? uxTaskGetStackHighWaterMark(LevelTaskHandle) : 0));
  f.printf("stackPrefs=%lu\n",      (unsigned long)(PrefsTaskHandle ? uxTaskGetStackHighWaterMark(PrefsTaskHandle) : 0));
  f.printf("stackSDWatchdog=%lu\n", (unsigned long)(SDWatchdogTaskHandle ? uxTaskGetStackHighWaterMark(SDWatchdogTaskHandle) : 0));

  f.printf("hbLoop=%lu\n",              (unsigned long)hbLoop);
  f.printf("hbSensorLoop=%lu\n",        (unsigned long)hbSensorLoop);
  f.printf("hbLevelTask=%lu\n",         (unsigned long)hbLevelTask);
  f.printf("hbDisplayTask=%lu\n",       (unsigned long)hbDisplayTask);

  f.printf("lastLoopMs=%lu\n",          (unsigned long)lastLoopMs);
  f.printf("lastSensorLoopMs=%lu\n",    (unsigned long)lastSensorLoopMs);
  f.printf("lastLevelTaskMs=%lu\n",     (unsigned long)lastLevelTaskMs);
  f.printf("lastDisplayTaskMs=%lu\n",   (unsigned long)lastDisplayTaskMs);

  f.printf("loopMaxUs=%lu\n",           (unsigned long)loopMaxUs);
  f.printf("sensorLoopMaxUs=%lu\n",     (unsigned long)sensorLoopMaxUs);
  f.printf("levelTaskMaxUs=%lu\n",      (unsigned long)levelTaskMaxUs);
  f.printf("displayTaskMaxUs=%lu\n",    (unsigned long)displayTaskMaxUs);

  f.printf("updateCurrentAndVoltageMaxUs=%lu\n", (unsigned long)updateCurrentAndVoltageMaxUs);
  f.printf("readSolarCurrentMaxUs=%lu\n",        (unsigned long)readSolarCurrentMaxUs);
  f.printf("updateDHTMaxUs=%lu\n",               (unsigned long)updateDHTMaxUs);
  f.printf("updateGas1MaxUs=%lu\n",              (unsigned long)updateGas1MaxUs);
  f.printf("updateGas2MaxUs=%lu\n",              (unsigned long)updateGas2MaxUs);
  f.printf("updateLevelMaxUs=%lu\n",             (unsigned long)updateLevelMaxUs);
  f.printf("saveAhStateToSDMaxUs=%lu\n",         (unsigned long)saveAhStateToSDMaxUs);
  f.printf("saveAhToSDMaxUs=%lu\n",              (unsigned long)saveAhToSDMaxUs);
  f.printf("saveGraphToSDMaxUs=%lu\n",           (unsigned long)saveGraphToSDMaxUs);

  f.printf("dhtReads=%lu\n",            (unsigned long)dhtReads);
  f.printf("dhtReadFailures=%lu\n",     (unsigned long)dhtReadFailures);
  f.printf("dhtMaxReadUs=%lu\n",        (unsigned long)dhtMaxReadUs);

  f.printf("ads_present=%u\n",          adsPresent ? 1 : 0);
  f.printf("lis_present=%u\n",          lisPresent ? 1 : 0);

  f.close();
  xSemaphoreGive(sdMutex);

  Serial.println("[DIAG] snapshot saved");
}
void loop() {

    uint32_t loopStartUs = micros();
    hbLoop++;
    lastLoopMs = millis();

monitorTaskStacks();
    saveDiagSnapshot();
    
    
    
    
    
    // --- Gestione pulsante (estratta in funzione) ---
    handleButtonInput();

    // --- Timeout inattività: gestito da funzione dedicata ---
    handleDisplayTimeout();

    // ------------------------------------------------------------------
    // HEARTBEAT DI SISTEMA (1 Hz) - leggerissimo, non blocca l’ESP32
    // ------------------------------------------------------------------
    {
        static unsigned long lastBeat = 0;
        unsigned long now = millis();
        if (now - lastBeat >= 1000) {
            const bool lvlActive = (now - lastLevelRequest < 1000);
            Serial.printf(
                "[HB] t=%lu  page=%d  dim=%d  excl=%d  lvlActive=%d  "
                "ADS=%d  LIS=%d  V=%.2f  I=%.2f  SolA=%.2f  "
                "Gas1=%.2f  Gas2=%.2f  Water=%.1f  Flow=%.2f  Heap=%u  "
                "HB: loop=%lu  sensor=%lu  level=%lu  display=%lu\n",
                now,
                (int)currentPage,
                isDimmed ? 1 : 0,
                levelExclusiveMode ? 1 : 0,
                lvlActive ? 1 : 0,
                adsPresent ? 1 : 0,
                lisPresent ? 1 : 0,
                vBattery,
                currentFiltered,
                solarAmps,
                gasResidual1,
                gasResidual2,
                remainingLiters,
                lastFlowLmin,
                (unsigned)ESP.getFreeHeap(),
                (unsigned long)hbLoop,
                (unsigned long)hbSensorLoop,
                (unsigned long)hbLevelTask,
                (unsigned long)hbDisplayTask
            );
            lastBeat = now;
        }
    }

    // Web server + DNS captive portal
    serviceWebRequests();

    uint32_t loopElapsedUs = micros() - loopStartUs;
    if (loopElapsedUs > loopMaxUs) loopMaxUs = loopElapsedUs;
}

bool initLIS3DH() {
  bool ok = false;
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    ok = lis.begin(LIS3DH_ADDR);
    if (ok) {
      lis.setRange(LIS3DH_RANGE_2_G);          // massima sensibilità
      lis.setDataRate(LIS3DH_DATARATE_50_HZ);  // 50 Hz: più campioni, meno jitter
    }
    xSemaphoreGive(i2cMutex);
  }
  return ok;
}

// Legge il LIS3DH, calcola roll/pitch, filtra e applica zero

void updateLevel() {

    if (!lisPresent) return;

    static unsigned long lastRead = 0;
    unsigned long now = millis();

    // ✅ Se l'utente NON è nella pagina livella → riduci frequenza a 15 Hz (66 ms)
    bool highRate = (now - lastLevelRequest < 1000);  // pagina livella aperta

    unsigned long period = highRate ? 40 : 120;  
    // 30 ms → ~33 Hz (prima erano 50 Hz)
    // 120 ms → ~8 Hz quando non serve frequenza alta

    if (now - lastRead < period) return;
    lastRead = now;

    sensors_event_t ev;

    // ✅ mutex velocissimo: 8 ms max → non blocca nulla
    
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        levelReady = false;
        return;
    }

    bool ok = lis.getEvent(&ev);
    xSemaphoreGive(i2cMutex);

    // ✅ gestisci errore correttamente
    if (!ok) {
        levelReady = false;
        return;
    }


    float ax = ev.acceleration.x / 9.80665f;
    float ay = ev.acceleration.y / 9.80665f;
    float az = ev.acceleration.z / 9.80665f;

    float roll  = atan2f(ay, az) * 57.2957795f - lvl_roll_zero;
    float pitch = atan2f(-ax, sqrtf(ay*ay + az*az)) * 57.2957795f - lvl_pitch_zero;

    // ✅ Filtri IIR identici ai tuoi (mantengono fluidità)
    lvl_roll  = (1.0f - alphaLevel) * lvl_roll  + alphaLevel * roll;
    lvl_pitch = (1.0f - alphaLevel) * lvl_pitch + alphaLevel * pitch;

    levelReady = true;
}



  // =======================================================
  // =====================  OLED DISPLAY  ==================
  // =======================================================

  // ICONE
  void iconBattery(int x, int y) {
    display.drawFrame(x, y, 18, 10);      // corpo
    display.drawBox(x + 18, y + 3, 2, 4); // polo +
  }

  // Batteria con riempimento in base al SOC (0..100)
  void iconBatteryLevel(int x, int y, int socPct) {
    if (socPct < 0) socPct = 0;
    if (socPct > 100) socPct = 100;

    display.drawFrame(x, y, 18, 10);      // contorno
    display.drawBox(x + 18, y + 3, 2, 4); // polo +

    int innerX = x + 1;
    int innerY = y + 1;
    int innerW = 16;
    int innerH = 8;

    int fillW = (innerW * socPct) / 100;
    if (fillW > 0) {
      display.drawBox(innerX, innerY, fillW, innerH);
    }
  }

  void iconWater(int x, int y) {
    display.drawCircle(x + 6, y + 6, 6);
    display.drawLine(x + 6, y, x + 6, y + 6);
  }

  void iconGas(int x, int y) {
    display.drawFrame(x, y + 2, 14, 16);
    display.drawBox(x + 4, y, 6, 4);
  }

  void iconTemp(int x, int y) {
    display.drawFrame(x + 4, y, 4, 10);
    display.drawCircle(x + 6, y + 12, 4);
  }

  void iconClock(int x, int y) {
    display.drawCircle(x + 8, y + 8, 8);
    display.drawLine(x + 8, y + 8, x + 8, y + 3);
    display.drawLine(x + 8, y + 8, x + 12, y + 8);
  }

  // Helpers allineamento destro
  void drawRightPrintf(int xRight, int y, const char* fmt, ...) {
    char buf[32];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    int w = display.getStrWidth(buf);
    display.setCursor(xRight - w, y);
    display.print(buf);
  }

   void drawOLED() {
    // FULL-BUFFER: frame unico (veloce, niente pagine tagliate)
    display.clearBuffer();
    display.setContrast(desiredContrast); 
    switch (currentPage) {

      // ===========================================
      // PAGINA 0 — HOME MINIMALE (LOW BRIGHTNESS)
      // ===========================================
      case 0: {
          const int xIcon   = 0;
          const int xLabel  = 24;
          const int xRight  = 126;
          int y  = 22;
          const int dy  = 22;
          const int gap = 15;

          const float soc_f = (batteryAh_max > 0.5 ? 100.0f * batteryAh_now / batteryAh_max : 0.0f);
          const int   soc_i = (int)(soc_f + 0.5f);

          // RIGA 1 — ORARIO
          iconClock(xIcon, y - 20);
          display.setFont(u8g2_font_logisoso18_tf);
          drawRightPrintf(xRight, y, "%02d:%02d", currentHour, currentMinute);


          // RIGA 2 — TENSIONE
          y += dy + gap;
          display.setFont(u8g2_font_logisoso26_tf);
          drawRightPrintf(xRight, y, "%.2f V", vBattery);


          // RIGA 3 — SOC
          y += dy - 1;
          iconBatteryLevel(xIcon, y - 12, soc_i);
          display.setFont(u8g2_font_8x13_tf);
          drawRightPrintf(xRight, y, "%d %%", soc_i);


          // RIGA 4 — TEMP INT
          y += dy + 4;
          iconTemp(xIcon, y);
          display.setFont(u8g2_font_8x13_tf);
          display.setCursor(xLabel, y);
          display.print("Int");
          display.setFont(u8g2_font_logisoso18_tf);
          if (dhtIntValid) drawRightPrintf(xRight, y, "%.1f C", tempInt);
          else             drawRightPrintf(xRight, y, "%s", "-- C");

          // RIGA 5 — TEMP EXT
          y += dy;
          display.setFont(u8g2_font_8x13_tf);
          display.setCursor(xLabel, y);
          display.print("Ext");
          display.setFont(u8g2_font_logisoso18_tf);
          if (dhtExtValid) drawRightPrintf(xRight, y, "%.1f C", tempExt);
          else             drawRightPrintf(xRight, y, "%s", "-- C");
          break;
        }

// ===========================================
// PAGINA 1 — BATTERIA 
// ===========================================
case 1: {

    const int screenW = 128;
    const int xValue  = 118;

    const int barX = 0;
    const int barW = 16;

    int y  = 20;   // punto di partenza verticale

    // Calcolo SOC
    const float soc_f = (batteryAh_max > 0.5 ? 100.0f * batteryAh_now / batteryAh_max : 0.0f);
    const int   soc_i = (int)(soc_f + 0.5f);

    // ---- TITOLO ----
    display.setFont(u8g2_font_fub14_tf);
    {
      int w = display.getStrWidth("BATTERIA");
      display.setCursor((screenW - w) / 2, y);
      display.print("BATTERIA");
    }

    // ---- BARRA VERTICALE SOC ----
    const int barTop    = 22;
    const int barBottom = 128;
    const int barH      = barBottom - barTop;

    int fillH = (barH * soc_i) / 100;
    int fillY = barBottom - fillH;

    display.drawFrame(barX, barTop, barW, barH);
    if (fillH > 0)
      display.drawBox(barX + 2, fillY, barW - 4, fillH);

    // Spostamento righe verso destra (valori)
    int xR = 118;

    // ---- RIGA 2: TENSIONE  ----
    y = 52;
    display.setFont(u8g2_font_logisoso22_tf);  // stessa dimensione pagina 0
    drawRightPrintf(xR, y, "%.2f V", vBattery);

    // ---- RIGA 3: CORRENTE ----
    y += 30;
    display.setFont(u8g2_font_logisoso22_tf);
    drawRightPrintf(xR, y, "%.2f A", currentFiltered);

    // ---- RIGA 4: Ah residui ----
    y += 24;

    display.setFont(u8g2_font_8x13_tf);
    display.setCursor(24, y);  // 24 px dall'inizio
    display.print("Res");
    
    display.setFont(u8g2_font_helvR14_tf);
    drawRightPrintf(xR, y, "%.1f Ah", batteryAh_now);

    // ---- RIGA 5: Percentuale SOC ----
    y += 18;
    display.setFont(u8g2_font_helvR14_tf);
    drawRightPrintf(xR, y, "%d %%", soc_i);

    break;
}

// ===========================================
// PAGINA 2 — SOLARE 
// ===========================================
case 2: {
    const int screenW = 128;
    const int xRight  = 126;

    // ---- TITOLO ----
    display.setFont(u8g2_font_fub14_tf);
    {
      const char* title = "SOLARE";
      int w = display.getStrWidth(title);
      display.setCursor((screenW - w) / 2, 16);
      display.print(title);
    }

    int y = 52;

    // ---- RIGA 1: Corrente solare (grande) ----
    display.setFont(u8g2_font_logisoso22_tf);
    drawRightPrintf(xRight, y, "%.2f A", solarAmps);

    // ---- RIGA 2: Tensione batteria con icona SOC ----
    y += 26;

    // calcolo SOC batteria come nelle altre pagine
    float soc_f = (batteryAh_max > 0.5f ? 100.0f * batteryAh_now / batteryAh_max : 0.0f);
    int   soc_i = (int)(soc_f + 0.5f);

    // Icona batteria variabile
    iconBatteryLevel(0, y - 12, soc_i);

    display.setFont(u8g2_font_helvR14_tf);
    drawRightPrintf(xRight, y, "%.2f V", vBattery);

    // ---- RIGA 3: Pannelli (testo a sinistra) + potenza a destra ----
    y += 20;

    display.setFont(u8g2_font_8x13_tf);
    display.setCursor(0, y);
    display.print("Pannelli");

    display.setFont(u8g2_font_helvR14_tf);
    drawRightPrintf(xRight, y, "%.0f W", solarWatts);

    // ---- RIGA 4: Ah giornalieri ----
    y += 28;

    display.setFont(u8g2_font_logisoso16_tf);
    drawRightPrintf(xRight, y, "%.3f Ah", solarAhToday);

    break;
}



// ===========================================
// PAGINA 3 — ACQUA 
// ===========================================
case 3: {
    const int screenW = 128;
    const int xValue  = 126;

    // Colonna 
    const int barX = 0;
    const int barW = 28;

    const int yTitle    = 16;
    const int barTop    = 22;
    const int barBottom = 128;
    const int barH      = barBottom - barTop;

    float pctWater = (tankCapacity > 0) ? (100.0f * remainingLiters / tankCapacity) : 0.0f;
    pctWater = constrain(pctWater, 0, 100);

    // --- Titolo ---
    display.setFont(u8g2_font_fub14_tf);
    {
      int w = display.getStrWidth("ACQUA");
      display.setCursor((screenW - w) / 2, yTitle);
      display.print("ACQUA");
    }

    // --- Barra verticale ---
    int fillH = (barH * (int)pctWater) / 100;
    int fillY = barBottom - fillH;

    display.drawFrame(barX, barTop, barW, barH);
    if (fillH > 0)
      display.drawBox(barX + 2, fillY, barW - 4, fillH);

    // --- Litri a destra ---
    display.setFont(u8g2_font_logisoso24_tf);
    int yValue = 86;
    drawRightPrintf(xValue, yValue, "%.1f L", remainingLiters);

    break;
}

      // ===========================================
      // PAGINA 4 — GAS
      // ===========================================
      case 4: {
          // Titolo
          display.setFont(u8g2_font_fub14_tf);
          int w = display.getStrWidth("GAS kg");
          display.setCursor((128 - w) / 2, 16);
          display.print("GAS kg");

          const int barTop    = 26;
          const int barBottom = 108;
          const int barH      = barBottom - barTop;

          const int barW  = 40;
          const int bar1X = 14;
          const int bar2X = 74;

          float pct1 = (GAS_MAX_1 > 0) ? (100.0f * gasResidual1 / GAS_MAX_1) : 0;
          float pct2 = (GAS_MAX_2 > 0) ? (100.0f * gasResidual2 / GAS_MAX_2) : 0;
          if (pct1 < 0) pct1 = 0; if (pct1 > 100) pct1 = 100;
          if (pct2 < 0) pct2 = 0; if (pct2 > 100) pct2 = 100;

          int fillH1 = (barH * (int)pct1) / 100;
          int fillY1 = barBottom - fillH1;

          display.drawFrame(bar1X, barTop, barW, barH);
          if (fillH1 > 0)
            display.drawBox(bar1X + 2, fillY1, barW - 4, fillH1);

          int fillH2 = (barH * (int)pct2) / 100;
          int fillY2 = barBottom - fillH2;

          display.drawFrame(bar2X, barTop, barW, barH);
          if (fillH2 > 0)
            display.drawBox(bar2X + 2, fillY2, barW - 4, fillH2);

          display.setFont(u8g2_font_logisoso16_tf);

          char buf1[10];
          snprintf(buf1, sizeof(buf1), "%.2f", gasResidual1);
          w = display.getStrWidth(buf1);
          display.setCursor(bar1X + (barW - w) / 2, barBottom + 20);
          display.print(buf1);

          char buf2[10];
          snprintf(buf2, sizeof(buf2), "%.2f", gasResidual2);
          w = display.getStrWidth(buf2);
          display.setCursor(bar2X + (barW - w) / 2, barBottom + 20);
          display.print(buf2);
          break;
        }

      // ===========================================
      // PAGINA 5 — TEMPERATURA
      // ===========================================
      case 5: {
          const int screenW = 128;
          const int xLabel  = 4;
          const int xRight  = 126;
          int y = 18;
          const int dyTemp = 32;
          const int dyHum  = 18;

          // Titolo
          display.setFont(u8g2_font_fub14_tf);
          int w = display.getStrWidth("TEMPERAT");
          display.setCursor((screenW - w) / 2, y);
          display.print("TEMPERAT");

          // Interno
          y += dyTemp + 4;
          display.setFont(u8g2_font_8x13_tf);
          display.setCursor(xLabel, y);
          display.print("Int");

          display.setFont(u8g2_font_logisoso20_tf);
          if (dhtIntValid) drawRightPrintf(xRight, y, "%.1f C", tempInt);
          else             drawRightPrintf(xRight, y, "%s", "-- C");

          y += dyHum;
          display.setFont(u8g2_font_8x13_tf);
          if (dhtIntValid) drawRightPrintf(xRight, y, "%.0f %%", humInt);
          else             drawRightPrintf(xRight, y, "%s", "-- %");

          // Esterno
          y += dyTemp;
          display.setFont(u8g2_font_8x13_tf);
          display.setCursor(xLabel, y);
          display.print("Ext");

          display.setFont(u8g2_font_logisoso20_tf);
          if (dhtExtValid) drawRightPrintf(xRight, y, "%.1f C", tempExt);
          else             drawRightPrintf(xRight, y, "%s", "-- C");

          y += dyHum;
          display.setFont(u8g2_font_8x13_tf);
          if (dhtExtValid) drawRightPrintf(xRight, y, "%.0f %%", humExt);
          else             drawRightPrintf(xRight, y, "%s", "-- %");
          break;
        }
    }

  }

