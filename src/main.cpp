/*
 * Analyseur O2 fixe (de paillasse, alimentation 220V AC) - ESP32-WROOM-32
 *
 * Capteur O2     : ADS1115 A0 (gain x16)         I2C @0x48
 * Affichage      : LCD 1602 I2C                  @0x27
 * Horloge        : DS3231                        I2C @0x68
 * Boutons        : 3 entrees, type configurable a la compilation :
 *                    BUTTON_MODE = 0  -> modules TTP223 (digital, HIGH actif)
 *                    BUTTON_MODE = 1  -> touch capacitif natif ESP32
 *                  Pins identiques dans les deux cas (G32/G33/G27).
 * Temperature    : DS18B20 sur G4 (OPTIONNEL)
 * LED RGB        : WS2812B (NeoPixel) sur G5
 *                  ATTENTION : data 3.3V vers LED 5V, voir WIRING.md
 * Lecteur RFID   : PN532 I2C @0x24, IRQ G18, RESET G19 (OPTIONNEL)
 *                  Lit le nom du plongeur en bloc 4 (Mifare Classic)
 * Imprimante     : TSC TH240 via UART2 (Serial2) - RX G16, TX G17
 *
 * Architecture : machine a etats, aucun delay(), tout sur millis().
 */

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_ADS1X15.h>
#include <RTClib.h>
#include <EEPROM.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_PN532.h>
#include <Adafruit_NeoPixel.h>

// ============================================================================
//  CONFIGURATION : choix du type de bouton (compile-time)
// ============================================================================
//   BUTTON_MODE = 0 : modules TTP223 (HIGH = touche)
//   BUTTON_MODE = 1 : touch capacitif natif ESP32 (touchRead < seuil = touche)
//
// Defini dans platformio.ini via -DBUTTON_MODE=N selon l'environnement choisi.
// Defaut : TTP223 si non specifie.
#ifndef BUTTON_MODE
  #define BUTTON_MODE 0
#endif

// ============================================================================
//  CONFIGURATION MATERIELLE (pins ESP32)
// ============================================================================
//
// Choix des pins boutons : T9/T8/T7 sont des broches "touch" capables, ET
// utilisables en GPIO standard. Donc identiques pour les deux modes.
//   T9 = GPIO 32 (LEFT)
//   T8 = GPIO 33 (CENTER)
//   T7 = GPIO 27 (RIGHT)
//
static const uint8_t PIN_BTN_LEFT     = 32;
static const uint8_t PIN_BTN_CENTER   = 33;
static const uint8_t PIN_BTN_RIGHT    = 27;
static const uint8_t PIN_TEMP         = 4;    // DS18B20 OneWire (optionnel)
static const uint8_t PIN_LED_RGB      = 5;    // WS2812B data
static const uint8_t PIN_PN532_IRQ    = 18;
static const uint8_t PIN_PN532_RESET  = 19;
static const uint8_t PIN_PRINTER_RX   = 16;   // RX UART2 (vers TX imprimante)
static const uint8_t PIN_PRINTER_TX   = 17;   // TX UART2 (vers RX imprimante)
static const uint8_t LCD_ADDR         = 0x27;
static const uint8_t ADS_O2_CHANNEL   = 0;

// ============================================================================
//  CONSTANTES LOGICIEL
// ============================================================================
static const uint8_t  STABILITY_SAMPLES   = 15;
static const float    STABILITY_THRESHOLD = 0.1f;
static const uint16_t LONG_PRESS_MS       = 3000;
static const uint8_t  DEBOUNCE_MS         = 50;
static const uint16_t SAMPLE_INTERVAL_MS  = 100;
static const uint16_t DISPLAY_INTERVAL_MS = 250;
static const uint16_t FEEDBACK_MS         = 1500;
static const uint16_t SPLASH_MS           = 2000;
static const uint16_t SPLASH_NOCAL_MS     = 5000;
static const uint16_t TEMP_INTERVAL_MS    = 2000;
static const uint16_t TEMP_CONV_MS        = 800;
static const float    PPO2_MIN            = 1.0f;
static const float    PPO2_MAX            = 1.6f;
static const float    PPO2_STEP           = 0.1f;
static const float    CALIB_REF_PERCENT   = 20.9f;
static const float    ADS_LSB_MV          = 0.0078125f;
static const float    TEMP_COEFF_PER_C    = 0.003f;
static const uint8_t  CELL_WARN_PERCENT   = 80;

// Touch capacitif (uniquement si BUTTON_MODE == 1)
//   touchRead() retourne ~80 au repos, descend vers ~25 quand touche
//   Ajuster en fonction de la taille du plot et de l'environnement.
static const uint16_t TOUCH_THRESHOLD     = 40;

// RFID
static const uint16_t RFID_POLL_MS        = 250;
static const uint32_t ARMED_TIMEOUT_MS    = 30000;
static const uint8_t  NAME_MAX_LEN        = 14;

// LED RGB
static const uint8_t  LED_BRIGHTNESS      = 80;
static const uint16_t LED_BLINK_MS        = 500;

