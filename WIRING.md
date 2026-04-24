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
| D5 | I/O | OneWire DS18B20 (capteur de température, **optionnel**) |
| D10 | Input | RX SoftwareSerial (vers TX imprimante) |
| D11 | Output | TX SoftwareSerial (vers RX imprimante) |
| 5V | Power | Alimentation modules logiques |
| 3.3V | Power | (optionnel — TTP223 peut être en 3.3V) |
| GND | Power | Masse commune |
| VIN | Power | Entrée bloc secteur 9V DC |

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

## Capteur de température DS18B20 (optionnel)

Le firmware détecte automatiquement la présence du DS18B20 au démarrage. **Si absent, aucune compensation n'est appliquée** (comportement par défaut). Si présent, la mesure O2 est corrigée de la dérive thermique de la cellule galvanique (~0.3 %/°C).

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
             ┌────┼────── 5V Nano
             │    │
             └────┴────── D5 Nano
                  │
                 GND ─── GND Nano
                 VDD ─── 5V  Nano
```

**Points importants** :
- La résistance de **pull-up 4.7 kΩ** entre DQ et VCC est **obligatoire** (le bus OneWire utilise la collecteur ouvert).
- Pour une mesure stable, placer le DS18B20 **au plus près de la cellule O2** (même enceinte/chambre).
- En mode "parasite power" (2 fils), le câblage est différent — **non utilisé ici**, on câble les 3 fils.
- La température de calibration est sauvegardée en EEPROM, donc la compensation est relative à l'environnement thermique du moment de la calibration.

**Câblage avec plusieurs capteurs** : le bus OneWire supporte plusieurs DS18B20 en parallèle sur le même fil, mais le firmware ne lit que l'**index 0** (le premier détecté). Un seul capteur suffit.

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

## Alimentation 220V AC

Appareil fixe de paillasse alimenté sur le **secteur 220V AC**.

### Schéma d'alimentation

```
  Prise 220V AC
       │
   ┌───┴────┐
   │ Fusible│  1 A temporisé
   │ 1A (T) │
   └───┬────┘
       │
   ┌───┴────────┐
   │Interrupteur│  bipolaire, coupe phase + neutre
   │  bipolaire │
   └───┬────────┘
       │
       ├─────────────────────► Bloc secteur 9V DC 1A ──► jack Nano (VIN)
       │                                                 (5V interne Nano)
       │
       └─────────────────────► Imprimante TSC TH240 (bloc 24V d'origine)
```

**Points clés** :
- **Un seul interrupteur bipolaire** coupe tout (Nano + imprimante)
- **Fusible 1 A temporisé (T)** en entrée, toujours sur la **phase**
- **Bloc secteur 9V DC 1A** type "adaptateur mural" standard, jack 5.5/2.1 mm → branché sur le jack DC du Nano
- L'**imprimante garde son bloc 24V d'origine** (non modifié)
- **Masses séparées** : le GND du Nano et le GND de l'imprimante se rejoignent uniquement via le fil TTL de données (cf. section imprimante)

### Câblage 220V (côté secteur)

```
  Phase (marron) ──► Fusible ──► Interrupteur ──┬── Bloc 9V DC
                                                 │
  Neutre (bleu) ───────────────► Interrupteur ──┼── Bloc 9V DC
                                                 │
                                                 └── Imprimante (via prise)
  Terre (vert/jaune) ──► boîtier métallique (si applicable)
```

### Sécurité 220V — ⚠️ OBLIGATOIRE

- **Toutes les connexions 220V sous gaine thermo ou dans un domino fermé**, aucun contact exposé
- **Distance minimale** de 4 mm entre pistes/connexions 220V et basse tension (règle "creepage")
- **Boîtier plastique ABS ignifuge** (préféré) ou **boîtier métallique relié à la terre**
- **Décharge de traction** sur le câble secteur (passe-câble à vis ou serre-câble)
- Vérifier que le **bloc secteur 9V** est marqué **CE** et double isolation (symbole ⊡)

### Consommation

| État | Consommation côté 220V |
|------|-------------------------|
| Veille (Nano + LCD + ADS) | ~2 W |
| Impression d'étiquette (pic ~2 s) | jusqu'à 30 W |

Un transformateur 9V/1A (9 VA) + imprimante 24V/2A (48 VA) = **~60 VA max**, largement couvert par un fusible 1 A temporisé.

### Option alternative : module AC-DC intégré (HLK-PM01)

Pour un montage plus compact (pas de bloc secteur externe), on peut intégrer un module **HLK-PM01** (220V AC → 5V DC, 600 mA) directement dans le boîtier, branché sur le pin **5V** du Nano (en bypassant le régulateur).

```
  220V AC ──► HLK-PM01 ──► 5V DC ──► pin 5V du Nano (pas VIN)
                           GND DC ──► GND du Nano
```

**⚠️ À ne faire que si tu maîtrises les règles d'isolation 220V en PCB.** Sinon, reste sur le bloc secteur externe : c'est plus sûr et déjà certifié CE.

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
- [ ] (Si DS18B20) Résistance pull-up 4.7 kΩ entre D5 et 5V bien présente

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
