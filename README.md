# O2-SENTRY-FIX

**Analyseur fixe d'oxygène pour plongée** (station de paillasse, alimentation secteur 220V AC) — basé sur Arduino Nano, avec impression d'étiquette d'analyse de mélange Nitrox/Trimix.

Projet de remplacement d'un analyseur O2 commercial défaillant, entièrement DIY et documenté.

---

## Fonctionnalités

- **Mesure O2** via cellule galvanique + ADS1115 (résolution 7.8 µV)
- **MOD** (Maximum Operating Depth) calculée en temps réel, ppO2 réglable 1.0 → 1.6
- **Indicateur de stabilité** sur 15 lectures (seuil 0.1 %)
- **Calibration à l'air** mémorisée en EEPROM (survit aux coupures)
- **Horodatage RTC** sauvegardé même batterie débranchée
- **Impression d'étiquette** TSPL sur imprimante thermique TSC TH240
- **Interface 3 boutons tactiles** (appui court / appui long)
- **Affichage LCD 16×2** rétroéclairé
- **Sans `delay()`** — code non bloquant basé sur `millis()`

---

## Matériel requis (BOM)

| Composant | Rôle | Notes |
|-----------|------|-------|
| Arduino Nano (ATmega328P) | MCU principal | clone ou original |
| ADS1115 (I2C, 0x48) | ADC 16 bits | gain ×16 pour sensibilité max |
| LCD 1602 I2C (0x27) | Affichage | avec module PCF8574 |
| RTC DS3231 (ou DS1307) | Horloge temps réel | DS3231 recommandé (plus précis) |
| 3× TTP223 | Boutons tactiles capacitifs | mode par défaut (HIGH actif) |
| Imprimante TSC TH240 | Étiqueteuse thermique | communication série 9600 bauds |
| Cellule O2 (ex: R-17 Med, R-22, OOM-202) | Capteur galvanique | sortie ~9-13 mV à l'air |
| Bloc secteur 9V DC 1A (jack 5.5/2.1 mm) | Alimentation Nano | depuis 220V AC |
| Interrupteur secteur + porte-fusible | Sécurité 220V | fusible 1A temporisé |
| Boîtier de paillasse (type coffret ABS) | Intégration fixe | avec passe-câbles |
| Câblage | — | fils 22 AWG, domino ou bornier |

---

## Schéma de câblage

Voir [WIRING.md](WIRING.md) pour le détail complet avec schémas ASCII.

**Résumé rapide** :

| Module | Pin Arduino Nano |
|--------|------------------|
| I2C (LCD / ADS1115 / RTC) | SDA=A4, SCL=A5 |
| Bouton GAUCHE (TTP223) | D2 |
| Bouton CENTRE (TTP223) | D3 |
| Bouton DROITE (TTP223) | D4 |
| Imprimante TSC RX | D11 (TX du Nano) |
| Imprimante TSC TX | D10 (RX du Nano) |
| Cellule O2 (+) | ADS1115 A0 |
| Cellule O2 (−) | ADS1115 GND |

---

## Installation logicielle

Voir [INSTALLATION.md](INSTALLATION.md) — utilise **PlatformIO dans VSCode**.

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
| **GAUCHE** | ppO2 − 0.1 (min 1.0) | — |
| **CENTRE** | Imprimer étiquette (si stable) | Entrer en réglage heure |
| **DROITE** | ppO2 + 0.1 (max 1.6) | Lancer la calibration |

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
4. Après l'année, l'horloge est sauvegardée et on revient en mode Lecture

### Calcul MOD

```
MOD = ((ppO2 / (fO2 / 100)) − 1) × 10
```

Exemple : fO2 = 32 %, ppO2 = 1.4 → MOD = (1.4 / 0.32 − 1) × 10 = **33.75 m**

---

## Impression d'étiquette

L'étiquette imprimée (50×30 mm) contient :

```
┌────────────────────────┐
│     ANALYSE O2         │
│                        │
│     O2: 32.0 %         │
│     MOD: 34 m          │
│     ppO2 ref: 1.4      │
│                        │
│  2026-04-24  14:32     │
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
- `Wire`, `SoftwareSerial`, `EEPROM` (intégrées à Arduino)

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
