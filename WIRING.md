# Guide de câblage - O2-SENTRY-FIX (ESP32-WROOM-32)

Schéma complet de connexion des modules à l'ESP32-WROOM-32.

---

## Vue d'ensemble des pins utilisées

| Pin ESP32 | Direction | Usage |
|-----------|-----------|-------|
| G21 (SDA) | I/O | Bus I2C (LCD + ADS1115 + RTC + PN532) |
| G22 (SCL) | I/O | Bus I2C |
| G32 (T9)  | Input | Bouton GAUCHE (TTP223 **ou** touch natif) |
| G33 (T8)  | Input | Bouton CENTRE (TTP223 **ou** touch natif) |
| G27 (T7)  | Input | Bouton DROITE (TTP223 **ou** touch natif) |
| G4        | I/O | OneWire DS18B20 (capteur de température, **optionnel**) |
| G5        | Output | LED RGB WS2812B (data) — voir note niveau 3.3 V |
| G18       | Input | PN532 IRQ (lecteur RFID, **optionnel**) |
| G19       | Output | PN532 RESET (lecteur RFID, **optionnel**) |
| G16       | Input | RX UART2 (vers TX imprimante TSC) |
| G17       | Output | TX UART2 (vers RX imprimante TSC) |
| 3V3       | Power | Alimentation modules 3.3 V |
| 5V (VIN)  | Power | Alimentation modules 5 V (si USB/externe) |
| GND       | Power | Masse commune |

> **BUTTON_MODE** : G32/G33/G27 sont des broches touch-capables (T9/T8/T7) **et** des GPIO standard. Le câblage est identique pour les deux modes ; seul le `#define BUTTON_MODE` dans `platformio.ini` change.

---

## Bus I2C (LCD + ADS1115 + RTC + PN532)

Les quatre modules partagent le même bus I2C sur **G21 (SDA) / G22 (SCL)**. Chaque module a une adresse unique, aucun conflit.

```
 ESP32-WROOM-32
 ┌──────────────┐
 │  G21 (SDA)  ├───┬──────────┬──────────┬───────────┐
 │  G22 (SCL)  ├───┼───┬──────┼───┬──────┼───┬───────┼───┐
 │  3V3        ├───┼───┼──────┼───┼──────┼───┼───────┼───┼──┐
 │  GND        ├───┼───┼──────┼───┼──────┼───┼───────┼───┼──┼──┐
 └─────────────┘   │   │      │   │      │   │       │   │  │  │
                  SDA SCL    SDA SCL    SDA SCL      SDA SCL │  │
              ┌────┴───┴──┐┌─┴───┴──┐┌─┴───┴──────┐┌─┴───┴──┤  │
              │ LCD 1602  ││ADS1115 ││ RTC DS3231 ││ PN532  │  │
              │ I2C @0x27 ││ @0x48  ││   @0x68    ││ @0x24  │  │
              └───────────┘└────────┘└────────────┘└────────┘  │
                              VCC──────────────────────────────┘  │
                              GND─────────────────────────────────┘
```

**Notes** :
- Les modules I2C courants intègrent leurs **résistances de pull-up**. Inutile d'en ajouter.
- Si bus instable : un seul module doit avoir ses pull-ups actifs.
- Adresse LCD : généralement **0x27**, parfois **0x3F** (dépend du PCF8574). Modifier `LCD_ADDR` dans [src/main.cpp](src/main.cpp) si nécessaire.
- Le PN532 doit être configuré en mode **I2C** via ses switches DIP (voir section dédiée).

---

## ADS1115 + Cellule O2