// EEPROM layout
static const int EEPROM_SIZE             = 256;
static const int EEPROM_CALIB_ADDR       = 0;
static const int EEPROM_INITIAL_ADDR     = 4;
static const int EEPROM_CALIB_TEMP_ADDR  = 8;
static const int EEPROM_MAGIC_ADDR       = 12;
static const int EEPROM_HIST_COUNT_ADDR  = 13;
static const int EEPROM_HIST_IDX_ADDR    = 14;
static const int EEPROM_HIST_BASE        = 16;
static const uint8_t EEPROM_MAGIC        = 0xA5;
static const uint8_t HIST_MAX            = 10;
static const uint8_t HIST_RECORD_SIZE    = 11;

// ============================================================================
//  OBJETS GLOBAUX
// ============================================================================
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
Adafruit_ADS1115  ads;
RTC_DS3231        rtc;
HardwareSerial    printer(2);  // UART2
OneWire           oneWire(PIN_TEMP);
DallasTemperature tempSensor(&oneWire);
Adafruit_PN532    pn532(PIN_PN532_IRQ, PIN_PN532_RESET);
Adafruit_NeoPixel pixel(1, PIN_LED_RGB, NEO_GRB + NEO_KHZ800);

// ============================================================================
//  MACHINE A ETATS
// ============================================================================
enum Mode : uint8_t {
  MODE_SPLASH,
  MODE_READ,
  MODE_SET_TIME,
  MODE_HISTORY,
  MODE_FEEDBACK
};

enum TimeField : uint8_t {
  FIELD_HOUR, FIELD_MIN, FIELD_DAY, FIELD_MONTH, FIELD_YEAR, FIELD_DONE
};

enum FeedbackKind : uint8_t {
  FB_NONE, FB_CALIB_OK, FB_CALIB_FAIL, FB_PRINTED,
  FB_UNSTABLE, FB_NOT_CALIB, FB_CELL_WEAK,
  FB_BADGE_OK, FB_BADGE_TIMEOUT, FB_BADGE_AUTH_FAIL
};

static Mode         g_mode           = MODE_SPLASH;
static TimeField    g_field          = FIELD_HOUR;
static FeedbackKind g_feedback       = FB_NONE;
static uint32_t     g_feedbackEnd    = 0;
static uint32_t     g_splashEnd      = 0;
static bool         g_pendingSetTime = false;

// ============================================================================
//  ETAT CAPTEUR / UTILISATEUR
// ============================================================================
static float    g_o2Buffer[STABILITY_SAMPLES];
static uint8_t  g_bufIdx          = 0;
static uint8_t  g_bufFilled       = 0;
static float    g_currentO2       = 0.0f;
static bool     g_isStable        = false;
static float    g_calibMv         = 10.0f;
static float    g_initialCalibMv  = 10.0f;
static float    g_calibTempC      = 20.0f;
static bool     g_calibValid      = false;
static float    g_ppO2Target      = 1.4f;

// Temperature
static bool     g_tempPresent     = false;
static float    g_currentTempC    = 20.0f;
static uint32_t g_lastTempReq     = 0;
static bool     g_tempReqPending  = false;

// Edition horodatage
static uint16_t g_eYear;
static uint8_t  g_eMonth, g_eDay, g_eHour, g_eMin;

// Historique
static uint8_t  g_histViewIdx = 0;

// RFID / Badge
static bool     g_pn532Present   = false;
static char     g_currentName[NAME_MAX_LEN + 1] = {0};
static uint8_t  g_lastUid[7]     = {0};
static uint8_t  g_lastUidLen     = 0;
static uint32_t g_lastRfidPoll   = 0;
static bool     g_armed          = false;
static uint32_t g_armedEnd       = 0;

// ============================================================================
//  GESTION DES BOUTONS (anti-rebond + short/long press)
// ============================================================================
enum ButtonEvent : uint8_t { BTN_NONE, BTN_SHORT, BTN_LONG };

struct Button {
  uint8_t  pin;
  bool     rawState;
  bool     stableState;
  uint32_t lastEdge;
  uint32_t pressStart;
  bool     longFired;
};

static Button g_btnL = {PIN_BTN_LEFT,   false, false, 0, 0, false};
static Button g_btnC = {PIN_BTN_CENTER, false, false, 0, 0, false};
static Button g_btnR = {PIN_BTN_RIGHT,  false, false, 0, 0, false};

// Lecture brute du bouton selon le mode choisi.
// Retourne true si le bouton est "actif" (touche).
static inline bool readButtonRaw(uint8_t pin) {
#if BUTTON_MODE == 1
  // Touch capacitif natif : la valeur descend quand on touche
  return touchRead(pin) < TOUCH_THRESHOLD;
#else
  // TTP223 : sortie HIGH quand on touche
  return digitalRead(pin) == HIGH;
#endif
}

