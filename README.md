# SoundPanel 7

Connected 7" wall panel for ambient sound monitoring, precise NTP time, local web control, MQTT, OTA, and Home Assistant.

- Hardware target: `Waveshare ESP32-S3-Touch-LCD-7`
- Firmware stack: Arduino + PlatformIO + LVGL 8
- Home Assistant: MQTT Discovery or custom native integration

Navigation: [Français](#fr) · [English](#en)

---

<a id="fr"></a>

## Français

### Vue d'ensemble

SoundPanel 7 est un panneau mural autonome qui affiche en continu :

- le niveau sonore ambiant
- l'heure réseau synchronisée via NTP
- l'état local du panneau, du Wi-Fi et des services

Le projet est conçu pour être lisible de loin, pilotable au doigt sur l'écran 7", administrable depuis un navigateur, et intégrable proprement dans un environnement Home Assistant ou MQTT.

### Fonctionnalités actuelles

#### Affichage local

- interface tactile LVGL avec 5 pages : vue générale, horloge, son, calibration, réglages
- grande horloge avec secondes
- affichage `dB instantané`, `Leq` et `Peak`
- historique glissant configurable
- indicateurs visuels d'alerte avec maintien configurable en orange et rouge
- extinction de l'écran et arrêt du panneau depuis l'interface

#### Audio et calibration

- entrée audio analogique sur `GPIO6` par défaut
- mode de réponse `Fast` ou `Slow`
- taille d'échantillonnage RMS configurable
- maintien du peak configurable
- calibration micro en `3` ou `5` points
- durée de capture de calibration configurable
- offsets de secours conservés dans la configuration
- mode audio mock activé par défaut à la compilation pour faciliter le développement

#### Réseau et services

- connexion Wi-Fi via portail `WiFiManager`
- hostname configurable
- mDNS avec annonce `_soundpanel7._tcp.local.`
- synchronisation NTP avec serveur, timezone et intervalle configurables
- OTA via `espota`
- publication MQTT avec auto-reconnexion
- MQTT Discovery pour Home Assistant

#### Interface web et API

- interface web embarquée sur le port `80`
- flux live SSE sur le port `81`
- page de supervision temps réel
- page de réglages et d'administration
- export, import, backup et restore de configuration
- reset partiel par section (`ui`, `time`, `audio`, `calibration`, `ota`, `mqtt`)
- actions système : reboot, shutdown, factory reset
- diagnostics système : uptime, température MCU, version firmware, environnement de build, état MQTT/OTA, charge LVGL, heap, nombre d'objets LVGL, timestamp du dernier backup

### Matériel cible

- carte : `Waveshare ESP32-S3-Touch-LCD-7`
- écran tactile 7"
- ESP32-S3 avec PSRAM
- Wi-Fi
- rétroéclairage pilotable
- entrée micro analogique via `Sensor AD`

Modules micro analogiques typiques :

- `MAX4466`
- `MAX9814`
- équivalent analogique

### Démarrage rapide

#### Prérequis

- `VS Code` + extension `PlatformIO`, ou `pio` en CLI
- câble USB pour le premier flash

#### Build

```bash
pio run
```

L'environnement par défaut est `soundpanel7_usb`.

#### Flash USB

```bash
pio run -e soundpanel7_usb -t upload
```

#### Moniteur série

```bash
pio device monitor -b 115200
```

#### Flash OTA

```bash
pio run -e soundpanel7_ota -t upload
```

À ajuster avant usage :

- `upload_port` dans `platformio.ini`
- `monitor_port` dans `platformio.ini`
- mot de passe OTA si activé

### Configuration par défaut

Valeurs par défaut du firmware :

- hostname : `soundpanel7`
- serveur NTP : `fr.pool.ntp.org`
- timezone : `CET-1CEST,M3.5.0/2,M10.5.0/3`
- intervalle NTP : `180 min`
- OTA : activé sur le port `3232`
- MQTT : désactivé
- topic MQTT racine : `soundpanel7`
- audio source : `Sensor Analog`
- pin analogique : `GPIO6`
- RMS samples : `256`
- réponse audio : `Fast`
- peak hold : `5000 ms`
- calibration : `3 points`, capture `3 s`

Note importante : le firmware est configuré avec `audioSource = Sensor Analog`, mais le build active aussi `-DSOUNDPANEL7_MOCK_AUDIO=1` par défaut. Pour une mesure réelle, il faut vérifier ce flag dans [`platformio.ini`](platformio.ini).

### Interface web et API

Accès :

- interface : `http://IP_DU_PANNEAU/`
- flux live : `http://IP_DU_PANNEAU:81/api/events`
- mDNS : `http://soundpanel7.local/` si le réseau le permet

Endpoints principaux :

- `GET /api/status`
- `POST /api/ui`
- `GET|POST /api/time`
- `GET /api/config/export`
- `POST /api/config/import`
- `POST /api/config/backup`
- `POST /api/config/restore`
- `POST /api/config/reset_partial`
- `GET|POST /api/ota`
- `GET|POST /api/mqtt`
- `POST /api/calibrate`
- `POST /api/calibrate/clear`
- `POST /api/calibrate/mode`
- `POST /api/reboot`
- `POST /api/shutdown`
- `POST /api/factory_reset`

Le `GET /api/status` expose notamment :

- mesures audio
- historique
- uptime
- état Wi-Fi et RSSI
- heure courante
- température MCU
- état OTA / MQTT
- version firmware et environnement de build
- statistiques runtime LVGL / heap

### MQTT

Réglages disponibles :

- `host`
- `port`
- `username`
- `password`
- `clientId`
- `baseTopic`
- `publishPeriodMs`
- `retain`

Topics publiés :

```text
soundpanel7/availability
soundpanel7/state
soundpanel7/db
soundpanel7/leq
soundpanel7/peak
soundpanel7/wifi/rssi
soundpanel7/wifi/ip
```

Avec MQTT Discovery, le firmware publie automatiquement les entités Home Assistant suivantes :

- `dB Instant`
- `Leq`
- `Peak`
- `WiFi RSSI`
- `WiFi IP`

### Home Assistant

Deux approches sont disponibles.

#### Option 1 : MQTT Discovery

Activer MQTT sur le panneau, renseigner le broker, puis laisser Home Assistant découvrir les entités.

#### Option 2 : intégration native

Le dépôt contient une intégration custom dans [`custom_components/soundpanel7`](custom_components/soundpanel7).

Installation :

```bash
mkdir -p /config/custom_components
cp -R custom_components/soundpanel7 /config/custom_components/
```

Puis :

1. redémarrer Home Assistant
2. redémarrer le panneau
3. ouvrir `Paramètres > Appareils et services`
4. attendre la découverte Zeroconf

Service annoncé par le firmware :

```text
_soundpanel7._tcp.local.
```

Chemin API utilisé par l'intégration native :

```text
/api/status
```

Capteurs exposés par l'intégration native :

- `dB Instant`
- `Leq`
- `Peak`
- `WiFi RSSI`
- `WiFi IP`
- `Uptime`

### Architecture

Flux principal :

```text
AudioEngine -> SharedHistory -> UI / Web / MQTT
```

Composants clés :

- [`src/main.cpp`](src/main.cpp)
- [`src/AudioEngine.cpp`](src/AudioEngine.cpp)
- [`src/ui/UiManager.cpp`](src/ui/UiManager.cpp)
- [`src/WebManager.cpp`](src/WebManager.cpp)
- [`src/MqttManager.cpp`](src/MqttManager.cpp)
- [`src/OtaManager.cpp`](src/OtaManager.cpp)
- [`src/SettingsStore.cpp`](src/SettingsStore.cpp)
- [`custom_components/soundpanel7`](custom_components/soundpanel7)

### Arborescence

```text
.
├── include/                    Headers partagés
├── src/                        Firmware principal
├── custom_components/          Intégration Home Assistant
├── assets/                     Polices
├── platformio.ini              Build et environnements
└── README.md
```

### État du projet

Le projet est utilisable aujourd'hui, avec quelques points à garder en tête :

- la précision finale dépend toujours de la chaîne analogique réelle et de la calibration
- `platformio.ini` contient encore des ports USB/IP propres à la machine de dev
- le dépôt ne contient pas actuellement de captures ou de dossier `docs/`
- aucun fichier de licence n'est présent dans le dépôt pour l'instant

---

<a id="en"></a>

## English

### Overview

SoundPanel 7 is a standalone 7" wall panel that continuously shows:

- ambient sound level
- NTP-synchronized time
- local device, Wi-Fi, and service status

It is designed to stay readable from a distance, usable directly from the touchscreen, manageable from a browser, and easy to integrate with MQTT or Home Assistant.

### Current features

#### Local UI

- LVGL touchscreen UI with 5 pages: overview, clock, sound, calibration, settings
- large clock with visible seconds
- `instant dB`, `Leq`, and `Peak`
- configurable rolling history
- visual alert states with configurable orange/red hold time
- screen off and device shutdown from the interface

#### Audio and calibration

- analog audio input on `GPIO6` by default
- `Fast` or `Slow` response mode
- configurable RMS sampling window
- configurable peak hold
- microphone calibration in `3` or `5` points
- configurable calibration capture duration
- fallback offsets stored in settings
- mock audio mode enabled by default at build time for easier development

#### Network and services

- Wi-Fi connection through `WiFiManager`
- configurable hostname
- mDNS advertisement on `_soundpanel7._tcp.local.`
- configurable NTP server, timezone, and sync interval
- OTA updates through `espota`
- MQTT publishing with auto reconnect
- Home Assistant MQTT Discovery

#### Web UI and API

- embedded web UI on port `80`
- live SSE stream on port `81`
- real-time dashboard
- settings and admin page
- config export, import, backup, and restore
- partial reset by scope (`ui`, `time`, `audio`, `calibration`, `ota`, `mqtt`)
- system actions: reboot, shutdown, factory reset
- system diagnostics: uptime, MCU temperature, firmware version, build environment, MQTT/OTA status, LVGL load, heap usage, LVGL object count, last backup timestamp

### Target hardware

- board: `Waveshare ESP32-S3-Touch-LCD-7`
- 7" touchscreen
- ESP32-S3 with PSRAM
- Wi-Fi
- controllable backlight
- analog microphone input through `Sensor AD`

Typical analog microphone modules:

- `MAX4466`
- `MAX9814`
- similar analog front-end

### Quick start

#### Requirements

- `VS Code` + `PlatformIO`, or `pio` CLI
- USB cable for first flash

#### Build

```bash
pio run
```

Default environment: `soundpanel7_usb`

#### Flash over USB

```bash
pio run -e soundpanel7_usb -t upload
```

#### Serial monitor

```bash
pio device monitor -b 115200
```

#### Flash over OTA

```bash
pio run -e soundpanel7_ota -t upload
```

Adjust before use:

- `upload_port` in `platformio.ini`
- `monitor_port` in `platformio.ini`
- OTA password if enabled

### Default configuration

Firmware defaults:

- hostname: `soundpanel7`
- NTP server: `fr.pool.ntp.org`
- timezone: `CET-1CEST,M3.5.0/2,M10.5.0/3`
- NTP sync interval: `180 min`
- OTA: enabled on port `3232`
- MQTT: disabled
- MQTT base topic: `soundpanel7`
- audio source: `Sensor Analog`
- analog pin: `GPIO6`
- RMS samples: `256`
- audio response: `Fast`
- peak hold: `5000 ms`
- calibration: `3 points`, `3 s` capture

Important note: the runtime default is `Sensor Analog`, but the build also enables `-DSOUNDPANEL7_MOCK_AUDIO=1` by default. Check [`platformio.ini`](platformio.ini) before evaluating real-world measurements.

### Web UI and API

Access:

- UI: `http://DEVICE_IP/`
- live stream: `http://DEVICE_IP:81/api/events`
- mDNS: `http://soundpanel7.local/` if your network supports it

Main endpoints:

- `GET /api/status`
- `POST /api/ui`
- `GET|POST /api/time`
- `GET /api/config/export`
- `POST /api/config/import`
- `POST /api/config/backup`
- `POST /api/config/restore`
- `POST /api/config/reset_partial`
- `GET|POST /api/ota`
- `GET|POST /api/mqtt`
- `POST /api/calibrate`
- `POST /api/calibrate/clear`
- `POST /api/calibrate/mode`
- `POST /api/reboot`
- `POST /api/shutdown`
- `POST /api/factory_reset`

`GET /api/status` includes:

- audio metrics
- history snapshot
- uptime
- Wi-Fi status and RSSI
- current time
- MCU temperature
- OTA / MQTT state
- firmware version and build environment
- LVGL / heap runtime stats

### MQTT

Available settings:

- `host`
- `port`
- `username`
- `password`
- `clientId`
- `baseTopic`
- `publishPeriodMs`
- `retain`

Published topics:

```text
soundpanel7/availability
soundpanel7/state
soundpanel7/db
soundpanel7/leq
soundpanel7/peak
soundpanel7/wifi/rssi
soundpanel7/wifi/ip
```

With MQTT Discovery, the firmware publishes these Home Assistant entities:

- `dB Instant`
- `Leq`
- `Peak`
- `WiFi RSSI`
- `WiFi IP`

### Home Assistant

Two integration paths are available.

#### Option 1: MQTT Discovery

Enable MQTT on the panel, configure your broker, and let Home Assistant create the entities automatically.

#### Option 2: native custom integration

The repository ships a custom integration in [`custom_components/soundpanel7`](custom_components/soundpanel7).

Install it with:

```bash
mkdir -p /config/custom_components
cp -R custom_components/soundpanel7 /config/custom_components/
```

Then:

1. restart Home Assistant
2. restart the panel
3. open `Settings > Devices & Services`
4. wait for Zeroconf discovery

Advertised service:

```text
_soundpanel7._tcp.local.
```

Default API path used by the native integration:

```text
/api/status
```

Sensors exposed by the native integration:

- `dB Instant`
- `Leq`
- `Peak`
- `WiFi RSSI`
- `WiFi IP`
- `Uptime`

### Architecture

Main flow:

```text
AudioEngine -> SharedHistory -> UI / Web / MQTT
```

Key components:

- [`src/main.cpp`](src/main.cpp)
- [`src/AudioEngine.cpp`](src/AudioEngine.cpp)
- [`src/ui/UiManager.cpp`](src/ui/UiManager.cpp)
- [`src/WebManager.cpp`](src/WebManager.cpp)
- [`src/MqttManager.cpp`](src/MqttManager.cpp)
- [`src/OtaManager.cpp`](src/OtaManager.cpp)
- [`src/SettingsStore.cpp`](src/SettingsStore.cpp)
- [`custom_components/soundpanel7`](custom_components/soundpanel7)

### Project layout

```text
.
├── include/                    Shared headers
├── src/                        Main firmware
├── custom_components/          Home Assistant integration
├── assets/                     Fonts
├── platformio.ini              Build environments
└── README.md
```

### Project status

The project is already usable, with a few practical caveats:

- final measurement accuracy still depends on the analog front-end and calibration quality
- `platformio.ini` still contains USB/IP values from the current development machine
- the repository currently does not include screenshots or a `docs/` folder
- no license file is present in the repository yet
