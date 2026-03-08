# 🎚️ SoundPanel 7

<p align="center">
  <strong>A connected wall panel for sound awareness, precision time, and calm workspaces.</strong>
</p>

<p align="center">
  Built on ESP32-S3 with a 7" touchscreen, real-time sound monitoring, NTP clock, web UI, MQTT, OTA, and Home Assistant integration.
</p>

<p align="center">
  <a href="#francais">Francais</a> ·
  <a href="#english">English</a>
</p>

---

## Table Of Contents

- [Francais](#francais)
- [Vision](#vision)
- [Points forts](#points-forts)
- [Cas d'usage](#cas-dusage)
- [Fonctionnalites](#fonctionnalites)
- [Materiel cible](#materiel-cible)
- [Demarrage rapide](#demarrage-rapide)
- [Configuration par defaut](#configuration-par-defaut)
- [Interface web](#interface-web)
- [Horloge NTP](#horloge-ntp)
- [MQTT](#mqtt)
- [Home Assistant](#home-assistant)
- [OTA](#ota)
- [Calibration](#calibration)
- [Architecture](#architecture)
- [Arborescence](#arborescence)
- [Etat du projet](#etat-du-projet)
- [Contribution](#contribution)
- [English](#english)
- [Overview](#overview)
- [Key features](#key-features)
- [Use cases](#use-cases)
- [Hardware target](#hardware-target)
- [Quick start](#quick-start)
- [Default configuration](#default-configuration)
- [Web interface](#web-interface)
- [NTP clock](#ntp-clock)
- [MQTT and Home Assistant](#mqtt-and-home-assistant)
- [OTA updates](#ota-updates)
- [Calibration workflow](#calibration-workflow)
- [Firmware architecture](#firmware-architecture)
- [Project layout](#project-layout)
- [Project status](#project-status)
- [License](#license)

---

<a id="francais" name="francais"></a>

## 🇫🇷 Francais

### ✨ Vision

SoundPanel 7 est un panneau mural connecte qui rend deux informations visibles en un coup d'oeil :

- le niveau sonore ambiant
- l'heure reseau synchronisee a la seconde

L'idee de depart etait tres simple.
Je travaille dans un open space trop bruyant, et je voulais un outil de **pedagogie douce** :
quelque chose de visible, factuel, elegant, et suffisamment clair
pour faire baisser naturellement le volume sans transformer l'espace
en salle de classe.

De la est nee une idee plus large :
un panneau autonome, lisible de loin, utile au bureau comme en regie,
en studio, en podcast, en atelier ou sur un plateau.

SoundPanel 7 n'est donc pas seulement un sonometre.
C'est un vrai **panneau d'ambiance et de supervision locale**,
capable d'afficher le son, le temps, l'etat reseau,
et de s'integrer proprement dans un environnement connecte.

![SoundPanel 7 product overview](docs/images/soundpanel7-hero-overview.jpg)

### 🚀 Points forts

- Grand affichage tactile 7" lisible a distance
- Mesure sonore en temps reel avec **dB instantane**, **Leq** et **Peak**
- **Grande horloge NTP** avec secondes visibles
- Interface web embarquee pour administration et calibration
- Publication **MQTT**
- **Home Assistant** via MQTT Discovery ou integration native
- Mises a jour **OTA**
- Stockage persistant des reglages

### 🎯 Cas d'usage

- Open space trop bruyant : rendre le niveau sonore visible sans agressivite
- Studio d'enregistrement : garder les niveaux et l'heure sous les yeux
- Regie ou diffusion : afficher une heure reseau fiable avec secondes
- Podcast ou voix off : suivre un top horaire ou un timing de prise
- Atelier ou lieu public : afficher un indicateur simple, comprehensible par tous
- Domotique personnelle : ajouter un panneau mural utile, pas juste decoratif

![SoundPanel 7 wall mounted in open space](docs/images/soundpanel7-wall-mounted-open-space.jpg)

![SoundPanel 7 in studio showing clock and sound levels](docs/images/soundpanel7-studio-clock-and-levels.jpg)

### 🧩 Fonctionnalites

#### 🔊 Monitoring sonore

- mesure continue du niveau sonore
- calcul du **Leq**
- calcul du **Peak**
- seuils visuels configurables
- historique glissant configurable
- calibration micro en 3 points

#### 🕒 Horloge et synchronisation

- horloge grand format affichee en permanence
- affichage des **secondes** pour les usages de diffusion, prise et synchro
- synchronisation automatique via **NTP**
- configuration du serveur NTP et de la timezone
- statut de synchronisation visible dans l'UI

#### 🖥️ Interface locale

- pilotage tactile sur ecran 7"
- lecture immediate des niveaux et de l'heure
- consultation des informations systeme
- usage autonome sans navigateur

#### 🌐 Interface web

- page de statut temps reel
- page d'administration
- configuration UI, NTP, OTA, MQTT et reseau
- calibration depuis le navigateur
- actions systeme : reboot, reset usine, portail Wi-Fi

#### 📡 Connectivite

- Wi-Fi via portail de configuration
- MQTT
- MQTT Discovery pour Home Assistant
- integration Home Assistant native via Zeroconf/mDNS
- OTA via `espota`

### 🛠️ Materiel cible

#### 🧠 Carte principale

- **Waveshare ESP32-S3-Touch-LCD-7**
- ecran tactile 7"
- ESP32-S3
- Wi-Fi
- PSRAM
- USB
- retroeclairage pilotable

#### 🎤 Entree audio

Le firmware est pense pour un **micro analogique** branche sur l'entree capteur.

Exemples compatibles cites dans le projet :

- `MAX4466`
- `MAX9814`
- equivalent analogique

Par defaut, le projet compile avec un **mode audio mock** pour faciliter le developpement.

Dans [`platformio.ini`](platformio.ini), le flag suivant est actif par defaut :

```ini
-DSOUNDPANEL7_MOCK_AUDIO=1
```

Si tu branches une vraie entree analogique, verifie ce point avant d'evaluer le comportement du panneau.

### ⚡ Demarrage rapide

#### 1. Cloner le depot

```bash
git clone https://github.com/jjtronics/SoundPanel7.git
cd SoundPanel7
```

#### 2. Preparer l'environnement

Le plus simple :

- **VS Code**
- extension **PlatformIO**

Tu peux aussi utiliser `pio` en ligne de commande si PlatformIO Core est deja installe.

#### 3. Compiler

```bash
pio run
```

#### 4. Flasher en USB

L'environnement par defaut est `soundpanel7_usb`.

```bash
pio run -e soundpanel7_usb -t upload
```

#### 5. Ouvrir le moniteur serie

```bash
pio device monitor -b 115200
```

Au boot, le firmware initialise successivement :

1. le stockage des reglages
2. l'affichage et le tactile
3. le reseau
4. l'OTA
5. le MQTT
6. l'interface web
7. le moteur audio

### ⚙️ Configuration par defaut

Valeurs actuelles du firmware :

- hostname : `soundpanel7`
- serveur NTP : `fr.pool.ntp.org`
- timezone POSIX : `CET-1CEST,M3.5.0/2,M10.5.0/3`
- OTA activee : `oui`
- port OTA : `3232`
- MQTT active : `non`
- topic MQTT de base : `soundpanel7`
- pin analogique : `GPIO6`
- source audio par defaut : capteur analogique

Ces reglages sont definis dans [`src/SettingsStore.h`](src/SettingsStore.h).

### 🌐 Interface web

Une fois l'appareil connecte au Wi-Fi :

- `http://IP_DU_SOUNDPANEL/`
- `http://IP_DU_SOUNDPANEL/admin`

L'interface permet notamment de regler :

- luminosite
- seuils de couleur
- duree d'historique
- NTP et timezone
- hostname
- parametres OTA
- parametres MQTT
- calibration micro

![SoundPanel 7 live dashboard](docs/images/soundpanel7-dashboard-live-view.png)

![SoundPanel 7 admin web interface](docs/images/soundpanel7-admin-web-interface.png)

### 🕒 Horloge NTP

SoundPanel 7 est aussi une **grande horloge reseau**.

C'est un vrai usage, pas un gadget.
Dans un studio, une regie, un espace podcast ou un environnement de diffusion,
avoir une heure fiable avec les secondes en grand est aussi utile que l'affichage du niveau sonore.

Le firmware gere :

- la synchronisation NTP automatique
- le parametrage du serveur NTP
- la timezone POSIX
- l'affichage local `HH:MM` avec badge secondes
- l'heure complete cote interface web

Par defaut, le serveur NTP configure est `fr.pool.ntp.org`.

![SoundPanel 7 studio clock and sound levels](docs/images/soundpanel7-studio-clock-and-levels.jpg)

### 📶 MQTT

Le panneau peut publier ses mesures vers un broker MQTT.

Parametres disponibles :

- host
- port
- username / password
- client ID
- topic racine
- intervalle de publication
- retain

Exemples de topics :

```text
soundpanel7/db
soundpanel7/leq
soundpanel7/peak
soundpanel7/status
soundpanel7/uptime
soundpanel7/wifi/rssi
soundpanel7/wifi/ip
```

### 🏠 Home Assistant

Deux approches sont proposees.

#### Option 1 : MQTT Discovery

La plus simple si Home Assistant utilise deja MQTT.

Tu actives MQTT sur le SoundPanel 7, tu renseignes le broker, et les entites sont creees automatiquement.

#### Option 2 : integration native Home Assistant

Une integration custom est fournie dans :

[`custom_components/soundpanel7`](custom_components/soundpanel7)

Le firmware annonce le service :

```text
_soundpanel7._tcp.local.
```

L'integration interroge ensuite l'API HTTP du panneau, notamment `/api/status`.

##### Installation

Copier `custom_components/soundpanel7` dans le dossier de configuration Home Assistant :

```text
config/custom_components/soundpanel7
```

Exemple :

```bash
mkdir -p /config/custom_components
cp -R custom_components/soundpanel7 /config/custom_components/
```

Puis :

1. redemarrer Home Assistant
2. redemarrer le SoundPanel 7
3. ouvrir `Parametres > Appareils et services`
4. attendre la decouverte automatique

Si rien n'apparait, verifier en priorite :

- que Home Assistant et le panneau sont sur le meme reseau
- que `manifest.json` est bien present cote Home Assistant
- que le panneau repond bien sur son interface web
- que le mDNS n'est pas perturbe par le reseau ou les VLANs

![SoundPanel 7 Home Assistant discovery](docs/images/soundpanel7-home-assistant-discovery.png)

### 🚀 OTA

Quand l'OTA est configuree et activee sur l'appareil :

```bash
pio run -e soundpanel7_ota -t upload
```

L'environnement OTA de [`platformio.ini`](platformio.ini) repose sur :

- `upload_protocol = espota`
- port par defaut `3232`
- mot de passe OTA configurable

Pense a ajuster `upload_port` a l'adresse IP reelle de ton appareil.

### 🎚️ Calibration

Le systeme de calibration fonctionne en **3 points**.

Procedure recommandee :

1. placer un sonometre de reference a cote du SoundPanel 7
2. generer ou mesurer un niveau stable
3. saisir la valeur reelle
4. capturer les trois points de reference

Valeurs typiques utiles :

- `45 dB`
- `65 dB`
- `85 dB`

Le firmware stocke ensuite les points et corrige la lecture.

![SoundPanel 7 calibration workflow](docs/images/soundpanel7-calibration-workflow.png)

### 🧠 Architecture

Le coeur du projet reste volontairement simple :

```text
AudioEngine -> SharedHistory -> UI / Web / MQTT
                  |
                  -> stockage et restitution des mesures
```

Composants principaux :

- [`src/main.cpp`](src/main.cpp) : orchestration globale
- [`src/AudioEngine.cpp`](src/AudioEngine.cpp) : acquisition et calculs audio
- [`src/ui/UiManager.cpp`](src/ui/UiManager.cpp) : interface LVGL
- [`src/WebManager.cpp`](src/WebManager.cpp) : HTTP, admin et live view
- [`src/MqttManager.cpp`](src/MqttManager.cpp) : MQTT et discovery
- [`src/OtaManager.cpp`](src/OtaManager.cpp) : OTA
- [`src/SettingsStore.cpp`](src/SettingsStore.cpp) : persistance NVS

### 📁 Arborescence

```text
.
├── src/                       Firmware principal
├── data/                      Fichiers web embarques
├── custom_components/         Integration Home Assistant
├── include/                   Headers partages
└── platformio.ini             Build, flash et environnements
```

### 🔧 Etat du projet

Le projet est deja exploitable, mais reste evolutif :

- la chaine audio depend encore beaucoup du capteur analogique reel
- le rendu final depend de la calibration
- des captures d'ecran du produit manquent encore au README
- certains parametres de `platformio.ini` sont ajustes pour la machine de dev actuelle

En clair : c'est un projet serieux, vivant, et deja tres utile.

### 🤝 Contribution

Les contributions sont bienvenues, surtout sur :

- documentation
- guide hardware et cablage
- calibration
- UX de l'interface
- integration et fiabilite reseau

Point d'entree recommande :

1. compiler
2. flasher
3. valider l'affichage local
4. tester l'interface web
5. tester MQTT, OTA et Home Assistant

---

<a id="english" name="english"></a>

## 🇬🇧 English

### ✨ Overview

SoundPanel 7 is a connected wall panel that makes two things instantly visible:

- ambient sound level
- network-synchronized time down to the second

The original idea came from a very practical problem:
the open space where I work was simply too noisy.

Instead of policing people or constantly asking coworkers to lower their voices,
the goal was to build a **gentle awareness tool**:
something visible, factual, calm, and elegant enough to blend into a real workspace.

From there, the concept naturally expanded into a broader device:
a standalone panel that stays on, remains readable from a distance,
and works just as well in an office as it does in a control room,
recording studio, podcast setup, workshop, or public-facing space.

SoundPanel 7 is not just a sound meter.
It is a **local monitoring panel** for sound, time, network visibility, and connected integration.

![SoundPanel 7 product overview](docs/images/soundpanel7-hero-overview.jpg)

### 🚀 Key features

- Large 7" touchscreen display readable from a distance
- Real-time sound monitoring with **instant dB**, **Leq**, and **Peak**
- Large **NTP clock** with visible seconds
- Embedded web interface for setup and calibration
- **MQTT** publishing
- **Home Assistant** through MQTT Discovery or native custom integration
- **OTA** firmware updates
- Persistent settings storage

### 🎯 Use cases

- Noisy open space: make sound levels visible without turning into the noise police
- Recording studio: keep both sound level and precise time in view
- Broadcast or control room: display reliable network time with seconds
- Podcast or voice booth: follow timing and on-air references
- Workshop or public space: show a simple, readable ambient indicator
- Smart home wall panel: useful information, not just decoration

![SoundPanel 7 wall mounted in open space](docs/images/soundpanel7-wall-mounted-open-space.jpg)

![SoundPanel 7 in studio showing clock and sound levels](docs/images/soundpanel7-studio-clock-and-levels.jpg)

### 🛠️ Hardware target

#### 🧠 Main board

- **Waveshare ESP32-S3-Touch-LCD-7**
- 7" touchscreen
- ESP32-S3
- Wi-Fi
- PSRAM
- USB
- controllable backlight

#### 🎤 Audio input

The firmware is designed around an **analog microphone** connected to the sensor input.

Examples referenced in the project:

- `MAX4466`
- `MAX9814`
- similar analog microphone modules

By default, the project builds with a **mock audio mode** to make development easier without a real measurement chain.

In [`platformio.ini`](platformio.ini), this flag is enabled by default:

```ini
-DSOUNDPANEL7_MOCK_AUDIO=1
```

If you connect a real analog input, check that setting first before judging the panel's behavior.

### ⚡ Quick start

#### 1. Clone the repository

```bash
git clone https://github.com/jjtronics/SoundPanel7.git
cd SoundPanel7
```

#### 2. Prepare the environment

The easiest setup is:

- **VS Code**
- **PlatformIO** extension

You can also use the `pio` CLI if PlatformIO Core is already installed.

#### 3. Build

```bash
pio run
```

#### 4. Flash over USB

The default environment is `soundpanel7_usb`.

```bash
pio run -e soundpanel7_usb -t upload
```

#### 5. Open the serial monitor

```bash
pio device monitor -b 115200
```

At boot, the firmware initializes:

1. settings storage
2. display and touch
3. networking
4. OTA
5. MQTT
6. web interface
7. audio engine

### ⚙️ Default configuration

Current firmware defaults:

- hostname: `soundpanel7`
- NTP server: `fr.pool.ntp.org`
- POSIX timezone: `CET-1CEST,M3.5.0/2,M10.5.0/3`
- OTA enabled: `yes`
- OTA port: `3232`
- MQTT enabled: `no`
- MQTT base topic: `soundpanel7`
- analog pin: `GPIO6`
- default audio source: analog sensor

These settings are defined in [`src/SettingsStore.h`](src/SettingsStore.h).

### 🌐 Web interface

Once the device is connected to Wi-Fi:

- `http://DEVICE_IP/`
- `http://DEVICE_IP/admin`

The interface lets you configure:

- brightness
- color thresholds
- history duration
- NTP and timezone
- hostname
- OTA settings
- MQTT settings
- microphone calibration

![SoundPanel 7 live dashboard](docs/images/soundpanel7-dashboard-live-view.png)

![SoundPanel 7 admin web interface](docs/images/soundpanel7-admin-web-interface.png)

### 🕒 NTP clock

SoundPanel 7 is also a **large network clock**.

That is not a cosmetic extra.
In a studio, control room, podcast environment, or broadcast workflow,
having reliable time with visible seconds is often just as useful as the sound reading itself.

The firmware handles:

- automatic NTP synchronization
- configurable NTP server
- POSIX timezone support
- local `HH:MM` display with a seconds badge
- full time display in the web interface

The default NTP server is `fr.pool.ntp.org`.

![SoundPanel 7 studio clock and sound levels](docs/images/soundpanel7-studio-clock-and-levels.jpg)

### 📶 MQTT and Home Assistant

The panel can publish its metrics to an MQTT broker.

Available settings:

- host
- port
- username / password
- client ID
- base topic
- publish interval
- retain

Example topics:

```text
soundpanel7/db
soundpanel7/leq
soundpanel7/peak
soundpanel7/status
soundpanel7/uptime
soundpanel7/wifi/rssi
soundpanel7/wifi/ip
```

Home Assistant support is available in two ways.

#### Option 1: MQTT Discovery

If your Home Assistant setup already relies on MQTT, this is the quickest path.
Enable MQTT on the panel, configure the broker, and entities will be created automatically.

#### Option 2: Native Home Assistant integration

A custom integration is included in:

[`custom_components/soundpanel7`](custom_components/soundpanel7)

The firmware advertises:

```text
_soundpanel7._tcp.local.
```

The integration then queries the panel over HTTP, including `/api/status`.

Installation example:

```bash
mkdir -p /config/custom_components
cp -R custom_components/soundpanel7 /config/custom_components/
```

After that:

1. restart Home Assistant
2. restart SoundPanel 7
3. open `Settings > Devices & Services`
4. wait for auto-discovery

If the device does not show up, check:

- same local network on both sides
- `manifest.json` correctly copied
- panel reachable through its web UI
- mDNS not blocked by network setup or VLAN segmentation

![SoundPanel 7 Home Assistant discovery](docs/images/soundpanel7-home-assistant-discovery.png)

### 🚀 OTA updates

Once OTA is configured and enabled on the device:

```bash
pio run -e soundpanel7_ota -t upload
```

The OTA environment in [`platformio.ini`](platformio.ini) uses:

- `upload_protocol = espota`
- default port `3232`
- configurable OTA password

Remember to adjust `upload_port` to the real IP address of your device.

### 🎚️ Calibration workflow

The calibration system uses **3 reference points**.

Recommended process:

1. place a reference sound meter next to SoundPanel 7
2. generate or observe a stable sound level
3. enter the real measured value
4. capture all three reference points

Typical useful values:

- `45 dB`
- `65 dB`
- `85 dB`

![SoundPanel 7 calibration workflow](docs/images/soundpanel7-calibration-workflow.png)

The firmware stores those points and applies the correction curve accordingly.

### 🧠 Firmware architecture

The core flow remains intentionally simple:

```text
AudioEngine -> SharedHistory -> UI / Web / MQTT
                  |
                  -> data storage and playback
```

Main components:

- [`src/main.cpp`](src/main.cpp): global orchestration
- [`src/AudioEngine.cpp`](src/AudioEngine.cpp): audio acquisition and calculations
- [`src/ui/UiManager.cpp`](src/ui/UiManager.cpp): LVGL interface
- [`src/WebManager.cpp`](src/WebManager.cpp): HTTP, admin, live view
- [`src/MqttManager.cpp`](src/MqttManager.cpp): MQTT publishing and discovery
- [`src/OtaManager.cpp`](src/OtaManager.cpp): OTA updates
- [`src/SettingsStore.cpp`](src/SettingsStore.cpp): NVS persistence

### 📁 Project layout

```text
.
├── src/                       Main firmware
├── data/                      Embedded web assets
├── custom_components/         Home Assistant integration
├── include/                   Shared headers
└── platformio.ini             Build, flash, and environments
```

### 🔧 Project status

The project is already useful and operational, but still evolving:

- the audio chain still depends heavily on the real analog sensor hardware
- final accuracy depends on calibration quality
- product screenshots are still missing from the README
- some `platformio.ini` values are tuned for the current development machine

In short: this is a serious project, a living one, and already a very practical tool.

## 📜 License

Open-source project.  
Add the final repository license here if you want the README to state it explicitly.