static ButtonEvent updateButton(Button &b) {
  ButtonEvent ev = BTN_NONE;
  const uint32_t now = millis();
  const bool raw = readButtonRaw(b.pin);

  if (raw != b.rawState) {
    b.rawState = raw;
    b.lastEdge = now;
  }

  if ((now - b.lastEdge) >= DEBOUNCE_MS && b.stableState != b.rawState) {
    const bool prev = b.stableState;
    b.stableState = b.rawState;

    if (b.stableState && !prev) {
      b.pressStart = now;
      b.longFired  = false;
    } else if (!b.stableState && prev) {
      if (!b.longFired) ev = BTN_SHORT;
    }
  }

  if (b.stableState && !b.longFired && (now - b.pressStart) >= LONG_PRESS_MS) {
    b.longFired = true;
    ev = BTN_LONG;
  }

  return ev;
}

// ============================================================================
//  EEPROM : CALIBRATION + HISTORIQUE
// ============================================================================
//   Note ESP32 : EEPROM est emule sur flash via NVS. Necessite EEPROM.begin()
//   au demarrage et EEPROM.commit() apres chaque ecriture.
//
static void eepromCommit() {
  EEPROM.commit();
}

static void loadCalibration() {
  uint8_t magic = 0;
  EEPROM.get(EEPROM_MAGIC_ADDR, magic);
  if (magic == EEPROM_MAGIC) {
    float v = 0.0f, vi = 0.0f, t = 20.0f;
    EEPROM.get(EEPROM_CALIB_ADDR,       v);
    EEPROM.get(EEPROM_INITIAL_ADDR,     vi);
    EEPROM.get(EEPROM_CALIB_TEMP_ADDR,  t);
    if (!isnan(v) && v > 0.1f && v < 100.0f) {
      g_calibMv        = v;
      g_initialCalibMv = (!isnan(vi) && vi > 0.1f && vi < 100.0f) ? vi : v;
      g_calibTempC     = (!isnan(t)  && t  > -10.0f && t < 80.0f) ? t  : 20.0f;
      g_calibValid     = true;
      return;
    }
  }
  g_calibMv        = 10.0f;
  g_initialCalibMv = 10.0f;
  g_calibTempC     = 20.0f;
  g_calibValid     = false;
}

static void saveCalibration(float mv, float tempC) {
  const bool firstTime = !g_calibValid;
  g_calibMv    = mv;
  g_calibTempC = tempC;

  EEPROM.put(EEPROM_CALIB_ADDR,      mv);
  EEPROM.put(EEPROM_CALIB_TEMP_ADDR, tempC);

  if (firstTime) {
    g_initialCalibMv = mv;
    EEPROM.put(EEPROM_INITIAL_ADDR, mv);
  }

  const uint8_t magic = EEPROM_MAGIC;
  EEPROM.put(EEPROM_MAGIC_ADDR, magic);
  eepromCommit();
  g_calibValid = true;
}

static uint8_t cellLifePercent() {
  if (g_initialCalibMv <= 0.1f) return 100;
  int pct = (int)((g_calibMv / g_initialCalibMv) * 100.0f + 0.5f);
  if (pct > 100) pct = 100;
  if (pct < 0)   pct = 0;
  return (uint8_t)pct;
}

struct HistRecord {
  uint8_t  yearOffset;
  uint8_t  month;
  uint8_t  day;
  uint8_t  hour;
  uint8_t  minute;
  uint16_t fO2_x10;
  uint16_t ppO2_x10;
  uint16_t mod;
};
static_assert(sizeof(HistRecord) == HIST_RECORD_SIZE,
              "HistRecord layout must be 11 bytes for EEPROM addressing");

static uint8_t histCount() {
  uint8_t c = 0;
  EEPROM.get(EEPROM_HIST_COUNT_ADDR, c);
  if (c > HIST_MAX) c = 0;
  return c;
}

static uint8_t histWriteIdx() {
  uint8_t i = 0;
  EEPROM.get(EEPROM_HIST_IDX_ADDR, i);
  if (i >= HIST_MAX) i = 0;
  return i;
}

static void histAdd(const HistRecord &r) {
  const uint8_t idx = histWriteIdx();
  const int addr = EEPROM_HIST_BASE + idx * HIST_RECORD_SIZE;
  EEPROM.put(addr, r);
  const uint8_t nextIdx = (idx + 1) % HIST_MAX;
  EEPROM.put(EEPROM_HIST_IDX_ADDR, nextIdx);
  uint8_t c = histCount();
  if (c < HIST_MAX) { c++; EEPROM.put(EEPROM_HIST_COUNT_ADDR, c); }
  eepromCommit();
}

static bool histRead(uint8_t viewIdx, HistRecord &out) {
  const uint8_t c = histCount();
  if (viewIdx >= c) return false;
  const uint8_t writeIdx = histWriteIdx();
  const int slot = ((int)writeIdx - 1 - (int)viewIdx + HIST_MAX * 2) % HIST_MAX;
  const int addr = EEPROM_HIST_BASE + slot * HIST_RECORD_SIZE;
  EEPROM.get(addr, out);
  return true;
}

