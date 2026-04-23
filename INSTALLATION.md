# Installation - Analyseur O2

## Prérequis

- **VSCode** installé ([télécharger ici](https://code.visualstudio.com/))
- Un **Arduino Nano** et un câble USB
- Windows / macOS / Linux

## 1. Installer PlatformIO dans VSCode (une seule fois)

1. Ouvre **VSCode**
2. Va dans l'onglet **Extensions** (raccourci : `Ctrl+Shift+X`)
3. Cherche **PlatformIO IDE** (éditeur : `platformio.platformio-ide`)
4. Clique sur **Install**
5. Attends la fin de l'installation initiale (plusieurs minutes, PlatformIO télécharge les toolchains AVR)
6. Redémarre VSCode si demandé

## 2. Ouvrir le projet

1. Dans VSCode : `Fichier → Ouvrir le dossier`
2. Sélectionne `d:\dev\O2-Sentry-fix`
3. PlatformIO détecte automatiquement le fichier `platformio.ini`
4. Il télécharge les 3 librairies nécessaires au premier build :
   - `Adafruit ADS1X15`
   - `RTClib`
   - `LiquidCrystal_I2C`

## 3. Compiler et flasher

1. Branche la carte Nano en USB
2. Utilise la **barre d'état bleue en bas de VSCode** :

| Icône | Action | Raccourci |
|-------|--------|-----------|
| ✔ | **Build** — compile le code | `Ctrl+Alt+B` |
| → | **Upload** — flash la carte | `Ctrl+Alt+U` |
| 🔌 | **Monitor** — moniteur série | `Ctrl+Alt+S` |
| 🗑 | **Clean** — nettoie le build | `Ctrl+Alt+C` |

## 4. En cas de problème d'upload

Si tu vois `avrdude: stk500_recv(): programmer is not responding` :

- Ta Nano utilise probablement l'**ancien bootloader**
- Clique sur l'environnement actif en bas de VSCode (par défaut `Default (nanoatmega328new)`)
- Sélectionne `env:nanoatmega328` (upload à 57600 bauds)
- Refais **Upload**

## Structure du projet

```
O2-Sentry-fix/
├── .vscode/
│   └── extensions.json       → recommande PlatformIO automatiquement
├── src/
│   └── main.cpp              → code principal
├── platformio.ini            → config board + librairies
└── .gitignore
```

## À savoir

- PlatformIO gère les librairies automatiquement dans `.pio/libdeps/` (rien à installer à la main)
- L'intellisense C++ fonctionne avec toutes les définitions Arduino
- Pas besoin de l'IDE Arduino en parallèle
- Le dossier `.pio/` est généré automatiquement (exclu du git via `.gitignore`)
