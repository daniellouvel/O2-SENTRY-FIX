# O2-SENTRY-FIX

**Analyseur fixe d'oxygène pour plongée** (station de paillasse, alimentation secteur 220V AC) — avec impression d'étiquette d'analyse de mélange Nitrox/Trimix.

Projet de remplacement d'un analyseur O2 commercial défaillant, entièrement DIY et documenté.

## Branches

| Branche | Microcontrôleur | État |
|---------|-----------------|------|
| `main` | Arduino Nano (ATmega328P) | version de référence |
| `esp32-port` | **ESP32-WROOM-32** | version active — plus de flash, touch natif |

> **Branche recommandée : `esp32-port`**. Le Nano est à la limite de la mémoire flash avec toutes les bibliothèques (PN532 + NeoPixel + RTC + ADS + LCD ≈ 28/30 KB). L'ESP32 n'a pas cette contrainte (4 MB flash).

---

## Fonctionnalités

- **Mesure O2** via cellule galvanique + ADS1115 (résolution 7.8 µV)
- **MOD** (Maximum Operating Depth) calculée en temps réel, choix direct entre ppO2 **1.4** (standard) et **1.6** (max)
- **Indicateur de stabilité** sur 15 lectures (seuil 0.1 %)
- **Calibration à l'air** mémorisée en EEPROM (survit aux coupures)
- **Horodatage RTC** sauvegardé même batterie débranchée
- **Identification du plongeur par badge RFID** (Mifare Classic) — impression auto avec nom
- **LED RGB** d'état visible de loin (couleurs par mode)
- **Compensation thermique** via DS18B20 optionnel
- **Historique EEPROM** des 10 dernières analyses
- **Impression d'étiquette** TSPL sur imprimante thermique TSC TH240
- **Interface 3 boutons tactiles** (appui court / appui long)
- **Affichage LCD 16×2** rétroéclairé
- **Sans `delay()`** — code non bloquant basé sur `millis()`

---

## Matériel requis (BOM)

> BOM pour la branche **`esp32-port`** (ESP32-WROOM-32). Voir branche `main` pour la version Arduino Nano.

| Composant | Rôle | Notes |
|-----------|------|-------|
| ESP32-WROOM-32 (carte de dev 38 pins) | MCU principal | 4 MB flash, WiFi/BT non utilisés |
| ADS1115 (I2C, 0x48) | ADC 16 bits | gain ×16, alimentation 3.3 V |
| LCD 1602 I2C (0x27) | Affichage | avec module PCF8574 |
| RTC DS3231 (ou DS1307) | Horloge temps réel | DS3231 recommandé (plus précis) |
| 3× TTP223 **ou** 3 pads métalliques | Boutons tactiles | TTP223 = `BUTTON_MODE=0` · pads nus = `BUTTON_MODE=1` (touch ESP32) |
| Imprimante TSC TH240 | Étiqueteuse thermique | UART2 9600 bauds + MAX3232 si RS-232 |
| Cellule O2 (ex: R-17 Med, R-22, OOM-202) | Capteur galvanique | sortie ~9-13 mV à l'air |
| DS18B20 (TO-92) + pull-up 4.7 kΩ | Capteur de température | **optionnel**, compensation thermique 0.3 %/°C |
| Module PN532 (I2C) + cartes Mifare Classic 1K | Lecteur RFID + badges | **optionnel**, nom du plongeur sur l'étiquette |
| WS2812B (1 LED RGB) + 74HCT245 | Indicateur d'état | level-shifter 3.3 V → 5 V recommandé |
| Bloc secteur 5V DC 2A | Alimentation ESP32 | depuis 220V AC |
| Interrupteur secteur + porte-fusible | Sécurité 220V | fusible 1A temporisé |
| Boîtier de paillasse (type coffret ABS) | Intégration fixe | avec passe-câbles |
| Câblage | — | fils 22 AWG, domino ou bornier |

---

## Schéma de câblage

Voir [WIRING.md](WIRING.md) pour le détail complet (schémas ASCII, notes niveau logique, touch natif).

**Résumé rapide (branche `esp32-port`)** :

