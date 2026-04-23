# Guide de câblage - O2-SENTRY-FIX

Schéma complet de connexion des modules à l'Arduino Nano.

---

## Vue d'ensemble des pins utilisées

| Pin Nano | Direction | Usage |
|----------|-----------|-------|
| A4 (SDA) | I/O | Bus I2C (LCD + ADS1115 + RTC) |
| A5 (SCL) | I/O | Bus I2C |
| D2 | Input | Bouton GAUCHE (TTP223) |
| D3 | Input | Bouton CENTRE (TTP223) |
| D4 | Input | Bouton DROITE (TTP223) |
| D10 | Input | RX SoftwareSerial (vers TX imprimante) |
| D11 | Output | TX SoftwareSerial (vers RX imprimante) |
| 5V | Power | Alimentation modules logiques |
| 3.3V | Power | (optionnel — TTP223 peut être en 3.3V) |
| GND | Power | Masse commune |
| VIN | Power | Entrée batterie 7-12V |

---

## Bus I2C (LCD + ADS1115 + RTC)

Les trois modules partagent le même bus I2C. Chaque module a une **adresse unique** et écoute son propre trafic.

```
 Arduino Nano
 ┌──────────┐
 │       A4 ├─────┬──────────┬──────────┐
 │   (SDA)  │     │          │          │
 │       A5 ├─────┼────┬─────┼────┬─────┼────┐
 │   (SCL)  │     │    │     │    │     │    │
 │       5V ├─────┼────┼─────┼────┼─────┼────┼────┐
 │      GND ├─────┼────┼─────┼────┼─────┼────┼────┼────┐
 └──────────┘     │    │     │    │     │    │    │    │
                 SDA  SCL   SDA  SCL   SDA  SCL  VCC  GND
              ┌───┴────┴──┐┌─┴────┴──┐┌─┴────┴───────────┐
              │ LCD 1602  ││ ADS1115 ││ RTC DS3231       │
              │ I2C @0x27 ││ @0x48   ││ @0x68            │
              └───────────┘└─────────┘└──────────────────┘
```

**Notes importantes** :
- Les modules I2C courants intègrent déjà leurs **résistances de pull-up** (4.7 kΩ ou 10 kΩ). Inutile d'en ajouter.
- Si bus instable : vérifier qu'**un seul** module a ses pull-ups soudés, ou couper les jumpers sur les autres.
- Adresse LCD : généralement **0x27**, parfois **0x3F** (dépend du PCF8574). Si rien n'affiche, changer `LCD_ADDR` dans [src/main.cpp](src/main.cpp).

---

## ADS1115 + Cellule O2

