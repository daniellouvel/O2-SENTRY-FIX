/*
 * Analyseur O2 fixe (de paillasse, alimentation 220V AC) - Arduino Nano
 * Capteur O2 via ADS1115 (A0), LCD 1602 I2C, RTC DS3231,
 * 3 boutons TTP223 (D2/D3/D4), imprimante TSC TH240 (D10/D11).
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

// ============================================================================
//  CONFIGURATION MATERIELLE
// ============================================================================
static const uint8_t PIN_BTN_LEFT   = 2;
static const uint8_t PIN_BTN_CENTER = 3;
static const uint8_t PIN_BTN_RIGHT  = 4;
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
static const float    PPO2_MIN            = 1.0f;
static const float    PPO2_MAX            = 1.6f;
static const float    PPO2_STEP           = 0.1f;
static const float    CALIB_REF_PERCENT   = 20.9f;
static const float    ADS_LSB_MV          = 0.0078125f;

static const int EEPROM_CALIB_ADDR = 0;
static const int EEPROM_MAGIC_ADDR = 8;
static const uint8_t EEPROM_MAGIC  = 0xA5;

// ============================================================================
//  OBJETS GLOBAUX
// ============================================================================
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
Adafruit_ADS1115  ads;
RTC_DS3231        rtc;
SoftwareSerial    printer(PIN_PRINTER_RX, PIN_PRINTER_TX);

// ============================================================================
//  MACHINE A ETATS
// ============================================================================
enum Mode : uint8_t {
  MODE_READ,
  MODE_SET_TIME,
  MODE_FEEDBACK
};

enum TimeField : uint8_t {
  FIELD_HOUR, FIELD_MIN, FIELD_DAY, FIELD_MONTH, FIELD_YEAR, FIELD_DONE
};

enum FeedbackKind : uint8_t {
  FB_NONE, FB_CALIB_OK, FB_CALIB_FAIL, FB_PRINTED, FB_UNSTABLE
};

static Mode         g_mode        = MODE_READ;
static TimeField    g_field       = FIELD_HOUR;
static FeedbackKind g_feedback    = FB_NONE;
static uint32_t     g_feedbackEnd = 0;

// ============================================================================
//  ETAT CAPTEUR / UTILISATEUR
// ============================================================================
static float    g_o2Buffer[STABILITY_SAMPLES];
static uint8_t  g_bufIdx      = 0;
static uint8_t  g_bufFilled   = 0;
static float    g_currentO2   = 0.0f;
static float    g_lastVoltage = 0.0f;
static bool     g_isStable    = false;
static float    g_calibMv     = 10.0f;
static float    g_ppO2Target  = 1.4f;

static uint16_t g_eYear;
static uint8_t  g_eMonth, g_eDay, g_eHour, g_eMin;

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
//  EEPROM : CALIBRATION
// ============================================================================
static void loadCalibration() {
  uint8_t magic = 0;
  EEPROM.get(EEPROM_MAGIC_ADDR, magic);
  if (magic == EEPROM_MAGIC) {
    float v = 0.0f;
    EEPROM.get(EEPROM_CALIB_ADDR, v);
    if (!isnan(v) && v > 0.1f && v < 100.0f) {
      g_calibMv = v;
      return;
    }
  }
  g_calibMv = 10.0f;
}

static void saveCalibration(float mv) {
  g_calibMv = mv;
  EEPROM.put(EEPROM_CALIB_ADDR, mv);
  const uint8_t magic = EEPROM_MAGIC;
  EEPROM.put(EEPROM_MAGIC_ADDR, magic);
}

// ============================================================================
//  LECTURE O2 + STABILITE
// ============================================================================
static void sampleO2() {
  const int16_t raw = ads.readADC_SingleEnded(ADS_O2_CHANNEL);
  g_lastVoltage = (float)raw * ADS_LSB_MV;

  g_currentO2 = (g_calibMv > 0.1f)
              ? (g_lastVoltage / g_calibMv) * CALIB_REF_PERCENT
              : 0.0f;
  if (g_currentO2 < 0.0f)  g_currentO2 = 0.0f;
  if (g_currentO2 > 99.9f) g_currentO2 = 99.9f;

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

// ============================================================================
//  CALCULS METIER
// ============================================================================
static int computeMOD(float fO2pct, float ppO2) {
  if (fO2pct <= 0.5f) return 0;
  const float mod = ((ppO2 / (fO2pct / 100.0f)) - 1.0f) * 10.0f;
  if (mod < 0.0f) return 0;
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
    case FIELD_HOUR:  g_eHour  = (uint8_t)((g_eHour  + delta + 24) % 24); break;
    case FIELD_MIN:   g_eMin   = (uint8_t)((g_eMin   + delta + 60) % 60); break;
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

static void displayRead() {
  char l1[20], l2[20];

  const int o2x10 = (int)(g_currentO2 * 10.0f + 0.5f);
  const char *tag = g_isStable ? "[OK]" : " ...";
  snprintf(l1, sizeof(l1), "O2:%2d.%d%% %s",
           o2x10 / 10, o2x10 % 10, tag);

  const int mod = computeMOD(g_currentO2, g_ppO2Target);
  const int px10 = (int)(g_ppO2Target * 10.0f + 0.5f);
  const DateTime now = rtc.now();
  snprintf(l2, sizeof(l2), "M:%3dm p%d.%d %02d:%02d",
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
  snprintf(l2, sizeof(l2), "%02d:%02d %02d/%02d/%04d",
           g_eHour, g_eMin, g_eDay, g_eMonth, g_eYear);

  lcd.setCursor(0, 0); lcdPrintPadded(l1);
  lcd.setCursor(0, 1); lcdPrintPadded(l2);
}

static void displayFeedback() {
  const char *l1 = "";
  const char *l2 = "";
  switch (g_feedback) {
    case FB_CALIB_OK:   l1 = "Calibration OK";  l2 = "Air = 20.9%";     break;
    case FB_CALIB_FAIL: l1 = "Calib. ERREUR";   l2 = "Tension faible";  break;
    case FB_PRINTED:    l1 = "Impression...";   l2 = "Etiquette OK";    break;
    case FB_UNSTABLE:   l1 = "Instable!";       l2 = "Attendez [OK]";   break;
    default: break;
  }
  lcd.setCursor(0, 0); lcdPrintPadded(l1);
  lcd.setCursor(0, 1); lcdPrintPadded(l2);
}

// ============================================================================
//  IMPRESSION TSPL (TSC TH240)
// ============================================================================
static void printLabel() {
  const DateTime now = rtc.now();
  const int mod  = computeMOD(g_currentO2, g_ppO2Target);
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

static void enterSetTime() {
  loadEditBufferFromRtc();
  g_field = FIELD_HOUR;
  g_mode  = MODE_SET_TIME;
  lcd.clear();
}

static void performCalibration() {
  const int16_t raw = ads.readADC_SingleEnded(ADS_O2_CHANNEL);
  const float mv = (float)raw * ADS_LSB_MV;
  if (mv > 1.0f && mv < 50.0f) {
    saveCalibration(mv);
    enterFeedback(FB_CALIB_OK);
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
  lcd.setCursor(0, 0); lcd.print("Analyseur O2");
  lcd.setCursor(0, 1); lcd.print("Init...");

  ads.setGain(GAIN_SIXTEEN);
  ads.begin();

  rtc.begin();
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  printer.begin(9600);

  loadCalibration();

  for (uint8_t i = 0; i < STABILITY_SAMPLES; i++) g_o2Buffer[i] = 0.0f;

  lcd.clear();
}

void loop() {
  const uint32_t now = millis();

  const ButtonEvent eL = updateButton(g_btnL);
  const ButtonEvent eC = updateButton(g_btnC);
  const ButtonEvent eR = updateButton(g_btnR);

  static uint32_t lastSample = 0;
  if (g_mode == MODE_READ && (now - lastSample) >= SAMPLE_INTERVAL_MS) {
    lastSample = now;
    sampleO2();
  }

  switch (g_mode) {

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
        if (g_isStable) {
          printLabel();
          enterFeedback(FB_PRINTED);
        } else {
          enterFeedback(FB_UNSTABLE);
        }
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
          g_mode = MODE_READ;
          lcd.clear();
        }
      }
      break;

    case MODE_FEEDBACK:
      if ((int32_t)(now - g_feedbackEnd) >= 0) {
        g_feedback = FB_NONE;
        g_mode     = MODE_READ;
        lcd.clear();
      }
      break;
  }

  static uint32_t lastDisplay = 0;
  if ((now - lastDisplay) >= DISPLAY_INTERVAL_MS) {
    lastDisplay = now;
    switch (g_mode) {
      case MODE_READ:     displayRead();     break;
      case MODE_SET_TIME: displaySetTime();  break;
      case MODE_FEEDBACK: displayFeedback(); break;
    }
  }
}