| Module | Pin ESP32-WROOM-32 |
|--------|---------------------|
| I2C (LCD / ADS1115 / RTC / PN532) | SDA=G21, SCL=G22 |
| Bouton GAUCHE (TTP223 ou touch) | G32 (T9) |
| Bouton CENTRE (TTP223 ou touch) | G33 (T8) |
| Bouton DROITE (TTP223 ou touch) | G27 (T7) |
| DS18B20 DQ (optionnel) | G4 (pull-up 4.7 kΩ vers 3V3) |
| LED RGB WS2812B (DIN) | G5 (via 74HCT245 recommandé) |
| PN532 IRQ (optionnel) | G18 |
| PN532 RESET (optionnel) | G19 |
| Imprimante TSC — UART2 TX | G17 |
| Imprimante TSC — UART2 RX | G16 |
| Cellule O2 (+) | ADS1115 A0 (différentiel) |
| Cellule O2 (−) | ADS1115 A1 (différentiel) |

---

## Installation logicielle

Voir [INSTALLATION.md](INSTALLATION.md) — utilise **PlatformIO dans VSCode**.

Choisir l'environnement selon le type de boutons :

```
# Boutons TTP223 (digitalRead)
pio run -e esp32-ttp223 -t upload

# Touch natif ESP32 (pad conducteur)
pio run -e esp32-touch -t upload
```

---

## Utilisation

### Mode Lecture (mode par défaut)

```
┌────────────────┐
│O2:32.0% [OK]   │  ← pourcentage + stabilité
│M:34m p1.4 12:45│  ← MOD + ppO2 de réf + heure
└────────────────┘
```

| Bouton | Appui court | Appui long (3 s) |
|--------|-------------|-------------------|
| **GAUCHE** | ppO2 = **1.4** (valeur standard) | Consulter l'historique |
| **CENTRE** | Imprimer étiquette **sans nom** (ligne `Plongeur: ____` à remplir) | Entrer en réglage heure |
| **DROITE** | ppO2 = **1.6** (valeur maximale) | Lancer la calibration |

### Workflow avec badge RFID (optionnel)

Si un module PN532 est branché, l'utilisateur peut **identifier le plongeur** en passant un badge Mifare Classic 1K sur le lecteur. Le nom est lu en bloc 4 (texte ASCII, 14 caractères max).

```
┌─ Cas A : analyse stable + badge passé
│   → impression immédiate avec le nom
│
├─ Cas B : analyse instable + badge passé
│   → mode "armé" pendant 30 secondes
│   → LED bleue clignotante
│   → LCD : "ARM:DUPONT  29s"
│   → impression auto dès stabilité atteinte
│   → si timeout 30s sans stabilité : annulation, feedback "Pas stabilise"
│   → annulation manuelle : appui CENTRE
│
└─ Cas C : pas de badge
    → appui CENTRE imprime sans nom
```

> **Anti-double impression** : tant que le badge reste posé sur le lecteur, aucune nouvelle impression n'est lancée. Il faut retirer le badge puis le repasser.

### LED RGB d'état (WS2812B)

| Couleur | Signification |
|---------|---------------|
| 🟢 Vert fixe | Stable + calibré, prêt à imprimer |
| 🟠 Orange fixe | Stabilisation en cours |
| 🔵 Bleu clignotant | Badge détecté, mode armé |
| 🟣 Violet 1.5s | Impression en cours |
| 🟡 Jaune fixe | Mode réglage de l'heure |
| 🔵 Cyan fixe | Mode historique |
| 🔴 Rouge clignotant | Erreur (non calibré, cellule usée, badge invalide) |
| ⚪ Blanc tamisé | Splash de démarrage |

### Mode Historique

Les 10 dernières analyses imprimées sont conservées en EEPROM (FIFO circulaire).

```
┌────────────────┐
│#3/10 fO2:32.0% │  ← position + pourcentage
│24/04 12:45 M34 │  ← date, heure, MOD
└────────────────┘
```

| Bouton | Appui court |
|--------|-------------|
| **GAUCHE** | Analyse plus ancienne |
| **DROITE** | Analyse plus récente |
| **CENTRE** | Sortir |