// ============================================================================
//  LECTURE O2 + STABILITE + COMPENSATION TEMP
// ============================================================================
static void resetStabilityBuffer() {
  g_bufIdx     = 0;
  g_bufFilled  = 0;
  g_isStable   = false;
  for (uint8_t i = 0; i < STABILITY_SAMPLES; i++) g_o2Buffer[i] = 0.0f;
}

static void sampleO2() {
  const int16_t raw = ads.readADC_SingleEnded(ADS_O2_CHANNEL);
  const float voltage = (float)raw * ADS_LSB_MV;

  float o2 = (g_calibMv > 0.1f)
           ? (voltage / g_calibMv) * CALIB_REF_PERCENT
           : 0.0f;

  if (g_tempPresent && g_calibValid) {
    const float dT = g_currentTempC - g_calibTempC;
    const float factor = 1.0f + TEMP_COEFF_PER_C * dT;
    if (factor > 0.5f) o2 /= factor;
  }

  if (o2 < 0.0f)  o2 = 0.0f;
  if (o2 > 99.9f) o2 = 99.9f;
  g_currentO2 = o2;

  g_o2Buffer[g_bufIdx] = g_currentO2;
  g_bufIdx = (g_bufIdx + 1) % STABILITY_SAMPLES;
  if (g_bufFilled < STABILITY_SAMPLES) g_bufFilled++;

  if (g_bufFilled >= STABILITY_SAMPLES) {
    float mn = g_o2Buffer[0];
    float mx = g_o2Buffer[0];
    for (uint8_t i = 1; i < STABILITY_SAMPLES; i++) {
      if (g_o2Buffer[i] < mn) mn = g_o2Buffer[i];
      if (g_o2Buffer[i] > mx) mx = g_o2Buffer[i];
    }
    g_isStable = (mx - mn) < STABILITY_THRESHOLD;
  } else {
    g_isStable = false;
  }
}

static void updateTemperature() {
  if (!g_tempPresent) return;
  const uint32_t now = millis();

  if (!g_tempReqPending && (now - g_lastTempReq) >= TEMP_INTERVAL_MS) {
    tempSensor.requestTemperatures();
    g_lastTempReq    = now;
    g_tempReqPending = true;
  }
  if (g_tempReqPending && (now - g_lastTempReq) >= TEMP_CONV_MS) {
    const float t = tempSensor.getTempCByIndex(0);
    if (t > -50.0f && t < 100.0f) g_currentTempC = t;
    g_tempReqPending = false;
  }
}

// ============================================================================
//  CALCULS METIER
// ============================================================================
static int computeMOD(float fO2pct, float ppO2) {
  if (fO2pct <= 0.5f) return 0;
  const float mod = ((ppO2 / (fO2pct / 100.0f)) - 1.0f) * 10.0f;
  if (mod < 0.0f)   return 0;
  if (mod > 999.0f) return 999;
  return (int)(mod + 0.5f);
}

// ============================================================================
//  UTILITAIRES DATE
// ============================================================================
static uint8_t daysInMonth(uint8_t m, uint16_t y) {
  if (m == 2) return (y%4==0 && (y%100!=0 || y%400==0)) ? 29 : 28;
  if (m == 4 || m == 6 || m == 9 || m == 11) return 30;
  return 31;
}

static void clampEditDay() {
  const uint8_t dmax = daysInMonth(g_eMonth, g_eYear);
  if (g_eDay > dmax) g_eDay = dmax;
  if (g_eDay < 1)    g_eDay = 1;
}

static void loadEditBufferFromRtc() {
  DateTime n = rtc.now();
  g_eYear  = n.year();
  g_eMonth = n.month();
  g_eDay   = n.day();
  g_eHour  = n.hour();
  g_eMin   = n.minute();
}

static void adjustField(int8_t delta) {
  switch (g_field) {
    case FIELD_HOUR:  g_eHour = (uint8_t)((g_eHour + delta + 24) % 24); break;
    case FIELD_MIN:   g_eMin  = (uint8_t)((g_eMin  + delta + 60) % 60); break;
    case FIELD_DAY: {
      const uint8_t dmax = daysInMonth(g_eMonth, g_eYear);
      g_eDay = (uint8_t)(((int)g_eDay - 1 + delta + dmax) % dmax + 1);
      break;
    }
    case FIELD_MONTH:
      g_eMonth = (uint8_t)(((int)g_eMonth - 1 + delta + 12) % 12 + 1);
      clampEditDay();
      break;
    case FIELD_YEAR:
      g_eYear = (uint16_t)constrain((int)g_eYear + delta, 2024, 2099);
      clampEditDay();
      break;
    default: break;
  }
}

// ============================================================================
//  AFFICHAGE LCD
// ============================================================================
static void lcdPrintPadded(const char *s) {
  lcd.print(s);
  for (uint8_t i = strlen(s); i < 16; i++) lcd.print(' ');
}

