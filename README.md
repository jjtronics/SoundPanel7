
# 🎚️ SoundPanel 7

> Sonomètre connecté avec écran tactile 7", historique temps réel, MQTT, OTA et interface Web.

**SoundPanel 7** est un sonomètre connecté open-source basé sur **ESP32-S3** et un **écran tactile 7 pouces**.  
Il permet de mesurer le niveau sonore ambiant, d’afficher un tableau de bord en temps réel et d’intégrer les données dans un système domotique ou de supervision via **MQTT**.

Le projet est conçu pour être **simple à installer**, **hautement configurable**, et **intégrable dans des environnements professionnels** (open space, studio, broadcast, etc.).

---

# 🇫🇷 Description

SoundPanel 7 est un **panneau de monitoring sonore autonome** capable de :

- afficher le niveau sonore instantané
- calculer le **Leq** et le **Peak**
- afficher un **historique graphique**
- publier les mesures via **MQTT**
- être configuré via une **interface web**
- être mis à jour via **OTA**
- être calibré avec un sonomètre de référence

L’interface tactile permet également de consulter les informations système et de gérer l’appareil directement depuis l’écran.

---

# 🇬🇧 Description

SoundPanel 7 is an **open-source connected sound level monitor** powered by an **ESP32-S3** with a **7" touchscreen display**.

It provides:

- real-time sound level monitoring
- **Leq** and **Peak** calculations
- historical sound level graph
- **MQTT publishing**
- full **web configuration interface**
- **OTA firmware updates**
- microphone calibration with a reference sound meter

The touchscreen interface allows direct device interaction without requiring a browser.

---

# 📸 Screenshots

*(à ajouter)*

### Dashboard

![dashboard screenshot](docs/dashboard.png)

### Admin interface

![admin screenshot](docs/admin.png)

### Calibration

![calibration screenshot](docs/calibration.png)

---

# ✨ Fonctionnalités

## 🎚️ Monitoring audio

- mesure du niveau sonore instantané
- calcul **Leq**
- calcul **Peak**
- historique graphique configurable
- zones visuelles (vert / orange / rouge)

---

## 📊 Historique

- graphique temps réel
- durée configurable (1 à 60 minutes)
- rendu adaptatif
- zones de seuil colorées

---

## 🌐 Interface Web

Interface accessible depuis n'importe quel navigateur.

Permet de configurer :

- luminosité écran
- seuils dB
- durée historique
- serveur NTP
- hostname
- OTA
- MQTT
- calibration micro

---

## 📡 MQTT

Publication des métriques audio vers un broker MQTT.

Exemples de topics :

soundpanel7/db  
soundpanel7/leq  
soundpanel7/peak  
soundpanel7/status  
soundpanel7/uptime  

Paramètres configurables :

- broker
- port
- credentials
- topic base
- publish interval
- retain flag

---

## 🔧 Calibration

Calibration du microphone en **3 points** :

1. placer un sonomètre de référence  
2. entrer la valeur réelle  
3. capturer le point  

Le système calcule automatiquement la courbe de correction.

---

## 🕒 NTP & horloge

Synchronisation automatique via NTP.

Configuration :

- serveur NTP
- timezone POSIX
- hostname

---

## 🚀 OTA

Mise à jour firmware **Over The Air**.

Paramètres :

- activation OTA
- port OTA
- hostname OTA
- mot de passe OTA

---

## ⚙️ Configuration persistante

Toutes les configurations sont sauvegardées dans la mémoire flash :

- UI
- calibration
- MQTT
- OTA
- réseau
- NTP

---

# 🖥️ Hardware

## Carte principale

Compatible avec :

Waveshare ESP32-S3-Touch-LCD-7

Caractéristiques :

- écran tactile 7"
- ESP32-S3
- PSRAM
- WiFi
- USB
- rétroéclairage contrôlable

---

## Microphone

Entrée analogique :

MAX4466 / MAX9814 / microphone analogique équivalent

---

## Alimentation

USB-C  
5V

---

# 📦 Installation

## 1️⃣ Cloner le projet

git clone https://github.com/yourusername/soundpanel7.git  
cd soundpanel7

---

## 2️⃣ Installer PlatformIO

Extension recommandée :

VSCode + PlatformIO

---

## 3️⃣ Compiler

pio run

---

## 4️⃣ Flash USB

pio run -t upload

---

## 5️⃣ Monitor série

pio device monitor

---

# 📡 OTA

Après configuration OTA dans l’interface Web :

pio run -e soundpanel7_ota -t upload

---

# 🌐 Interface Web

Une fois connecté au WiFi :

http://IP_DU_DEVICE

Admin :

http://IP_DU_DEVICE/admin

---

# 📊 MQTT Integration

Example payloads:

soundpanel7/db -> 54.3  
soundpanel7/leq -> 52.1  
soundpanel7/peak -> 61.2  

---

# 📁 Structure du projet

src/
 ├── main.cpp  
 ├── WebManager.cpp  
 ├── UiManager.cpp  
 ├── AudioEngine.cpp  
 ├── MqttManager.cpp  
 ├── NetManager.cpp  

include/
 ├── settings.h  
 ├── webmanager.h  
 ├── audiomanager.h  

---

# 🧠 Architecture

AudioEngine  
     ↓  
UiManager (LVGL)  
     ↓  
WebManager (HTTP)  
     ↓  
MqttManager  

---

# 🧪 Calibration recommandée

Utiliser un sonomètre de référence.

Points typiques :

40 dB  
65 dB  
85 dB  

---

# 🔐 Sécurité

Possibilité de configurer :

- mot de passe OTA
- credentials MQTT

---

# 🛠️ Développement

Le projet utilise :

- ESP32 Arduino Core
- LVGL
- PlatformIO
- ESPAsyncWebServer

---

# 🤝 Contribution

Les contributions sont les bienvenues.

Fork → Pull Request.

---

# 📜 Licence

Projet open-source.

Licence : MIT

---

# 🙏 Remerciements

Merci aux projets suivants :

- ESP32
- LVGL
- PlatformIO
- ESPAsyncWebServer

---

# ⭐ Support

Si le projet vous est utile :

⭐ Star le repo  
🍴 Fork  
🐛 Report issues  

---

# 📬 Contact

Créé par **Jean‑Jacques Castellotti**