La cellule galvanique produit une tension faible (9–13 mV à l'air). Le gain `GAIN_SIXTEEN` de l'ADS1115 (±256 mV, LSB = 7.8 µV) est optimal.

```
                      ADS1115
                    ┌─────────┐
     Cellule O2     │         │
   ┌───────────┐    │   A0 ◄──┼─── (+) cellule
   │    (+) ●──┼────┤         │
   │    (−) ●──┼────┤  GND ◄──┼─── (−) cellule
   └───────────┘    │         │
                    │   VDD ──┼─── 3V3 ESP32
                    │   GND ──┼─── GND ESP32
                    │   SDA ──┼─── G21 ESP32
                    │   SCL ──┼─── G22 ESP32
                    │  ADDR ──┼─── GND (→ adresse 0x48)
                    └─────────┘
```

> L'ADS1115 fonctionne en **3.3 V** — ne pas le connecter au 5V de l'ESP32.

**Polarité cellule O2** : fil **positif** (souvent rouge) sur **A0**, négatif sur **GND**. Polarité inversée → lecture 0 %.

---

## Boutons — deux modes de câblage

### Mode 0 : TTP223 (BUTTON_MODE=0, `env:esp32-ttp223`)

Les modules TTP223 pilotent la sortie à **HIGH quand touché** (configuration par défaut). Le code utilise `digitalRead(pin) == HIGH`.

```
                 TTP223 (×3)
               ┌──────────┐
               │  VCC ────┼── 3V3 ESP32  (ou 5V — modules tolèrent 2.5–5.5V)
               │  GND ────┼── GND ESP32
               │  I/O ────┼── G32 (GAUCHE) / G33 (CENTRE) / G27 (DROITE)
               └──────────┘
                (pad capacitif au dos)
```

**Jumpers au dos du TTP223** :
- Par défaut : sortie **HIGH quand touché** (attendu par le firmware)
- Jumper `A` inversé → sortie LOW quand touché : **ne pas faire**
- Jumper `B` → mode toggle : **laisser ouvert**, le firmware gère l'état

### Mode 1 : Touch natif ESP32 (BUTTON_MODE=1, `env:esp32-touch`)

Aucun module externe nécessaire. Connecter une **plaquette métallique** (pad, vis, ou feuille de cuivre) directement sur G32/G33/G27.

```
  G32 ──── pad métallique GAUCHE
  G33 ──── pad métallique CENTRE
  G27 ──── pad métallique DROITE
```

La sensibilité est réglée par `TOUCH_THRESHOLD` dans [src/main.cpp](src/main.cpp) (défaut : **40** — abaisser si trop sensible, monter si sous-sensible).

> Utiliser du **fil blindé** si le câble entre l'ESP32 et le pad est long (>10 cm), pour éviter les faux déclenchements. La tresse du blindage va à GND, **pas** sur le fil signal.

**Matériaux de pad utilisables** :
- Scotch cuivre (adhésif conducteur, hobby électronique)
- Rondelle/vis en acier inoxydable insérée dans le boîtier
- PCB avec plan de cuivre

---

## Capteur de température DS18B20 (optionnel)

Le firmware détecte automatiquement le DS18B20 au démarrage. Si absent, aucune compensation n'est appliquée. Si présent, la mesure O2 est corrigée de ~0.3 %/°C.

```
          DS18B20 (TO-92, vu de face, plat vers soi)
               ┌─────┐
               │  •  │
               │ DS  │
               │18B20│
               └┬─┬─┬┘
                │ │ │
               GND│ VDD
                  DQ
                  │
          4.7 kΩ  │      (pull-up OBLIGATOIRE)
             ┌────┼────── 3V3 ESP32
             │    │
             └────┴────── G4 ESP32
                  │
                 GND ─── GND ESP32
                 VDD ─── 3V3 ESP32
```

**Points importants** :
- Pull-up **4.7 kΩ** entre DQ et VCC : **obligatoire** (OneWire = collecteur ouvert)
- Placer le DS18B20 **au plus près de la cellule O2** pour une compensation pertinente
- Câblage **3 fils** (non parasite) : VDD séparé, pas le mode 2 fils
- La température au moment de la calibration est sauvegardée en EEPROM

---

## Lecteur RFID PN532 (optionnel)

Partage le bus I2C — adresse 0x24, aucun conflit avec LCD (0x27), ADS1115 (0x48), RTC (0x68).

### Configuration du module

| Mode | SEL0 | SEL1 |
|------|------|------|
| **I2C** (utilisé ici) | OFF | ON |
| HSU (UART)            | OFF | OFF |
| SPI                   | ON  | OFF |

→ Vérifier que les switches DIP sont sur **I2C**.

### Câblage

```
       PN532
   ┌────────────┐
   │ VCC ───────┼── 3V3 ESP32  (la plupart des modules clones acceptent 3.3 V)
   │ GND ───────┼── GND ESP32
   │ SDA ───────┼── G21 ESP32  (partage I2C)
   │ SCL ───────┼── G22 ESP32
   │ IRQ ───────┼── G18 ESP32  (signal "carte détectée")
   │ RSTO/RSTPDN┼── G19 ESP32  (reset hardware)
   └────────────┘
```

> Si le module nécessite 5V (certaines versions avec régulateur absent), alimenter depuis le pin **5V/VIN** de l'ESP32.

### Format des badges (Mifare Classic 1K)

| Paramètre | Valeur |
|-----------|--------|
| Bloc lu | **4** (premier bloc de données du secteur 1) |
| Authentification | Clé A par défaut `FF FF FF FF FF FF` |
| Format | Texte ASCII brut, null-terminé |
| Longueur max | 14 caractères |

### Encodage d'un badge (Android)

1. Installer **MIFARE Classic Tool** (Google Play, gratuit)
2. "Write tag" → "Write block"
3. Approcher la carte
4. Secteur 1, bloc 0 (= bloc 4 absolu), clé A par défaut
5. Saisir le nom en ASCII (l'app propose une conversion automatique)
6. Compléter avec `00` jusqu'à 16 octets

### Workflow RFID

- **Sans badge** : appui CENTRE → étiquette avec `Plongeur: ____________` à remplir au stylo
- **Badge + analyse stable** : impression immédiate avec le nom
- **Badge + analyse instable** : mode armé 30 s, LED bleue clignotante, impression auto dès `[OK]`
- **Annulation** : appui CENTRE pendant l'attente
- **Anti-double** : retirer le badge puis le repasser pour une nouvelle impression

---

## LED RGB WS2812B

### ⚠️ Problème de niveau logique

L'ESP32 fonctionne en **3.3 V** mais les WS2812B standard nécessitent un signal data entre **3.5 V et 5 V**. Avec 3.3 V, la LED peut fonctionner dans certains cas (chance), mais c'est hors spec et peu fiable.

**Solution recommandée : level-shifter 74HCT245**

```
                        74HCT245
  G5 ESP32 ──────► A1 │         │ B1 ──────► DIN WS2812B
              DIR = L  │         │
  3V3 ───────────  VCC │         │ VCC ─────── 5V
  GND ───────────  GND │         │ GND ─────── GND
                        └─────────┘
```

> Une seule sortie utilisée (A1→B1). Les 7 autres peuvent être laissées non-connectées ou à GND.

**Alternative** : une diode Schottky (ex. 1N4148) en série sur la ligne 5V du WS2812B abaisse VCC à ~4.3 V, ce qui abaisse aussi le seuil de réception et rend la donnée 3.3 V plus fiable. Moins propre que le 74HCT245 mais fonctionnel en dépannage.

### Câblage avec level-shifter

```
   G5 ESP32 ────► 74HCT245 ────► DIN WS2812B
   5V        ─────────────────── VCC WS2812B
   GND       ─────────────────── GND WS2812B
```

### Câblage direct (test/prototype, pas garanti)

```
   G5 ESP32 ────────────────────► DIN WS2812B
   5V        ──────────────────── VCC WS2812B
   GND       ──────────────────── GND WS2812B
```

> Une seule LED suffit. Si strip, seule la LED d'index 0 est pilotée.

### Codes couleur

| Couleur | Comportement | Signification |
|---------|--------------|---------------|
| ⚪ Blanc tamisé | fixe | Splash de démarrage |
| 🟠 Orange | fixe | Mesure en cours de stabilisation |
| 🟢 Vert | fixe | Stable + calibré, prêt à imprimer |
| 🔵 Bleu | clignotant 1 Hz | Badge détecté, mode armé (attente stab.) |
| 🟣 Violet | fixe (1.5 s) | Impression en cours |
| 🟡 Jaune | fixe | Mode réglage de l'heure |
| 🔵 Cyan | fixe | Consultation de l'historique |
| 🔴 Rouge | clignotant | Erreur (non calibré, cellule usée, badge invalide) |

> Luminosité : `LED_BRIGHTNESS = 80/255` dans [src/main.cpp](src/main.cpp). Ajustable.

---

## Imprimante TSC TH240 — UART2 (HardwareSerial)

L'ESP32 utilise **Serial2 (UART2)** sur G16 (RX) / G17 (TX) — pas de SoftwareSerial.

### Cas 1 : Imprimante en TTL 3.3 V ou 5 V tolérant

```
  G17 ESP32 (TX) ────► RX imprimante
  G16 ESP32 (RX) ◄──── TX imprimante
  GND ESP32       ──── GND imprimante
```

> Les niveaux logiques de l'ESP32 sont **3.3 V**. Si l'imprimante attend du TTL 5V, ajouter un level-shifter bidirectionnel (BSS138 ou similaire) sur les deux lignes.

### Cas 2 : Imprimante en RS-232 (±12V, cas fréquent sur TH240)

**MAX3232 obligatoire** (version 3.3 V du MAX232) pour éviter de détruire l'ESP32.

```
  G17 ESP32 (TX) ──► T1IN  │          │ T1OUT ──► Pin 2 DB9 (RX imprimante)
  G16 ESP32 (RX) ◄── R1OUT │ MAX3232  │ R1IN  ◄── Pin 3 DB9 (TX imprimante)
  3V3 ESP32      ──  VCC   │          │
  GND ESP32      ──  GND   │          │ GND   ──  Pin 5 DB9 (masse)
                            └──────────┘
```

> Utiliser **MAX3232** (3.3 V) et **non** MAX232 (5 V) — l'alimentation est différente.

**Alimentation imprimante** : la TH240 a son **propre bloc 24V**. Ne jamais tenter de l'alimenter depuis l'ESP32.

---

## Alimentation 220V AC

Appareil fixe de paillasse alimenté sur le **secteur 220V AC**.

### Schéma d'alimentation

```
  Prise 220V AC
       │
   ┌───┴────┐
   │ Fusible│  1 A temporisé (T)
   └───┬────┘
       │
   ┌───┴────────┐
   │Interrupteur│  bipolaire — coupe phase + neutre
   └───┬────────┘
       │
       ├── Bloc secteur 5V DC 2A ──► USB ou pin 5V ESP32
       │   (ou 9V → jack VIN avec régulateur interne)
       │
       └── Imprimante TSC TH240 (bloc 24V d'origine, non modifié)
```

> **ESP32 vs Nano** : l'ESP32 accepte 3.3–3.6 V sur pin 3V3, ou 5V sur pin 5V (via régulateur LDO interne), ou 5–12V sur pin VIN (si régulateur embarqué sur la carte de dev). Vérifier la fiche de ta carte de dev. Un bloc 5V/2A USB type C est le plus simple.

### Câblage 220V

```
  Phase (marron) ──► Fusible ──► Interrupteur ──┬── Bloc 5V DC
                                                 │
  Neutre (bleu) ───────────────► Interrupteur ──┼── Bloc 5V DC
                                                 │
                                                 └── Imprimante (via prise)
  Terre (vert/jaune) ──► boîtier métallique (si applicable)
```

### Sécurité 220V — ⚠️ OBLIGATOIRE

- Toutes connexions 220V **sous gaine thermo ou domino fermé**, aucun contact exposé
- **Distance ≥ 4 mm** entre pistes 220V et basse tension (règle "creepage")
- **Boîtier ABS ignifuge** ou boîtier métallique relié à la terre
- **Décharge de traction** sur le câble secteur
- Bloc secteur marqué **CE** et double isolation (symbole ⊡)

---

## Plan de masse

Tous les GND reliés en **étoile** sur un point commun (barrette bus ou pin GND ESP32). Éviter les masses en série : les boucles créent des décalages de tension mesurables sur la cellule O2.

---

## Checklist avant premier allumage

- [ ] Polarité cellule O2 : (+) sur A0, (−) sur GND de l'ADS1115
- [ ] Jumper ADDR ADS1115 → **GND** (adresse 0x48)
- [ ] DIP switches PN532 → **I2C** (SEL0 OFF, SEL1 ON)
- [ ] Pile CR2032 dans le RTC (maintien de l'heure hors tension)
- [ ] Tous les GND reliés entre eux
- [ ] Bloc secteur imprimante branché séparément
- [ ] Adresse LCD vérifiée (0x27 ou 0x3F) avec scanner I2C
- [ ] (Si DS18B20) pull-up **4.7 kΩ** entre DQ et 3V3 bien présent
- [ ] (Si WS2812B) level-shifter 74HCT245 câblé OU câblage direct accepté en prototype
- [ ] (Si BUTTON_MODE=1) pads touch bien isolés mécaniquement (pas de contact avec boîtier GND)
- [ ] TTP223 : jumper A sur **HIGH actif** (par défaut), jumper B ouvert
- [ ] `env:esp32-ttp223` ou `env:esp32-touch` sélectionné dans PlatformIO selon le montage

---

## Scanner I2C (debug)

```cpp
#include <Wire.h>
void setup() {
  Wire.begin(21, 22);  // SDA=G21, SCL=G22
  Serial.begin(115200);
  for (byte a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.print("Trouve: 0x");
      Serial.println(a, HEX);
    }
  }
}
void loop() {}
```

Résultat attendu : `0x24` (PN532, si présent), `0x27` (LCD), `0x48` (ADS1115), `0x68` (RTC).
