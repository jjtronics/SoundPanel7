# Agent Documentation Utilisateur - SoundPanel 7

Guide pour l'agent chargé de maintenir le README.md à jour avec les nouvelles fonctionnalités.

## Votre rôle

Maintenir une documentation utilisateur claire, à jour et structurée pour les utilisateurs finaux de SoundPanel 7. Le README.md est le point d'entrée principal du projet et doit refléter fidèlement les capacités du firmware.

## Structure actuelle du README

Le README est **bilingue** (Français / English) avec les sections suivantes :

### Section Française
- Vision et présentation
- Aperçu visuel (photos du projet en situation)
- Points forts et cas d'usage
- Fonctionnalités détaillées (son, horloge, interfaces, connectivité)
- Matériel cible (Waveshare + headless)
- Démarrage rapide (clone, build, flash, OTA)
- Configuration par défaut
- Interface web (dashboards, paramètres, API)
- Sécurité (authentification, PIN)
- Wi-Fi multi-AP
- Gestion de configuration (export/import/backup)
- Horloge NTP
- MQTT
- Notifications sortantes (Slack, Telegram, WhatsApp)
- Home Assistant (MQTT Discovery + intégration native)
- OTA (réseau + GitHub)
- Calibration
- Architecture firmware
- État du projet

### Section Anglaise
Structure identique, traduction fidèle.

## Workflow de mise à jour

### 1. Identifier l'impact utilisateur

Avant de modifier le README, déterminer :
- **Feature ajoutée** : Quelle nouvelle capacité pour l'utilisateur ?
- **Modification de workflow** : Les étapes d'utilisation changent-elles ?
- **Nouveaux paramètres** : Y a-t-il de nouveaux réglages dans l'interface ?
- **Changement de comportement** : Une fonctionnalité existante change-t-elle ?
- **Breaking change** : Nécessite-t-il une action de l'utilisateur ?

### 2. Sections concernées typiquement

**Nouvelle source audio** → Mettre à jour :
- Section "Fonctionnalités > Monitoring sonore"
- Section "Entrée audio" (liste des sources)
- Section "Câblage audio par carte" (si nouveau hardware)
- Tableaux de GPIO mapping

**Nouveau mode TARDIS** → Mettre à jour :
- Section "Variantes matérielles supportées" (builds headless)
- Section "Interface web" (bloc Mode TARDIS)
- Ajouter captures d'écran si applicable

**Nouvel endpoint API** → Mettre à jour :
- Section "Interface web" (liste des endpoints)
- Documenter le format de requête/réponse si complexe

**Nouvelle intégration** → Mettre à jour :
- Section "Fonctionnalités > Connectivité"
- Ajouter une section dédiée si intégration majeure
- Mettre à jour le schéma de flux si nécessaire

### 3. Style d'écriture

**Ton** :
- Direct et factuel
- Technique mais accessible
- Pas de marketing excessif
- Utiliser des exemples concrets

**Format** :
```markdown
### 🎯 Titre clair

Paragraphe d'introduction concis.

**Points clés** :
- Point 1
- Point 2

Exemple de commande ou configuration :
```bash
pio run -e soundpanel7_usb -t upload
```

<p align="center">
  <img src="path/to/screenshot.jpg" alt="Description précise" width="900">
</p>
```

**Emojis utilisés** :
- 🎚️ : Calibration, réglages audio
- 🔊 : Monitoring sonore
- 🕒 : Horloge, temps
- 🖥️ : Interface locale
- 🌐 : Interface web
- 📡 : Connectivité, réseau
- 🩺 : Diagnostics
- 🛠️ : Matériel
- 🔗 : Liens
- 🎤 : Audio, micro
- 🔌 : Câblage
- ⚡ : Démarrage rapide
- ⚙️ : Configuration
- 📶 : MQTT
- 🚨 : Alertes, notifications
- 🏠 : Home Assistant
- 🚀 : OTA

### 4. Captures d'écran

**Quand ajouter une capture** :
- Nouvelle page dans l'interface web
- Nouveau mode visuel sur l'écran tactile
- Changement UI majeur

**Bonnes pratiques** :
- Résolution adaptée (900px width pour full, 420-500px pour détails)
- Captures claires, pas de reflets
- Alt text descriptif pour l'accessibilité
- Stockage dans `MEDIAS/WEBUI/` ou `MEDIAS/HARDWARE/`

**Format de référence** :
```markdown
<p align="center">
  <img src="MEDIAS/WEBUI/WebUI-Dashboard-NewFeature.jpg" alt="Dashboard nouvelle fonctionnalité de SoundPanel 7" width="900">
</p>

<p align="center">
  <em>Légende explicative de la capture.</em>
</p>
```

### 5. Tableaux de référence

**Maintenir à jour** :
- Tableau de câblage audio (Waveshare + headless)
- Liste des endpoints API
- Topics MQTT publiés
- Capteurs Home Assistant exposés

**Format cohérent** :
```markdown
| Colonne 1 | Colonne 2 | Colonne 3 |
| --- | --- | --- |
| Valeur A | Valeur B | Valeur C |
```

### 6. Exemples de commandes

**Toujours fournir** :
- Commandes complètes, prêtes à copier-coller
- Alternatives si plusieurs méthodes existent
- Commentaires pour clarifier les options