Le tag `[OK]` s'affiche uniquement lorsque l'écart max−min sur les 15 dernières mesures est < 0.1 %. L'impression est bloquée tant que la mesure est instable (`...`).

### Mode Calibration

1. Exposer la cellule à l'**air ambiant** (20.9 % O2) pendant 1-2 min
2. Attendre la stabilisation (`[OK]`)
3. **Appui long DROITE** (3 s)
4. Message `Calibration OK — Air = 20.9%` pendant 1.5 s
5. Valeur sauvegardée en EEPROM (persistante)

### Mode Réglage de l'heure

1. **Appui long CENTRE** (3 s)
2. Séquence : **Heure → Minute → Jour → Mois → Année**
3. GAUCHE / DROITE pour modifier, CENTRE pour valider et passer au champ suivant
4. Le champ actif est encadré par `[..]` sur l'afficheur
5. Après l'année, l'horloge est sauvegardée et on revient en mode Lecture
6. **Appui long GAUCHE à tout moment = annuler sans sauver**

> À la première mise sous tension (ou après une perte d'alimentation du RTC), l'analyseur entre automatiquement dans ce mode pour forcer le réglage.

### Calcul MOD

```
MOD = ((ppO2 / (fO2 / 100)) − 1) × 10
```

Exemple : fO2 = 32 %, ppO2 = 1.4 → MOD = (1.4 / 0.32 − 1) × 10 = **33.75 m**

---

## Impression d'étiquette

L'étiquette imprimée (50×30 mm) contient :

**Avec badge RFID** :
```
┌────────────────────────┐
│     ANALYSE O2         │
│  Plongeur: DUPONT      │
│                        │
│     O2: 32.0 %         │
│     MOD: 34 m          │
│     ppO2 ref: 1.4      │
│  2026-04-27  14:32     │
└────────────────────────┘
```

**Sans badge** (la ligne reste vide pour remplir au stylo) :
```
┌────────────────────────┐
│     ANALYSE O2         │
│  Plongeur: ___________ │
│                        │
│     O2: 32.0 %         │
│     MOD: 34 m          │
│     ppO2 ref: 1.4      │
│  2026-04-27  14:32     │
└────────────────────────┘
```

---

## Structure du projet

```
O2-SENTRY-FIX/
├── src/
│   └── main.cpp           Code principal
├── platformio.ini         Configuration PlatformIO
├── README.md              Ce fichier
├── INSTALLATION.md        Guide d'installation logicielle
├── WIRING.md              Guide de câblage détaillé
├── LICENSE                Licence MIT
└── .gitignore
```

---

## Dépendances logicielles

Gérées automatiquement par PlatformIO via [platformio.ini](platformio.ini) :

- [Adafruit ADS1X15](https://github.com/adafruit/Adafruit_ADS1X15)
- [RTClib](https://github.com/adafruit/RTClib)
- [LiquidCrystal_I2C](https://github.com/marcoschwartz/LiquidCrystal_I2C)
- [OneWire](https://github.com/PaulStoffregen/OneWire) + [DallasTemperature](https://github.com/milesburton/Arduino-Temperature-Control-Library)
- [Adafruit PN532](https://github.com/adafruit/Adafruit-PN532)
- [Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel)
- `Wire`, `HardwareSerial`, `EEPROM` (intégrées à l'ESP32 Arduino framework)

---

## Avertissements

> **⚠️ SÉCURITÉ PLONGÉE** : Ce projet est un outil d'aide à la préparation, **pas un équipement de sécurité certifié**. Toujours vérifier l'analyse avec un analyseur commercial avant une plongée. L'auteur décline toute responsabilité en cas d'accident.

> **Calibration régulière** : La cellule O2 vieillit (durée de vie ~1 an). Calibrer avant chaque session et remplacer la cellule si la tension à l'air chute sous ~7 mV.

> **Précision** : ±0.5 % O2 après bonne calibration. Pas adapté pour les analyses Trimix He (nécessite un second capteur).

---

## Licence

MIT — voir [LICENSE](LICENSE).

---

## Auteur

Daniel Louvel — [@daniellouvel](https://github.com/daniellouvel)
