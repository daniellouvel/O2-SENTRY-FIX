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
| D6 | Output | LED RGB WS2812B (data) |
| D7 | Input | PN532 IRQ (lecteur RFID, **optionnel**) |
| D8 | Output | PN532 RESET (lecteur RFID, **optionnel**) |
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

## Lecteur RFID PN532 (optionnel)

Module compatible Adafruit PN532 ou clones (NXP). Il partage le bus I2C avec les autres modules — adresse 0x24, **aucun conflit** avec LCD (0x27), ADS1115 (0x48) et RTC (0x68).

### Configuration du module

Sur le PN532 type "Adafruit shield" ou "Elechouse v3", il y a deux **switches DIP** ou jumpers de mode :

| Mode | SEL0 | SEL1 |
|------|------|------|
| **I2C** (utilisé ici) | OFF | ON |
| HSU (UART)            | OFF | OFF |
| SPI                   | ON  | OFF |

→ Vérifier que les switches sont bien sur **I2C**.

### Câblage

```
       PN532
   ┌────────────┐
   │ VCC ───────┼── 5V Nano (certains modules sont en 3.3V uniquement,
   │            │   verifier la fiche - en general le module a un
   │            │   regulateur 3.3V interne et accepte 5V)
   │ GND ───────┼── GND Nano
   │ SDA ───────┼── A4 Nano (partage avec autres modules I2C)
   │ SCL ───────┼── A5 Nano
   │ IRQ ───────┼── D7 Nano (signal "carte detectee")
   │ RSTO/RSTPDN┼── D8 Nano (reset hardware)
   └────────────┘
```

### Format des badges

Le firmware lit le **nom du plongeur** sur des cartes **Mifare Classic 1K** (les plus courantes, ~0.20€/pièce).

| Paramètre | Valeur |
|-----------|--------|
| Bloc lu | **4** (premier bloc de données du secteur 1) |
| Authentification | Clé A par défaut `FF FF FF FF FF FF` |
| Format | Texte ASCII brut, null-terminé |
| Longueur max | 14 caractères |

### Encodage d'un badge avec un téléphone Android

1. Installer l'application **MIFARE Classic Tool** (gratuite, Google Play)
2. Bouton "Write tag" → "Write block"
3. Approcher la carte du téléphone
4. Choisir : **secteur 1, bloc 0** (= bloc 4 absolu), **clé A par défaut**
5. Saisir le nom en hex (ex. `DUPONT` = `44 55 50 4F 4E 54`)
6. Compléter avec des `00` jusqu'à 16 octets

> Astuce : l'app propose un mode "Write text" qui fait la conversion ASCII → hex automatiquement.

### Workflow d'utilisation

- **Sans badge** : appui CENTRE imprime l'étiquette avec une ligne `Plongeur: ____________` à remplir au stylo
- **Avec badge** :
  - Si l'analyse est stable : impression immédiate avec le nom
  - Sinon : mode "armé" pendant 30s, impression auto dès que `[OK]`. Le LED clignote en bleu.
  - Annulation manuelle : appui CENTRE pendant l'attente
  - Re-passer le même badge réinitialise le timer 30s

---

## LED RGB de signalisation (WS2812B)

Une LED RGB adressable (NeoPixel) donne un état visible **de loin**, sans avoir à lire le LCD.

### Câblage

```
   WS2812B (module 1 LED ou strip)
   ┌──────────┐
   │ VCC/5V ──┼── 5V Nano
   │ GND ─────┼── GND Nano
   │ DI/DIN ──┼── D6 Nano (data)
   └──────────┘
```

> Une seule LED suffit. Si tu utilises un strip, le firmware ne pilote que la LED d'index 0.

### Codes couleur

| Couleur | Comportement | Signification |
|---------|--------------|---------------|
| ⚪ Blanc tamisé | fixe | Splash de démarrage |
| 🟠 Orange | fixe | Mesure en cours de stabilisation |
| 🟢 Vert | fixe | Stable + calibré, prêt à imprimer |
| 🔵 Bleu | clignotant 1 Hz | Badge détecté, mode armé (attente stab.) |
| 🟣 Violet | fixe (1.5s) | Impression en cours |
| 🟡 Jaune | fixe | Mode réglage de l'heure |
| 🔵 Cyan | fixe | Consultation de l'historique |
| 🔴 Rouge | clignotant ou fixe | Erreur (non calibré, cellule usée, badge invalide) |

> La luminosité est limitée à `LED_BRIGHTNESS = 80/255` dans le firmware pour éviter d'éblouir et de tirer trop de courant. Modifiable dans `main.cpp`.

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
- [ ] (Si PN532) DIP switches en mode **I2C** (SEL0 OFF, SEL1 ON)
- [ ] (Si PN532) Clé A par défaut `FF FF FF FF FF FF` non modifiée sur la carte
- [ ] LED WS2812B en sens correct : DIN côté Nano, DOUT vers la suivante (si plusieurs)

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