static void displaySplash() {
  char l2[20];
  if (g_calibValid) {
    const int mv_i = (int)g_calibMv;
    const int mv_d = (int)((g_calibMv - mv_i) * 10.0f + 0.5f);
    snprintf(l2, sizeof(l2), "Cal:%d.%dmV V%d%%",
             mv_i, mv_d, cellLifePercent());
  } else {
    strcpy(l2, "NON CALIBRE!");
  }
  lcd.setCursor(0, 0); lcdPrintPadded("Analyseur O2");
  lcd.setCursor(0, 1); lcdPrintPadded(l2);
}

static void displayRead() {
  char l1[20], l2[20];

  if (g_calibValid) {
    const int o2x10 = (int)(g_currentO2 * 10.0f + 0.5f);
    const char *tag = g_isStable ? "[OK]" : " ...";
    snprintf(l1, sizeof(l1), "O2:%2d.%d%% %s",
             o2x10 / 10, o2x10 % 10, tag);
  } else {
    strcpy(l1, "O2: ---  NONCAL");
  }

  if (g_armed) {
    const uint32_t now32 = millis();
    int rem = (int)((g_armedEnd - now32) / 1000);
    if (rem < 0)  rem = 0;
    if (rem > 99) rem = 99;
    char nameTrunc[9];
    strncpy(nameTrunc, g_currentName, 8);
    nameTrunc[8] = 0;
    snprintf(l2, sizeof(l2), "ARM:%-8s%2ds", nameTrunc, rem);
  } else {
    const int mod  = g_calibValid ? computeMOD(g_currentO2, g_ppO2Target) : 0;
    const int px10 = (int)(g_ppO2Target * 10.0f + 0.5f);
    const DateTime now = rtc.now();
    snprintf(l2, sizeof(l2), "M:%3d p%d.%d %02d:%02d",
             mod, px10 / 10, px10 % 10, now.hour(), now.minute());
  }

  lcd.setCursor(0, 0); lcdPrintPadded(l1);
  lcd.setCursor(0, 1); lcdPrintPadded(l2);
}

static void displaySetTime() {
  char l1[20], l2[20];
  const char *label;
  switch (g_field) {
    case FIELD_HOUR:  label = "Heure";  break;
    case FIELD_MIN:   label = "Minute"; break;
    case FIELD_DAY:   label = "Jour";   break;
    case FIELD_MONTH: label = "Mois";   break;
    case FIELD_YEAR:  label = "Annee";  break;
    default:          label = "?";      break;
  }
  snprintf(l1, sizeof(l1), "Reglage %s", label);

  const uint16_t yy = g_eYear % 100;
  switch (g_field) {
    case FIELD_HOUR:
      snprintf(l2, sizeof(l2), "[%02d]:%02d %02d/%02d/%02d",
               g_eHour, g_eMin, g_eDay, g_eMonth, yy);
      break;
    case FIELD_MIN:
      snprintf(l2, sizeof(l2), "%02d:[%02d] %02d/%02d/%02d",
               g_eHour, g_eMin, g_eDay, g_eMonth, yy);
      break;
    case FIELD_DAY:
      snprintf(l2, sizeof(l2), "%02d:%02d [%02d]/%02d/%02d",
               g_eHour, g_eMin, g_eDay, g_eMonth, yy);
      break;
    case FIELD_MONTH:
      snprintf(l2, sizeof(l2), "%02d:%02d %02d/[%02d]/%02d",
               g_eHour, g_eMin, g_eDay, g_eMonth, yy);
      break;
    case FIELD_YEAR:
      snprintf(l2, sizeof(l2), "%02d:%02d %02d/%02d/[%02d]",
               g_eHour, g_eMin, g_eDay, g_eMonth, yy);
      break;
    default:
      snprintf(l2, sizeof(l2), "%02d:%02d %02d/%02d/%02d",
               g_eHour, g_eMin, g_eDay, g_eMonth, yy);
      break;
  }

  lcd.setCursor(0, 0); lcdPrintPadded(l1);
  lcd.setCursor(0, 1); lcdPrintPadded(l2);
}

static void displayHistory() {
  char l1[20], l2[20];
  const uint8_t c = histCount();

  if (c == 0) {
    lcd.setCursor(0, 0); lcdPrintPadded("Historique");
    lcd.setCursor(0, 1); lcdPrintPadded("(vide)");
    return;
  }

  HistRecord r;
  if (!histRead(g_histViewIdx, r)) {
    lcd.setCursor(0, 0); lcdPrintPadded("Histo: erreur");
    lcd.setCursor(0, 1); lcdPrintPadded("");
    return;
  }

  snprintf(l1, sizeof(l1), "#%d/%d fO2:%d.%d%%",
           g_histViewIdx + 1, c,
           r.fO2_x10 / 10, r.fO2_x10 % 10);
  snprintf(l2, sizeof(l2), "%02d/%02d %02d:%02d M%d",
           r.day, r.month, r.hour, r.minute, r.mod);

  lcd.setCursor(0, 0); lcdPrintPadded(l1);
  lcd.setCursor(0, 1); lcdPrintPadded(l2);
}

