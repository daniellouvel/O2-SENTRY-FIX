/*
 * Analyseur O2 fixe (de paillasse, alimentation 220V AC) - Arduino Nano
 *
 * Capteur O2     : ADS1115 A0 (gain x16)
 * Affichage      : LCD 1602 I2C @0x27
 * Horloge        : DS3231 I2C @0x68
 * Boutons        : TTP223 sur D2 (GAUCHE) / D3 (CENTRE) / D4 (DROITE)
 * Temperature    : DS18B20 sur D5 (OPTIONNEL - compensation si detecte)
 * Imprimante     : TSC TH240 via SoftwareSerial D10 (RX) / D11 (TX)
 *
 * Architecture : machine a etats, aucun delay(), tout sur millis().
 */

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_ADS1X15.h>
#include <RTClib.h>
#include <SoftwareSerial.h>
#include <EEPROM.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ============================================================================
//  CONFIGURATION MATERIELLE
// ============================================================================
static const uint8_t PIN_BTN_LEFT   = 2;
static const uint8_t PIN_BTN_CENTER = 3;
static const uint8_t PIN_BTN_RIGHT  = 4;
static const uint8_t PIN_TEMP       = 5;   // DS18B20 OneWire (optionnel)
static const uint8_t PIN_PRINTER_RX = 10;
static const uint8_t PIN_PRINTER_TX = 11;
static const uint8_t LCD_ADDR       = 0x27;
static const uint8_t ADS_O2_CHANNEL = 0;

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
static const float    TEMP_COEFF_PER_C    = 0.003f;  // 0.3% / C
static const uint8_t  CELL_WARN_PERCENT   = 80;

// EEPROM layout
static const int EEPROM_CALIB_ADDR       = 0;    // float 4B
static const int EEPROM_INITIAL_ADDR     = 4;    // float 4B
static const int EEPROM_CALIB_TEMP_ADDR  = 8;    // float 4B
static const int EEPROM_MAGIC_ADDR       = 12;   // uint8_t
static const int EEPROM_HIST_COUNT_ADDR  = 13;   // uint8_t
static const int EEPROM_HIST_IDX_ADDR    = 14;   // uint8_t
static const int EEPROM_HIST_BASE        = 16;   // 10 x 11B
static const uint8_t EEPROM_MAGIC        = 0xA5;
static const uint8_t HIST_MAX            = 10;
static const uint8_t HIST_RECORD_SIZE    = 11;

// ============================================================================
//  OBJETS GLOBAUX
// ============================================================================
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
Adafruit_ADS1115  ads;
RTC_DS3231        rtc;
SoftwareSerial    printer(PIN_PRINTER_RX, PIN_PRINTER_TX);
OneWire           oneWire(PIN_TEMP);
DallasTemperature tempSensor(&oneWire);

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
  FB_UNSTABLE, FB_NOT_CALIB, FB_CELL_WEAK
};

static Mode         g_mode           = MODE_SPLASH;
static TimeField    g_field          = FIELD_HOUR;
static FeedbackKind g_feedback       = FB_NONE;
static uint32_t     g_feedbackEnd    = 0;
static uint32_t     g_splashEnd      = 0;
static bool         g_pendingSetTime = false;  // force SET_TIME apres splash

// ============================================================================
//  ETAT CAPTEUR / UTILISATEUR
// ============================================================================
static float    g_o2Buffer[STABILITY_SAMPLES];
static uint8_t  g_bufIdx      = 0;
static uint8_t  g_bufFilled   = 0;
static float    g_currentO2   = 0.0f;
static bool     g_isStable    = false;
static float    g_calibMv         = 10.0f;   // mV a 20.9% (calibration courante)
static float    g_initialCalibMv  = 10.0f;   // mV premiere calibration (vie cellule)
static float    g_calibTempC      = 20.0f;   // C a la calibration
static bool     g_calibValid      = false;   // EEPROM magic trouve ?
static float    g_ppO2Target      = 1.4f;

// Temperature
static bool     g_tempPresent     = false;
static float    g_currentTempC    = 20.0f;
static uint32_t g_lastTempReq     = 0;
static bool     g_tempReqPending  = false;

// Buffer d'edition horodatage
static uint16_t g_eYear;
static uint8_t  g_eMonth, g_eDay, g_eHour, g_eMin;

// Historique (index de lecture dans MODE_HISTORY)
static uint8_t  g_histViewIdx = 0;  // 0 = plus recent

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

static ButtonEvent updateButton(Button &b) {
  ButtonEvent ev = BTN_NONE;
  const uint32_t now = millis();
  const bool raw = (digitalRead(b.pin) == HIGH);

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
  uint8_t  yearOffset;  // annee - 2024
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
}