**Exemple** :
```markdown
```bash
# Build par défaut (soundpanel7_usb)
pio run

# Build explicite par profil
pio run -e soundpanel7_usb
pio run -e soundpanel7_headless_usb
```
```

### 7. Cohérence bilingue

**Processus** :
1. Modifier d'abord la section française
2. Traduire fidèlement en anglais
3. Vérifier que les exemples de code sont identiques
4. Adapter les URLs si nécessaire (liens vers documentation externe)

**Attention** :
- Garder les noms techniques en anglais (ex: "OTA", "MQTT", "NTP")
- Traduire les descriptions et explications
- Conserver la même structure de sections

## Checklist de mise à jour

### Avant de committer

- [ ] Les deux versions (FR + EN) sont mises à jour
- [ ] Les captures d'écran sont à jour si applicable
- [ ] Les tableaux de référence sont cohérents avec le code
- [ ] Les exemples de commandes sont testés et fonctionnels
- [ ] Les liens internes fonctionnent (ancres de sections)
- [ ] Pas de typo (relecture)
- [ ] Le TOC (table des matières) est à jour si ajout de section
- [ ] Les emojis sont utilisés de manière cohérente

### Validation

```bash
# Vérifier les liens internes
# Ouvrir README.md dans un viewer Markdown
# Tester que les ancres de navigation fonctionnent

# Vérifier que les images existent
find MEDIAS/ -name "*.jpg" -o -name "*.jpeg" -o -name "*.png" -o -name "*.gif"

# Si nouvelle commande documentée, la tester
pio run -e soundpanel7_usb
```

## Exemples de mises à jour

### Exemple 1 : Nouvelle source audio "I2S Microphone Array"

**Sections à modifier** :

1. **Fonctionnalités > Monitoring sonore** :
```markdown
- choix de la source audio : `Demo`, `Analog Mic`, `PDM MEMS`, `INMP441`, `I2S Array`
```

2. **Entrée audio** :
```markdown
- `I2S Array` : matrice de microphones I2S pour beam forming
```

3. **Câblage audio par carte** :
Ajouter une ligne dans les tableaux :
```markdown
| I2S Array | `SCK / BCLK` | `GPIO12` | `SCK` |
| I2S Array | `WS / LRCL` | `GPIO11` | `MOSI` |
| I2S Array | `SD` | `GPIO13` | `MISO` |
| I2S Array | `SELECT` | `GPIO14` | `CS` |
```

4. **Répéter en anglais**

### Exemple 2 : Nouveau dashboard "Analyse spectrale"

**Sections à modifier** :

1. **Affichage du dashboard** :
```markdown
- choisir la vue affichée : principal, horloge, LIVE, sonomètre, **analyse spectrale**
```

2. **Ajouter capture d'écran** :
```markdown
<p align="center">
  <img src="MEDIAS/WEBUI/WebUI-Dashboard-Spectral.jpg" alt="Dashboard analyse spectrale" width="900">
</p>
```

3. **Interface web > Endpoints** (si nouvel endpoint) :
```markdown
GET   /api/spectral
POST  /api/spectral/config
```

### Exemple 3 : Support Prometheus metrics

**Sections à modifier** :

1. **Fonctionnalités > Connectivité** :
```markdown
- export de métriques Prometheus
```

2. **Ajouter nouvelle section dédiée** :
```markdown
### 📊 Métriques Prometheus

Le firmware expose des métriques au format Prometheus sur :

```text
http://soundpanel7.local/metrics
```

Métriques disponibles :
- `soundpanel7_db_instant`
- `soundpanel7_leq`
- `soundpanel7_peak`
- `soundpanel7_heap_free`
- `soundpanel7_uptime_seconds`

Configuration Prometheus :

```yaml
scrape_configs:
  - job_name: 'soundpanel7'
    static_configs:
      - targets: ['soundpanel7.local:80']
```
```

## Références

- README actuel : `README.md` (toujours partir de l'existant)
- Captures d'écran : `MEDIAS/WEBUI/` et `MEDIAS/HARDWARE/`
- Structure du firmware : `AI.md` (pour comprendre l'architecture)
- Workflow de release : `RELEASING.md` (pour les breaking changes)

## Anti-patterns à éviter

❌ **Documenter du code en développement** : attendre que la feature soit mergée
❌ **Copier-coller sans adapter** : chaque section a son ton et son contexte
❌ **Oublier la traduction anglaise** : le README doit toujours être bilingue
❌ **Screenshots obsolètes** : les mettre à jour si l'UI change
❌ **Commandes non testées** : toujours valider les exemples
❌ **Jargon technique excessif** : rester accessible aux makers

## Tone and Voice

**Ce que le README est** :
- Un guide pratique pour les utilisateurs
- Une documentation de référence
- Une vitrine du projet

**Ce que le README n'est pas** :
- Un manuel de développement (→ AI.md, AGENT_DEV.md)
- Une spec technique exhaustive (→ code source)
- Un tutoriel détaillé étape par étape (→ blog externe si nécessaire)

Le README doit permettre à un maker de :
1. Comprendre ce que fait SoundPanel 7
2. Savoir quel matériel acheter
3. Compiler et flasher le firmware
4. Configurer et utiliser l'appareil
5. Intégrer avec Home Assistant si souhaité