static void displayFeedback() {
  const char *l1 = "";
  const char *l2 = "";
  char l2buf[20];
  switch (g_feedback) {
    case FB_CALIB_OK:
      l1 = "Calibration OK";
      snprintf(l2buf, sizeof(l2buf), "V:%d%% T:%dC",
               cellLifePercent(), (int)g_currentTempC);
      l2 = l2buf;
      break;
    case FB_CALIB_FAIL: l1 = "Calib. ERREUR"; l2 = "Tension faible";  break;
    case FB_PRINTED:    l1 = "Impression...";  l2 = "Etiquette OK";   break;
    case FB_UNSTABLE:   l1 = "Instable!";      l2 = "Attendez [OK]";  break;
    case FB_NOT_CALIB:  l1 = "NON CALIBRE!";   l2 = "App. long DROIT";break;
    case FB_CELL_WEAK:
      l1 = "CELLULE USEE!";
      snprintf(l2buf, sizeof(l2buf), "Vie: %d%% - chg",
               cellLifePercent());
      l2 = l2buf;
      break;
    case FB_BADGE_OK:
      l1 = "Impression OK";
      snprintf(l2buf, sizeof(l2buf), ">> %s", g_currentName);
      l2 = l2buf;
      break;
    case FB_BADGE_TIMEOUT:
      l1 = "Timeout badge";
      l2 = "Pas stabilise";
      break;
    case FB_BADGE_AUTH_FAIL:
      l1 = "Badge invalide";
      l2 = "Cle ou format";
      break;
    default: break;
  }
  lcd.setCursor(0, 0); lcdPrintPadded(l1);
  lcd.setCursor(0, 1); lcdPrintPadded(l2);
}

// ============================================================================
//  IMPRESSION TSPL (TSC TH240) + AJOUT HISTORIQUE
// ============================================================================
static void printLabel(const char *plongeurName) {
  const DateTime now = rtc.now();
  const int mod   = computeMOD(g_currentO2, g_ppO2Target);
  const int o2x10 = (int)(g_currentO2 * 10.0f + 0.5f);
  const int px10  = (int)(g_ppO2Target * 10.0f + 0.5f);

  char line[80];

  printer.println(F("SIZE 50 mm, 30 mm"));
  printer.println(F("GAP 2 mm, 0 mm"));
  printer.println(F("DIRECTION 1"));
  printer.println(F("DENSITY 8"));
  printer.println(F("SPEED 4"));
  printer.println(F("CLS"));

  printer.println(F("TEXT 20,10,\"4\",0,1,1,\"ANALYSE O2\""));

  if (plongeurName != NULL && plongeurName[0] != 0) {
    snprintf(line, sizeof(line),
             "TEXT 20,55,\"3\",0,1,1,\"Plongeur: %s\"", plongeurName);
  } else {
    snprintf(line, sizeof(line),
             "TEXT 20,55,\"3\",0,1,1,\"Plongeur: ____________\"");
  }
  printer.println(line);

  snprintf(line, sizeof(line),
           "TEXT 20,95,\"5\",0,1,1,\"O2: %d.%d %%\"",
           o2x10 / 10, o2x10 % 10);
  printer.println(line);

  snprintf(line, sizeof(line),
           "TEXT 20,150,\"4\",0,1,1,\"MOD: %d m\"", mod);
  printer.println(line);

  snprintf(line, sizeof(line),
           "TEXT 20,190,\"3\",0,1,1,\"ppO2 ref: %d.%d\"",
           px10 / 10, px10 % 10);
  printer.println(line);

  snprintf(line, sizeof(line),
           "TEXT 20,225,\"2\",0,1,1,\"%04d-%02d-%02d  %02d:%02d\"",
           now.year(), now.month(), now.day(),
           now.hour(), now.minute());
  printer.println(line);

  printer.println(F("PRINT 1,1"));

  HistRecord r;
  r.yearOffset = (now.year() >= 2024) ? (uint8_t)(now.year() - 2024) : 0;
  r.month      = now.month();
  r.day        = now.day();
  r.hour       = now.hour();
  r.minute     = now.minute();
  r.fO2_x10    = (uint16_t)o2x10;
  r.ppO2_x10   = (uint16_t)px10;
  r.mod        = (uint16_t)mod;
  histAdd(r);
}