La cellule galvanique produit une tension faible (9–13 mV à l'air). Le gain `GAIN_SIXTEEN` de l'ADS1115 (±256 mV, LSB = 7.8 µV) est optimal.

```
                      ADS1115
                    ┌─────────┐
     Cellule O2     │         │
   ┌───────────┐    │   A0 ◄──┤───(+) cellule
   │    (+) ●──┼────┤         │
   │    (−) ●──┼────┤  GND ◄──┤───(−) cellule
   └───────────┘    │         │
                    │   VDD ──┼─── 5V Nano
                    │   GND ──┼─── GND Nano
                    │   SDA ──┼─── A4 Nano
                    │   SCL ──┼─── A5 Nano
                    │  ADDR ──┼─── GND (→ adresse 0x48)
                    └─────────┘
```

**Polarité cellule O2** : le fil **positif** (souvent rouge ou marqué +) va sur **A0**, le négatif sur **GND**. Inverser les fils donne une lecture négative → 0 %.

**Connecteur cellule** : la plupart des cellules utilisent un connecteur **Molex 43025** 2-broches ou un câble soudé. Vérifier avec la fiche du modèle choisi.

---

## Boutons TTP223

⚠️ **Important** : la logique est **HIGH quand touché** (mode par défaut du module TTP223). Le code utilise `pinMode(pin, INPUT)` **sans pull-up interne** car le TTP223 pilote activement la sortie.

```
                 TTP223 (×3)
               ┌──────────┐
               │  VCC ────┼── 5V Nano
               │  GND ────┼── GND Nano
               │  I/O ────┼── D2 (GAUCHE) / D3 (CENTRE) / D4 (DROITE)
               └──────────┘
                (pad capacitif au dos)
```

**Jumpers au dos du TTP223** :
- Par défaut : **sortie HIGH quand touché** (configuration attendue par le code)
- Si tu inverses le jumper `A` : sortie LOW quand touché → **ne fonctionnera pas**, il faudrait inverser la logique du code.
- Le jumper `B` passe en mode "toggle" (bascule) — **à laisser ouvert**, le code gère lui-même l'état.

---

## Imprimante TSC TH240

Communication série 9600 bauds via SoftwareSerial.

### Cas 1 : Imprimante en TTL 5V (rare)

Connexion directe possible :

```
  Nano D11 (TX) ────► RX imprimante
  Nano D10 (RX) ◄──── TX imprimante
  GND Nano       ──── GND imprimante
```

### Cas 2 : Imprimante en RS-232 standard (±12V, le plus fréquent sur TH240)

**Obligation d'utiliser un convertisseur de niveau MAX3232** sinon le Nano sera détruit par les tensions ±12V.

```
  Nano D11 (TX) ────► T1IN  │          │ T1OUT ────► Pin 2 DB9 (RX imprimante)
  Nano D10 (RX) ◄──── R1OUT │ MAX3232  │ R1IN  ◄──── Pin 3 DB9 (TX imprimante)
  5V Nano       ────  VCC   │          │
  GND Nano      ────  GND   │          │ GND   ────  Pin 5 DB9 (masse)
                            └──────────┘
```

**Alimentation imprimante** : la TH240 a son **propre bloc secteur 24V**. Ne jamais tenter de l'alimenter depuis le Nano — courant de tirage jusqu'à 2 A en impression.

---

## Alimentation

### Option A : Batterie 9V (simple)

```
  Batterie 9V (+) ──[interrupteur]── VIN Nano
  Batterie 9V (−) ─────────────────── GND Nano
```

Le régulateur interne du Nano fournit le 5V (max ~500 mA). Suffisant pour LCD + ADS + RTC + TTP223 au repos. **Autonomie ~3-5 h** avec une 9V alcaline.

### Option B : Li-Ion 3.7V + boost 5V (recommandé, plus longue autonomie)

```
  18650 3.7V ──── Module TP4056 (charge USB) ──── MT3608 boost 5V ──── Nano 5V
```

Plus économique, rechargeable via USB, autonomie **10-15 h**.

### Consommation approximative

| État | Consommation |
|------|--------------|
| Nano + LCD éteint | ~20 mA |
| + LCD allumé (rétroéclairé) | ~45 mA |
| + ADS en lecture continue | ~50 mA |
| + impression (pic, ~2 s) | **à charge imprimante**, pas sur Nano |

---

## Plan de masse

**Tous les GND doivent être reliés en étoile** sur un seul point commun (généralement le GND du Nano ou une barrette de bus). Éviter les masses en série qui créent des boucles et des décalages de tension sur la cellule O2.

---

## Checklist avant premier allumage

- [ ] Polarité cellule O2 respectée (+ sur A0, − sur GND de l'ADS)
- [ ] Jumpers ADDR de l'ADS1115 → **GND** (adresse 0x48)
- [ ] Jumper A du TTP223 → position **par défaut** (HIGH actif)
- [ ] Tous les GND reliés entre eux
- [ ] Pile CR2032 insérée dans le RTC (sinon perte de l'heure à chaque coupure)
- [ ] Bloc secteur imprimante branché séparément
- [ ] Adresse LCD (0x27 ou 0x3F) vérifiée avec un scanner I2C

---

## Scanner I2C (debug)

En cas de problème d'affichage, téléverser ce sketch jetable pour scanner les adresses :

```cpp
#include <Wire.h>
void setup() {
  Wire.begin();
  Serial.begin(9600);
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

Résultat attendu : `0x27` (LCD), `0x48` (ADS), `0x68` (RTC).