static bool histRead(uint8_t viewIdx, HistRecord &out) {
  const uint8_t c = histCount();
  if (viewIdx >= c) return false;
  const uint8_t writeIdx = histWriteIdx();
  // recent = writeIdx - 1 - viewIdx (modulo HIST_MAX)
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

  // Compensation temperature (uniquement si DS18B20 present et calibration valide)
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

  const int mod  = g_calibValid ? computeMOD(g_currentO2, g_ppO2Target) : 0;
  const int px10 = (int)(g_ppO2Target * 10.0f + 0.5f);
  const DateTime now = rtc.now();
  // Format compact 16 cols : "M: 34 p1.4 12:45" (sans suffixe m, context evident)
  snprintf(l2, sizeof(l2), "M:%3d p%d.%d %02d:%02d",
           mod, px10 / 10, px10 % 10, now.hour(), now.minute());

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

  // Ligne 2 : "HH:MM DD/MM/YY" = 14 chars + crochets (+2) = 16 EXACTEMENT
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

  // "#10/10 fO2:99.9%" = 16 max
  snprintf(l1, sizeof(l1), "#%d/%d fO2:%d.%d%%",
           g_histViewIdx + 1, c,
           r.fO2_x10 / 10, r.fO2_x10 % 10);
  // "24/04 12:45 M:34" = 16 max (MOD borne a 999 dans computeMOD)
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
    default: break;
  }
  lcd.setCursor(0, 0); lcdPrintPadded(l1);
  lcd.setCursor(0, 1); lcdPrintPadded(l2);
}

// ============================================================================
//  IMPRESSION TSPL (TSC TH240) + AJOUT HISTORIQUE
// ============================================================================
static void printLabel() {
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

  printer.println(F("TEXT 20,15,\"4\",0,1,1,\"ANALYSE O2\""));

  snprintf(line, sizeof(line),
           "TEXT 20,65,\"5\",0,1,1,\"O2: %d.%d %%\"",
           o2x10 / 10, o2x10 % 10);
  printer.println(line);

  snprintf(line, sizeof(line),
           "TEXT 20,125,\"4\",0,1,1,\"MOD: %d m\"", mod);
  printer.println(line);

  snprintf(line, sizeof(line),
           "TEXT 20,170,\"3\",0,1,1,\"ppO2 ref: %d.%d\"",
           px10 / 10, px10 % 10);
  printer.println(line);

  snprintf(line, sizeof(line),
           "TEXT 20,210,\"2\",0,1,1,\"%04d-%02d-%02d  %02d:%02d\"",
           now.year(), now.month(), now.day(),
           now.hour(), now.minute());
  printer.println(line);

  printer.println(F("PRINT 1,1"));

  // Ajout a l'historique
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
  g_mode = MODE_READ;
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
  pinMode(PIN_BTN_LEFT,   INPUT);
  pinMode(PIN_BTN_CENTER, INPUT);
  pinMode(PIN_BTN_RIGHT,  INPUT);

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

  printer.begin(9600);

  tempSensor.begin();
  tempSensor.setWaitForConversion(false);
  g_tempPresent = (tempSensor.getDeviceCount() > 0);
  if (g_tempPresent) {
    tempSensor.requestTemperatures();
    g_lastTempReq    = millis();
    g_tempReqPending = true;
  }

  loadCalibration();
  resetStabilityBuffer();

  // Splash plus long si non calibre, pour que l'utilisateur lise "NON CALIBRE"
  g_mode           = MODE_SPLASH;
  g_splashEnd      = millis() + (g_calibValid ? SPLASH_MS : SPLASH_NOCAL_MS);
  g_pendingSetTime = rtcLost;  // si RTC a perdu l'alim : forcer SET_TIME
  displaySplash();
}

void loop() {
  const uint32_t now = millis();

  updateTemperature();

  const ButtonEvent eL = updateButton(g_btnL);
  const ButtonEvent eC = updateButton(g_btnC);
  const ButtonEvent eR = updateButton(g_btnR);

  // Echantillonnage O2 en MODE_READ uniquement
  static uint32_t lastSample = 0;
  if (g_mode == MODE_READ && (now - lastSample) >= SAMPLE_INTERVAL_MS) {
    lastSample = now;
    sampleO2();
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
        if (!g_calibValid) {
          enterFeedback(FB_NOT_CALIB);
        } else if (!g_isStable) {
          enterFeedback(FB_UNSTABLE);
        } else {
          printLabel();
          enterFeedback(FB_PRINTED);
        }
      }
      if (eL == BTN_LONG) {
        enterHistory();
      }
      if (eC == BTN_LONG) {
        enterSetTime();
      }
      if (eR == BTN_LONG) {
        performCalibration();
      }
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
      if (eL == BTN_LONG) {  // annuler sans sauver
        enterRead();
      }
      break;

    case MODE_HISTORY: {
      const uint8_t c = histCount();
      if (eL == BTN_SHORT && c > 0) {
        if (g_histViewIdx + 1 < c) g_histViewIdx++;
      }
      if (eR == BTN_SHORT && c > 0) {
        if (g_histViewIdx > 0) g_histViewIdx--;
      }
      if (eC == BTN_SHORT) {
        enterRead();
      }
      break;
    }

    case MODE_FEEDBACK:
      if ((int32_t)(now - g_feedbackEnd) >= 0) {
        g_feedback = FB_NONE;
        enterRead();
      }
      break;
  }

  // Rafraichissement LCD
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
