/*
 * Analyseur O2 fixe (de paillasse, alimentation 220V AC) - ESP32-WROOM-32
 *
 * Capteur O2     : ADS1115 differentiel A0-A1 (gain x16)  I2C @0x48
 * Affichage      : LCD 1602 I2C                            @0x27
 * Horloge        : DS3231                                  I2C @0x68
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
 * WiFi AP        : SSID "O2-Sentry" - interface web sur 192.168.4.1
 *                  Reglage et verrouillage ppO2 a distance
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
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>

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
// Cellule O2 câblée en différentiel : (+) → A0, (−) → A1
// Rejette le bruit secteur (CMRR ~90 dB) — important sur alim 220V

// ============================================================================
//  WIFI AP
// ============================================================================
static const char*  WIFI_SSID = "O2-Sentry";
static const char*  WIFI_PASS = "plongee24";  // min 8 car. ou "" pour réseau ouvert
static AsyncWebServer server(80);
static DNSServer    dnsServer;

// Page HTML embarquée en flash (raw string literal)
static const char HTML_PAGE[] = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>O2 Sentry</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:monospace;background:#1a1a2e;color:#eee;padding:12px;max-width:420px;margin:auto}
h1{text-align:center;padding:12px;color:#4fc3f7;font-size:1.3em}
.card{background:#16213e;border:1px solid #0f3460;border-radius:10px;padding:16px;margin:10px 0}
.card h2{font-size:.85em;color:#4fc3f7;margin-bottom:12px;text-transform:uppercase;letter-spacing:.05em}
.big{font-size:2.8em;font-weight:bold;text-align:center;padding:10px 0}
.row{display:flex;justify-content:space-between;padding:5px 0;border-bottom:1px solid #0f3460}
.row:last-child{border-bottom:none}
.lbl{color:#888}.val{font-weight:bold}
.ok{color:#66bb6a}.warn{color:#ffa726}.err{color:#ef5350}
.sb{display:inline-block;padding:3px 12px;border-radius:12px;font-size:.8em}
.sb-ok{background:#1b5e20;color:#a5d6a7}
.sb-warn{background:#bf360c;color:#ffe0b2}
.opts{display:flex;gap:8px;margin:10px 0}
label.opt{flex:1;display:flex;align-items:center;justify-content:center;gap:6px;
  cursor:pointer;background:#0f3460;padding:12px;border-radius:8px;font-size:1em}
label.opt input{accent-color:#4fc3f7;width:16px;height:16px}
.lckrow{display:flex;align-items:center;gap:10px;margin:10px 0;
  padding:12px;background:#0f3460;border-radius:8px}
.lckrow input{width:22px;height:22px;accent-color:#ef5350;cursor:pointer;flex-shrink:0}
.lckrow small{color:#aaa;font-size:.8em}
button{width:100%;padding:13px;background:#0f3460;color:#4fc3f7;
  border:1px solid #4fc3f7;border-radius:8px;font-size:1em;
  font-family:monospace;cursor:pointer;margin-top:6px}
button:active{background:#1565c0}
.msg{text-align:center;margin-top:8px;min-height:20px;color:#66bb6a;font-size:.9em}
.foot{font-size:.7em;color:#444;text-align:center;margin-top:18px}
input[type=datetime-local]{width:100%;padding:10px;background:#0f3460;color:#eee;
  border:1px solid #4fc3f7;border-radius:8px;font-size:.95em;
  font-family:monospace;margin:10px 0;color-scheme:dark}
</style>
</head>
<body>
<h1>&#9889; O2 Sentry</h1>

<div class="card">
<h2>Mesure en direct</h2>
<div class="big" id="o2v">-- %</div>
<div style="text-align:center;margin-bottom:12px">
  <span class="sb sb-warn" id="stab">Stabilisation...</span>
</div>
<div class="row"><span class="lbl">MOD</span><span class="val" id="mod">--</span></div>
<div class="row"><span class="lbl">ppO2 ref</span><span class="val" id="ppo2c">--</span></div>
<div class="row"><span class="lbl">Temperature</span><span class="val" id="temp">--</span></div>
<div class="row"><span class="lbl">Calibration</span><span class="val" id="cal">--</span></div>
<div class="row"><span class="lbl">Pile RTC</span><span class="val" id="rtcs">--</span></div>
</div>

<div class="card">
<h2>Reglage ppO2</h2>
<div class="opts">
  <label class="opt"><input type="radio" name="pp" value="1.4" id="r14"> ppO2 = 1.4</label>
  <label class="opt"><input type="radio" name="pp" value="1.6" id="r16"> ppO2 = 1.6</label>
</div>
<div class="lckrow">
  <input type="checkbox" id="lck">
  <div><strong>Verrouiller</strong><br>
  <small>Desactive les boutons physiques GAUCHE / DROITE</small></div>
</div>
<button onclick="apply()">Appliquer</button>
<div class="msg" id="msg"></div>
</div>

<div class="card">
<h2>Calibration cellule O2</h2>
<div class="row"><span class="lbl">Derniere calibration</span><span class="val" id="caldt">--</span></div>
<div class="row"><span class="lbl">Cellule O2</span><span class="val" id="celv">--</span></div>
<div class="row"><span class="lbl">Tension live</span><span class="val" id="livemv">--</span></div>
<button onclick="doCalib()">Calibrer a l'air (20.9 %)</button>
<div class="msg" id="calmsg"></div>
</div>

<div class="card">
<h2>Date / Heure</h2>
<div class="row"><span class="lbl">RTC actuelle</span><span class="val" id="rtcv">--</span></div>
<input type="datetime-local" id="dtp">
<button onclick="setdt()">Appliquer</button>
<div class="msg" id="dtmsg"></div>
</div>

<div class="card">
<h2 style="display:flex;justify-content:space-between;align-items:center;cursor:pointer"
    onclick="tog()">
  Historique (50 dernieres analyses)
  <span id="arr" style="font-size:1.2em;color:#4fc3f7">&#9660;</span>
</h2>
<div id="hc">
<div id="hl"><em style="color:#666">Chargement...</em></div>
<div style="display:flex;gap:8px;margin-top:10px">
<button onclick="loadH()" style="flex:1">Rafraichir</button>
<a href="/history.csv" download style="flex:1;display:flex;align-items:center;justify-content:center;padding:13px;background:#0f3460;color:#4fc3f7;border:1px solid #4fc3f7;border-radius:8px;font-family:monospace;font-size:1em;text-decoration:none">Telecharger CSV</a>
</div>
</div>
</div>

<div class="foot">O2-Sentry &bull; WiFi AP &bull; 192.168.4.1</div>

<script>
var dirty=false,dtInited=false;
function upd(){
  fetch('/data').then(r=>r.json()).then(d=>{
    document.getElementById('o2v').textContent=d.o2.toFixed(1)+' %';
    var s=document.getElementById('stab');
    if(d.stable){s.textContent='OK Stable';s.className='sb sb-ok';}
    else{s.textContent='Stabilisation...';s.className='sb sb-warn';}
    document.getElementById('mod').textContent=d.mod+' m';
    document.getElementById('ppo2c').textContent=d.ppo2.toFixed(1)+(d.locked?' [VERR]':'');
    document.getElementById('temp').textContent=d.temp>-50?d.temp.toFixed(1)+' degC':'N/A';
    document.getElementById('cal').textContent=d.cal?'OK':'Non calibre';
    var cv=document.getElementById('celv');
    cv.textContent=d.cell+'% ('+d.cellmv.toFixed(1)+' mV)';
    cv.className='val '+(d.cell>=80?'ok':(d.cell>=50?'warn':'err'));
    document.getElementById('caldt').textContent=d.caldate||'Non calibre';
    document.getElementById('livemv').textContent=d.livemv.toFixed(3)+' mV';
    var rs=document.getElementById('rtcs');
    if(d.rtcok){rs.textContent='OK';rs.className='val ok';}
    else{rs.textContent='Pile HS !';rs.className='val err';}
    if(!dirty){
      document.getElementById(d.ppo2>=1.55?'r16':'r14').checked=true;
      document.getElementById('lck').checked=d.locked;
    }
    if(d.dt){
      var _p=d.dt.split('T'),_d=_p[0].split('-');
      document.getElementById('rtcv').textContent=_d[2]+'/'+_d[1]+'/'+_d[0]+' '+_p[1];
      if(!dtInited){document.getElementById('dtp').value=d.dt;dtInited=true;}
    }
  }).catch(function(){});
}
function apply(){
  var v=document.querySelector('input[name=pp]:checked');
  if(!v)return;
  var b='ppo2='+v.value+'&lock='+(document.getElementById('lck').checked?'1':'0');
  fetch('/ppo2',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})
    .then(r=>r.text()).then(function(){
      dirty=false;
      var m=document.getElementById('msg');
      m.textContent='Applique OK';
      setTimeout(function(){m.textContent='';},2500);
      upd();
    });
}
document.querySelectorAll('input[name=pp]').forEach(function(el){
  el.addEventListener('change',function(){dirty=true;});
});
document.getElementById('lck').addEventListener('change',function(){dirty=true;});
function setdt(){
  var v=document.getElementById('dtp').value;
  if(!v)return;
  var p=v.split('T'),dt=p[0].split('-'),tm=(p[1]||'00:00').split(':');
  var body='y='+dt[0]+'&mo='+dt[1]+'&d='+dt[2]+'&h='+tm[0]+'&mi='+tm[1];
  fetch('/settime',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})
    .then(r=>r.text()).then(function(){
      var m=document.getElementById('dtmsg');
      m.textContent='Heure reglée';
      setTimeout(function(){m.textContent='';},2500);
      dtInited=false;upd();
    });
}
function doCalib(){
  if(!confirm('Exposer la cellule a l\'air ambiant (20.9 %) et attendre la stabilisation.\n\nLancer la calibration ?'))return;
  fetch('/calibrate',{method:'POST'})
    .then(r=>r.json()).then(function(d){
      var m=document.getElementById('calmsg');
      if(d.ok){m.textContent='Calibration OK — '+d.mv.toFixed(1)+' mV, cellule '+d.cell+'%';m.className='msg ok';}
      else if(d.err==='instable'){m.textContent='Mesure instable — attendre [OK]';m.className='msg warn';}
      else{m.textContent='Echec — tension hors plage';m.className='msg err';}
      setTimeout(function(){m.textContent='';m.className='msg';},4000);
      upd();
    });
}
var hOpen=true;
function tog(){
  hOpen=!hOpen;
  document.getElementById('hc').style.display=hOpen?'block':'none';
  document.getElementById('arr').innerHTML=hOpen?'&#9660;':'&#9654;';
}
function loadH(){
  fetch('/history').then(r=>r.json()).then(function(d){
    var el=document.getElementById('hl');
    if(!d.length){el.innerHTML='<em style="color:#666">Aucune analyse enregistree</em>';return;}
    var h='<table style="width:100%;border-collapse:collapse;font-size:.82em">';
    h+='<tr style="color:#4fc3f7;border-bottom:1px solid #0f3460">';
    h+='<th style="text-align:left;padding:4px">Date/Heure</th>';
    h+='<th style="text-align:left;padding:4px">Plongeur</th>';
    h+='<th>O2</th><th>ppO2</th><th>MOD</th></tr>';
    d.forEach(function(r,i){
      var s=i===0?'color:#ffa726':'';
      h+='<tr style="border-top:1px solid #0f3460;'+s+'">';
      h+='<td style="padding:4px">'+r.date+' '+r.time+'</td>';
      h+='<td style="padding:4px">'+(r.name||'&mdash;')+'</td>';
      h+='<td style="text-align:center">'+r.o2.toFixed(1)+'%</td>';
      h+='<td style="text-align:center">'+r.ppo2.toFixed(1)+'</td>';
      h+='<td style="text-align:center">'+r.mod+'m</td></tr>';
    });
    h+='</table>';
    el.innerHTML=h;
  }).catch(function(){});
}
upd();
loadH();
setInterval(upd,2000);
</script>
</body>
</html>
)rawliteral";

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
static const float    PPO2_STD            = 1.4f;  // valeur standard (GAUCHE)
static const float    PPO2_MAX            = 1.6f;  // valeur max     (DROITE)
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
static const int EEPROM_SIZE             = 1300; // 16 header + 50×25 history
static const int EEPROM_CALIB_ADDR       = 0;
static const int EEPROM_INITIAL_ADDR     = 4;
static const int EEPROM_CALIB_TEMP_ADDR  = 8;
static const int EEPROM_MAGIC_ADDR       = 12;
static const int EEPROM_HIST_COUNT_ADDR  = 13;
static const int EEPROM_HIST_IDX_ADDR    = 14;
static const int EEPROM_SETTINGS_ADDR    = 15;  // bit0=ppO2(0→1.4,1→1.6) bit1=lock
static const int EEPROM_HIST_BASE        = 16;
static const int EEPROM_CALIB_DATE_ADDR  = 1266; // 5 octets : day,month,yearOff,hour,min
static const uint8_t EEPROM_MAGIC        = 0xA5;
static const uint8_t HIST_MAX            = 50;
static const uint8_t HIST_RECORD_SIZE    = 25;  // 11 données + 14 nom plongeur

// ============================================================================
//  OBJETS GLOBAUX
// ============================================================================
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
Adafruit_ADS1115  ads;
RTC_DS1307        rtc;
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
static bool         g_adsPresent     = false;
static bool         g_rtcPresent     = false;
static bool         g_rtcOK          = false;
static bool     g_calibDateValid = false;
static uint8_t  g_calibDay = 0, g_calibMonth = 0, g_calibYearOff = 0;
static uint8_t  g_calibHour = 0, g_calibMin = 0;

// ============================================================================
//  ETAT CAPTEUR / UTILISATEUR
// ============================================================================
static float    g_o2Buffer[STABILITY_SAMPLES];
static uint8_t  g_bufIdx          = 0;
static uint8_t  g_bufFilled       = 0;
static float    g_currentO2       = 0.0f;
static float    g_currentMv       = 0.0f;
static bool     g_isStable        = false;
static float    g_calibMv         = 10.0f;
static float    g_initialCalibMv  = 10.0f;
static float    g_calibTempC      = 20.0f;
static bool     g_calibValid      = false;
static float    g_ppO2Target      = 1.4f;
static bool     g_ppO2Locked      = false;  // verrouillage via interface web

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
      // chargement horodatage calibration
      {
        uint8_t cd = 0, cm = 0, cy = 0, ch = 0, cmi = 0;
        EEPROM.get(EEPROM_CALIB_DATE_ADDR,     cd);
        EEPROM.get(EEPROM_CALIB_DATE_ADDR + 1, cm);
        EEPROM.get(EEPROM_CALIB_DATE_ADDR + 2, cy);
        EEPROM.get(EEPROM_CALIB_DATE_ADDR + 3, ch);
        EEPROM.get(EEPROM_CALIB_DATE_ADDR + 4, cmi);
        if (cd >= 1 && cd <= 31 && cm >= 1 && cm <= 12 && ch <= 23 && cmi <= 59) {
          g_calibDay = cd; g_calibMonth = cm; g_calibYearOff = cy;
          g_calibHour = ch; g_calibMin = cmi;
          g_calibDateValid = true;
        }
      }
      g_calibValid     = true;
      return;
    }
  }
  g_calibMv        = 10.0f;
  g_initialCalibMv = 10.0f;
  g_calibTempC     = 20.0f;
  g_calibValid     = false;
}

static void saveSettings() {
  uint8_t b = 0;
  if (g_ppO2Target >= 1.55f) b |= 0x01;
  if (g_ppO2Locked)          b |= 0x02;
  EEPROM.put(EEPROM_SETTINGS_ADDR, b);
  const uint8_t magic = EEPROM_MAGIC;
  EEPROM.put(EEPROM_MAGIC_ADDR, magic);
  eepromCommit();
}

static void loadSettings() {
  uint8_t magic = 0;
  EEPROM.get(EEPROM_MAGIC_ADDR, magic);
  if (magic != EEPROM_MAGIC) return;
  uint8_t b = 0;
  EEPROM.get(EEPROM_SETTINGS_ADDR, b);
  g_ppO2Target = (b & 0x01) ? PPO2_MAX : PPO2_STD;
  g_ppO2Locked = (b & 0x02) != 0;
}

static DateTime g_cachedTime(2024, 1, 1, 0, 0, 0);
static uint32_t g_lastRtcMs  = 0;
static uint8_t  g_rtcErrCnt  = 0;

static DateTime rtcNow() {
  if (!g_rtcPresent) return g_cachedTime;
  const uint32_t m = millis();
  if (m - g_lastRtcMs < 1000) return g_cachedTime;  // cache 1 s
  g_lastRtcMs = m;
  const DateTime dt = rtc.now();
  if (dt.year() >= 2000 && dt.year() <= 2099) {
    g_cachedTime = dt;
    g_rtcErrCnt  = 0;
  } else if (++g_rtcErrCnt >= 5) {
    g_rtcPresent = false;
    Serial.println("RTC: trop d'erreurs consecutives, desactive");
  }
  return g_cachedTime;
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
  // sauvegarde horodatage calibration
  {
    const DateTime now = rtcNow();
    g_calibDay     = now.day();
    g_calibMonth   = now.month();
    g_calibYearOff = (now.year() >= 2024) ? (uint8_t)(now.year() - 2024) : 0;
    g_calibHour    = now.hour();
    g_calibMin     = now.minute();
    g_calibDateValid = true;
    EEPROM.put(EEPROM_CALIB_DATE_ADDR,     g_calibDay);
    EEPROM.put(EEPROM_CALIB_DATE_ADDR + 1, g_calibMonth);
    EEPROM.put(EEPROM_CALIB_DATE_ADDR + 2, g_calibYearOff);
    EEPROM.put(EEPROM_CALIB_DATE_ADDR + 3, g_calibHour);
    EEPROM.put(EEPROM_CALIB_DATE_ADDR + 4, g_calibMin);
  }
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

struct __attribute__((packed)) HistRecord {
  uint8_t  yearOffset;
  uint8_t  month;
  uint8_t  day;
  uint8_t  hour;
  uint8_t  minute;
  uint16_t fO2_x10;
  uint16_t ppO2_x10;
  uint16_t mod;
  char     name[NAME_MAX_LEN];  // 14 octets, sans null-terminateur
};
static_assert(sizeof(HistRecord) == HIST_RECORD_SIZE,
              "HistRecord layout must be 25 bytes for EEPROM addressing");

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
  if (!g_adsPresent) return;
  const int16_t raw = ads.readADC_Differential_0_1();
  const float voltage = (float)raw * ADS_LSB_MV;
  g_currentMv = voltage;

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
  DateTime n = rtcNow();
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
    const DateTime now = rtcNow();
    // 'L' si ppO2 verrouillé, ' ' sinon — reste 16 caractères
    snprintf(l2, sizeof(l2), "M:%3d p%d.%d%c%02d:%02d",
             mod, px10 / 10, px10 % 10,
             g_ppO2Locked ? 'L' : ' ',
             now.hour(), now.minute());
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
  const DateTime now = rtcNow();
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
           "TEXT 20,225,\"2\",0,1,1,\"%02d/%02d/%04d  %02d:%02d\"",
           now.day(), now.month(), now.year(),
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
  memset(r.name, 0, NAME_MAX_LEN);
  memcpy(r.name, g_currentName, strnlen(g_currentName, NAME_MAX_LEN));
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
  if (!g_adsPresent) { enterFeedback(FB_NOT_CALIB); return; }
  const int16_t raw = ads.readADC_Differential_0_1();
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
//  SERVEUR WEB — handlers
// ============================================================================
static void webHandleRoot(AsyncWebServerRequest *request) {
  request->send(200, "text/html", HTML_PAGE);
}

static void webHandleData(AsyncWebServerRequest *request) {
  const int mod = g_calibValid ? computeMOD(g_currentO2, g_ppO2Target) : 0;
  const DateTime now = rtcNow();
  const uint8_t cellPct = cellLifePercent();
  char caldate[20] = "";
  if (g_calibDateValid) {
    snprintf(caldate, sizeof(caldate), "%02d/%02d/%04d %02d:%02d",
             g_calibDay, g_calibMonth, (int)g_calibYearOff + 2024,
             g_calibHour, g_calibMin);
  }
  char json[320];
  snprintf(json, sizeof(json),
    "{\"o2\":%.1f,\"stable\":%s,\"mod\":%d,\"ppo2\":%.1f,"
    "\"locked\":%s,\"temp\":%.1f,\"cal\":%s,"
    "\"dt\":\"%04d-%02d-%02dT%02d:%02d\","
    "\"rtcok\":%s,\"cell\":%d,\"cellmv\":%.1f,\"livemv\":%.3f,\"caldate\":\"%s\"}",
    g_currentO2,
    g_isStable  ? "true" : "false",
    mod,
    g_ppO2Target,
    g_ppO2Locked ? "true" : "false",
    g_currentTempC,
    g_calibValid ? "true" : "false",
    now.year(), now.month(), now.day(),
    now.hour(), now.minute(),
    g_rtcOK     ? "true" : "false",
    cellPct,
    g_calibMv,
    g_currentMv,
    caldate
  );
  request->send(200, "application/json", json);
}

static void webHandleSetTime(AsyncWebServerRequest *request) {
  auto ip = [&](const char *k) -> int {
    return request->hasParam(k, true) ? request->getParam(k, true)->value().toInt() : -1;
  };
  const int y  = ip("y");
  const int mo = ip("mo");
  const int d  = ip("d");
  const int h  = ip("h");
  const int mi = ip("mi");
  if (y >= 2024 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31
      && h >= 0 && h <= 23 && mi >= 0 && mi <= 59 && g_rtcPresent) {
    rtc.adjust(DateTime(y, mo, d, h, mi, 0));
    g_rtcOK = true;
  }
  request->send(200, "text/plain", "OK");
}

static void webHandleCalibrate(AsyncWebServerRequest *request) {
  if (!g_adsPresent) {
    request->send(200, "application/json", "{\"ok\":false,\"err\":\"ads absent\"}");
    return;
  }
  if (!g_isStable) {
    request->send(200, "application/json", "{\"ok\":false,\"err\":\"instable\"}");
    return;
  }
  const int16_t raw = ads.readADC_Differential_0_1();
  const float   mv  = (float)raw * ADS_LSB_MV;
  if (mv > 1.0f && mv < 50.0f) {
    saveCalibration(mv, g_currentTempC);
    resetStabilityBuffer();
    const uint8_t pct = cellLifePercent();
    if (pct < CELL_WARN_PERCENT) enterFeedback(FB_CELL_WEAK);
    else                          enterFeedback(FB_CALIB_OK);
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"mv\":%.1f,\"cell\":%d}", mv, pct);
    request->send(200, "application/json", buf);
  } else {
    enterFeedback(FB_CALIB_FAIL);
    request->send(200, "application/json", "{\"ok\":false,\"err\":\"tension\"}");
  }
}

static void webHandleHistoryCsv(AsyncWebServerRequest *request) {
  const uint8_t c = histCount();
  String csv = "Date,Heure,Plongeur,O2 %,ppO2,MOD m\r\n";
  for (uint8_t i = 0; i < c; i++) {
    HistRecord r;
    if (!histRead(i, r)) continue;
    char nm[NAME_MAX_LEN + 1] = {0};
    memcpy(nm, r.name, NAME_MAX_LEN);
    char buf[96];
    snprintf(buf, sizeof(buf), "%02d/%02d/%04d,%02d:%02d,%s,%.1f,%.1f,%d\r\n",
             r.day, r.month, (int)r.yearOffset + 2024,
             r.hour, r.minute,
             nm,
             r.fO2_x10  / 10.0f,
             r.ppO2_x10 / 10.0f,
             (int)r.mod);
    csv += buf;
  }
  AsyncWebServerResponse *resp = request->beginResponse(200, "text/csv", csv);
  resp->addHeader("Content-Disposition", "attachment; filename=\"historique.csv\"");
  request->send(resp);
}

static void webHandleHistory(AsyncWebServerRequest *request) {
  const uint8_t c = histCount();
  String json = "[";
  for (uint8_t i = 0; i < c; i++) {
    HistRecord r;
    if (!histRead(i, r)) continue;
    if (i > 0) json += ",";
    char nm[NAME_MAX_LEN + 1] = {0};
    memcpy(nm, r.name, NAME_MAX_LEN);
    char buf[160];
    snprintf(buf, sizeof(buf),
      "{\"date\":\"%02d/%02d/%02d\",\"time\":\"%02d:%02d\","
      "\"o2\":%.1f,\"ppo2\":%.1f,\"mod\":%d,\"name\":\"%s\"}",
      r.day, r.month, (r.yearOffset + 24) % 100,
      r.hour, r.minute,
      r.fO2_x10  / 10.0f,
      r.ppO2_x10 / 10.0f,
      (int)r.mod,
      nm
    );
    json += buf;
  }
  json += "]";
  request->send(200, "application/json", json);
}

static void webHandleSetPpO2(AsyncWebServerRequest *request) {
  if (request->hasParam("ppo2", true)) {
    const float v = request->getParam("ppo2", true)->value().toFloat();
    if (v >= 1.35f && v <= 1.45f)  g_ppO2Target = PPO2_STD;
    else if (v >= 1.55f)            g_ppO2Target = PPO2_MAX;
  }
  if (request->hasParam("lock", true)) {
    g_ppO2Locked = (request->getParam("lock", true)->value() == "1");
  }
  saveSettings();
  request->send(200, "text/plain", "OK");
}

// ============================================================================
//  SETUP / LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== O2 Sentry demarrage ===");

  // ── WiFi + DNS + Web EN PREMIER ────────────────────────────────────────────
  // Demarrage avant l'I2C pour rester accessible meme si un peripherique bloque
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(
    IPAddress(192, 168, 4, 1),
    IPAddress(192, 168, 4, 1),
    IPAddress(255, 255, 255, 0)
  );
  const bool apOK = WiFi.softAP(WIFI_SSID, WIFI_PASS, 6, false, 4);
  Serial.printf("WiFi AP %s  IP=%s\n", apOK ? "OK" : "ECHEC",
                WiFi.softAPIP().toString().c_str());

  delay(200);  // laisse le temps a l'interface AP d'etre prete avant DNS
  dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));

  server.on("/",        HTTP_GET,  webHandleRoot);
  server.on("/data",    HTTP_GET,  webHandleData);
  server.on("/history", HTTP_GET,  webHandleHistory);
  server.on("/ppo2",    HTTP_POST, webHandleSetPpO2);
  server.on("/settime", HTTP_POST, webHandleSetTime);
  server.on("/calibrate",   HTTP_POST, webHandleCalibrate);
  server.on("/history.csv", HTTP_GET,  webHandleHistoryCsv);
  // Portail captif : repond aux checks Android (/generate_204) et iOS
  server.on("/generate_204",              HTTP_ANY, [](AsyncWebServerRequest *r){ r->send(204, "text/plain", ""); });
  server.on("/hotspot-detect.html",       HTTP_ANY, [](AsyncWebServerRequest *r){ r->send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"); });
  server.on("/library/test/success.html", HTTP_ANY, [](AsyncWebServerRequest *r){ r->send(200, "text/html", "Success"); });
  server.on("/connecttest.txt",           HTTP_ANY, [](AsyncWebServerRequest *r){ r->send(200, "text/plain", "Microsoft Connect Test"); });
  server.on("/ncsi.txt",                  HTTP_ANY, [](AsyncWebServerRequest *r){ r->send(200, "text/plain", "Microsoft NCSI"); });
  server.onNotFound([](AsyncWebServerRequest *r){ r->send(200, "text/html", HTML_PAGE); });
  server.begin();
  Serial.println("Serveur web demarre - http://192.168.4.1");

  // ── Boutons ─────────────────────────────────────────────────────────────────
#if BUTTON_MODE == 0
  pinMode(PIN_BTN_LEFT,   INPUT);
  pinMode(PIN_BTN_CENTER, INPUT);
  pinMode(PIN_BTN_RIGHT,  INPUT);
#endif

  // ── I2C ─────────────────────────────────────────────────────────────────────
  Wire.begin();
  Wire.setTimeout(15); // timeout I2C 15 ms

  // Scan I2C : liste tous les peripheriques trouves
  Serial.println("Scan I2C :");
  int i2cFound = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  0x%02X trouve", addr);
      if (addr == 0x27 || addr == 0x3F) Serial.print(" <- LCD PCF8574");
      if (addr == 0x48)                  Serial.print(" <- ADS1115");
      if (addr == 0x68)                  Serial.print(" <- DS1307 RTC");
      if (addr == 0x24 || addr == 0x48 || addr == 0x29) Serial.print(" <- PN532?");
      Serial.println();
      i2cFound++;
    }
  }
  if (i2cFound == 0) Serial.println("  Aucun peripherique I2C detecte !");
  Serial.println("Scan I2C termine.");

  lcd.init();
  lcd.backlight();
  lcd.clear();

  ads.setGain(GAIN_SIXTEEN);
  g_adsPresent = ads.begin();
  Serial.printf("ADS1115 : %s\n", g_adsPresent ? "OK" : "ABSENT - verifier cablage I2C @0x48");

  g_rtcPresent = rtc.begin();
  Serial.printf("RTC DS1307 : %s\n", g_rtcPresent ? "OK" : "ABSENT - verifier cablage");
  if (g_rtcPresent) {
    g_rtcOK = rtc.isrunning();
    if (!g_rtcOK) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // ── UART2 ───────────────────────────────────────────────────────────────────
  printer.begin(9600, SERIAL_8N1, PIN_PRINTER_RX, PIN_PRINTER_TX);

  // ── DS18B20 ─────────────────────────────────────────────────────────────────
  tempSensor.begin();
  tempSensor.setWaitForConversion(false);
  g_tempPresent = (tempSensor.getDeviceCount() > 0);
  if (g_tempPresent) {
    tempSensor.requestTemperatures();
    g_lastTempReq    = millis();
    g_tempReqPending = true;
  }

  // ── LED ─────────────────────────────────────────────────────────────────────
  pixel.begin();
  pixel.setBrightness(LED_BRIGHTNESS);
  pixel.clear();
  pixel.show();

  // ── PN532 ───────────────────────────────────────────────────────────────────
  pn532.begin();
  const uint32_t fw = pn532.getFirmwareVersion();
  if (fw != 0) {
    pn532.SAMConfig();
    g_pn532Present = true;
  }

  // ── EEPROM ──────────────────────────────────────────────────────────────────
  EEPROM.begin(EEPROM_SIZE);
  loadCalibration();
  loadSettings();
  resetStabilityBuffer();

  g_mode           = MODE_SPLASH;
  g_splashEnd      = millis() + (g_calibValid ? SPLASH_MS : SPLASH_NOCAL_MS);
  g_pendingSetTime = !g_rtcOK;
  displaySplash();

  Serial.println("Setup termine.");
}

void loop() {
  const uint32_t now = millis();
  dnsServer.processNextRequest();
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
      if (!g_ppO2Locked) {
        if (eL == BTN_SHORT) { g_ppO2Target = PPO2_STD; saveSettings(); }
        if (eR == BTN_SHORT) { g_ppO2Target = PPO2_MAX; saveSettings(); }
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
          if (g_rtcPresent)
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