// ============================================================================
//  LED RGB
// ============================================================================
static void setLed(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

static void updateLED() {
  static uint32_t lastUpdate = 0;
  const uint32_t now = millis();
  if ((now - lastUpdate) < 100) return;
  lastUpdate = now;

  uint8_t r = 0, g = 0, b = 0;
  bool blink = false;

  switch (g_mode) {
    case MODE_SPLASH:
      r = 30; g = 30; b = 30;
      break;
    case MODE_READ:
      if (!g_calibValid) {
        r = 255; blink = true;
      } else if (g_armed) {
        b = 255; blink = true;
      } else if (g_isStable) {
        g = 255;
      } else {
        r = 200; g = 80;
      }
      break;
    case MODE_SET_TIME:
      r = 200; g = 150;
      break;
    case MODE_HISTORY:
      g = 150; b = 200;
      break;
    case MODE_FEEDBACK:
      switch (g_feedback) {
        case FB_PRINTED:
        case FB_BADGE_OK:       r = 200; b = 200; break;
        case FB_CALIB_OK:       g = 255;          break;
        case FB_CALIB_FAIL:
        case FB_NOT_CALIB:
        case FB_CELL_WEAK:
        case FB_BADGE_AUTH_FAIL:
        case FB_BADGE_TIMEOUT:  r = 255;          break;
        case FB_UNSTABLE:       r = 200; g = 80;  break;
        default:                                  break;
      }
      break;
  }

  if (blink && (now / LED_BLINK_MS) % 2 == 0) {
    r = g = b = 0;
  }

  setLed(r, g, b);
}

// ============================================================================
//  RFID PN532
// ============================================================================
static bool readBadgeName(const uint8_t *uid, uint8_t uidLen, char *outName) {
  if (uidLen != 4) return false;

  uint8_t key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  if (!pn532.mifareclassic_AuthenticateBlock(
        (uint8_t *)uid, uidLen, 4, 0, key)) {
    return false;
  }

  uint8_t data[16];
  if (!pn532.mifareclassic_ReadDataBlock(4, data)) return false;

  uint8_t i = 0;
  for (; i < NAME_MAX_LEN; i++) {
    const uint8_t c = data[i];
    if (c == 0 || c < 0x20 || c > 0x7E) break;
    outName[i] = (char)c;
  }
  outName[i] = 0;
  return outName[0] != 0;
}

static void enterFeedback(FeedbackKind kind);

static void onBadgeRead(const char *name) {
  if (!g_calibValid) {
    enterFeedback(FB_NOT_CALIB);
    return;
  }
  strncpy(g_currentName, name, sizeof(g_currentName));
  g_currentName[sizeof(g_currentName) - 1] = 0;

  if (g_isStable) {
    printLabel(g_currentName);
    enterFeedback(FB_BADGE_OK);
    g_armed = false;
  } else {
    g_armed    = true;
    g_armedEnd = millis() + ARMED_TIMEOUT_MS;
  }
}

static void pollRfid() {
  if (!g_pn532Present) return;
  const uint32_t now = millis();
  if ((now - g_lastRfidPoll) < RFID_POLL_MS) return;
  g_lastRfidPoll = now;

  uint8_t uid[7];
  uint8_t uidLen = 0;
  const bool found = pn532.readPassiveTargetID(
      PN532_MIFARE_ISO14443A, uid, &uidLen, 50);

  if (!found) {
    g_lastUidLen = 0;
    return;
  }

  if (uidLen == g_lastUidLen &&
      memcmp(uid, g_lastUid, uidLen) == 0) {
    if (g_armed) g_armedEnd = now + ARMED_TIMEOUT_MS;
    return;
  }

  memcpy(g_lastUid, uid, uidLen);
  g_lastUidLen = uidLen;

  char name[NAME_MAX_LEN + 1] = {0};
  if (readBadgeName(uid, uidLen, name)) {
    onBadgeRead(name);
  } else {
    enterFeedback(FB_BADGE_AUTH_FAIL);
  }
}

// ============================================================================
//  TRANSITIONS D'ETAT
// ============================================================================
static void enterFeedback(FeedbackKind kind) {
  g_feedback    = kind;
  g_feedbackEnd = millis() + FEEDBACK_MS;
  g_mode        = MODE_FEEDBACK;
  lcd.clear();
}

static void enterRead() {
  resetStabilityBuffer();
  g_armed = false;
  g_mode  = MODE_READ;
  lcd.clear();
}

static void enterSetTime() {
  loadEditBufferFromRtc();
  g_field = FIELD_HOUR;
  g_mode  = MODE_SET_TIME;
  lcd.clear();
}

static void enterHistory() {
  g_histViewIdx = 0;
  g_mode = MODE_HISTORY;
  lcd.clear();
}

static void performCalibration() {
  const int16_t raw = ads.readADC_SingleEnded(ADS_O2_CHANNEL);
  const float mv    = (float)raw * ADS_LSB_MV;
  if (mv > 1.0f && mv < 50.0f) {
    saveCalibration(mv, g_currentTempC);
    resetStabilityBuffer();
    if (cellLifePercent() < CELL_WARN_PERCENT) {
      enterFeedback(FB_CELL_WEAK);
    } else {
      enterFeedback(FB_CALIB_OK);
    }
  } else {
    enterFeedback(FB_CALIB_FAIL);
  }
}

// ============================================================================
//  SETUP / LOOP
// ============================================================================
void setup() {
  // Boutons : pinMode necessaire seulement en mode TTP223
#if BUTTON_MODE == 0
  pinMode(PIN_BTN_LEFT,   INPUT);
  pinMode(PIN_BTN_CENTER, INPUT);
  pinMode(PIN_BTN_RIGHT,  INPUT);
#endif

  // I2C : sur ESP32 par defaut SDA=21, SCL=22
  Wire.begin();

  lcd.init();
  lcd.backlight();
  lcd.clear();

  ads.setGain(GAIN_SIXTEEN);
  ads.begin();

  rtc.begin();
  const bool rtcLost = rtc.lostPower();
  if (rtcLost) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // UART2 sur ESP32 (Serial2) - 9600 bauds vers l'imprimante TSC
  printer.begin(9600, SERIAL_8N1, PIN_PRINTER_RX, PIN_PRINTER_TX);

  tempSensor.begin();
  tempSensor.setWaitForConversion(false);
  g_tempPresent = (tempSensor.getDeviceCount() > 0);
  if (g_tempPresent) {
    tempSensor.requestTemperatures();
    g_lastTempReq    = millis();
    g_tempReqPending = true;
  }

  pixel.begin();
  pixel.setBrightness(LED_BRIGHTNESS);
  pixel.clear();
  pixel.show();

  pn532.begin();
  const uint32_t fw = pn532.getFirmwareVersion();
  if (fw != 0) {
    pn532.SAMConfig();
    g_pn532Present = true;
  }

  // EEPROM emule sur ESP32 - allocation explicite
  EEPROM.begin(EEPROM_SIZE);
  loadCalibration();
  resetStabilityBuffer();

  g_mode           = MODE_SPLASH;
  g_splashEnd      = millis() + (g_calibValid ? SPLASH_MS : SPLASH_NOCAL_MS);
  g_pendingSetTime = rtcLost;
  displaySplash();
}

void loop() {
  const uint32_t now = millis();

  updateTemperature();
  updateLED();

  const ButtonEvent eL = updateButton(g_btnL);
  const ButtonEvent eC = updateButton(g_btnC);
  const ButtonEvent eR = updateButton(g_btnR);

  static uint32_t lastSample = 0;
  if (g_mode == MODE_READ && (now - lastSample) >= SAMPLE_INTERVAL_MS) {
    lastSample = now;
    sampleO2();
  }

  if (g_mode == MODE_READ) {
    pollRfid();

    if (g_armed) {
      if (g_isStable && g_calibValid) {
        printLabel(g_currentName);
        g_armed = false;
        enterFeedback(FB_BADGE_OK);
      } else if ((int32_t)(now - g_armedEnd) >= 0) {
        g_armed = false;
        enterFeedback(FB_BADGE_TIMEOUT);
      }
    }
  }

  switch (g_mode) {

    case MODE_SPLASH:
      if ((int32_t)(now - g_splashEnd) >= 0) {
        if (g_pendingSetTime) {
          g_pendingSetTime = false;
          enterSetTime();
        } else {
          enterRead();
        }
      }
      break;

    case MODE_READ:
      if (eL == BTN_SHORT) {
        g_ppO2Target -= PPO2_STEP;
        if (g_ppO2Target < PPO2_MIN) g_ppO2Target = PPO2_MIN;
      }
      if (eR == BTN_SHORT) {
        g_ppO2Target += PPO2_STEP;
        if (g_ppO2Target > PPO2_MAX) g_ppO2Target = PPO2_MAX;
      }
      if (eC == BTN_SHORT) {
        if (g_armed) {
          g_armed = false;
          lcd.clear();
        } else if (!g_calibValid) {
          enterFeedback(FB_NOT_CALIB);
        } else if (!g_isStable) {
          enterFeedback(FB_UNSTABLE);
        } else {
          printLabel(NULL);
          enterFeedback(FB_PRINTED);
        }
      }
      if (eL == BTN_LONG) enterHistory();
      if (eC == BTN_LONG) enterSetTime();
      if (eR == BTN_LONG) performCalibration();
      break;

    case MODE_SET_TIME:
      if (eL == BTN_SHORT) adjustField(-1);
      if (eR == BTN_SHORT) adjustField(+1);
      if (eC == BTN_SHORT) {
        g_field = (TimeField)(g_field + 1);
        if (g_field >= FIELD_DONE) {
          rtc.adjust(DateTime(g_eYear, g_eMonth, g_eDay,
                              g_eHour, g_eMin, 0));
          enterRead();
        }
      }
      if (eL == BTN_LONG) enterRead();
      break;

    case MODE_HISTORY: {
      const uint8_t c = histCount();
      if (eL == BTN_SHORT && c > 0) {
        if (g_histViewIdx + 1 < c) g_histViewIdx++;
      }
      if (eR == BTN_SHORT && c > 0) {
        if (g_histViewIdx > 0) g_histViewIdx--;
      }
      if (eC == BTN_SHORT) enterRead();
      break;
    }

    case MODE_FEEDBACK:
      if ((int32_t)(now - g_feedbackEnd) >= 0) {
        g_feedback = FB_NONE;
        enterRead();
      }
      break;
  }

  static uint32_t lastDisplay = 0;
  if ((now - lastDisplay) >= DISPLAY_INTERVAL_MS) {
    lastDisplay = now;
    switch (g_mode) {
      case MODE_SPLASH:   displaySplash();   break;
      case MODE_READ:     displayRead();     break;
      case MODE_SET_TIME: displaySetTime();  break;
      case MODE_HISTORY:  displayHistory();  break;
      case MODE_FEEDBACK: displayFeedback(); break;
    }
  }
}
