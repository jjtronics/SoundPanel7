# Agent Documentation Release - SoundPanel 7

Guide pour l'agent chargé de maintenir la documentation du processus de release (RELEASING.md).

## Votre rôle

Maintenir la documentation du workflow de release à jour pour que n'importe quel mainteneur puisse publier une nouvelle version de SoundPanel 7 de manière fiable et reproductible.

## Structure du RELEASING.md

Le fichier documente :
1. **Livrables versionnés** : firmware ESP32 + intégration Home Assistant
2. **Localisation des versions** : `AppConfig.h` et `manifest.json`
3. **Checklist de release** : étapes à suivre
4. **Création du tag Git**
5. **Création de la release GitHub**
6. **Assets de release** : firmware.bin, bootloader, partitions, manifest
7. **Structure du release-manifest.json**
8. **Notes de release suggérées**
9. **Note HACS**

## Quand mettre à jour RELEASING.md

### Changements nécessitant une mise à jour

✅ **Nouveau livrable** :
- Nouveau profil hardware nécessitant un firmware.bin distinct
- Nouvel artifact à publier (ex: filesystem.bin, documentation PDF)

✅ **Changement de workflow** :
- Nouveau step dans la checklist de release
- Nouvelle validation requise avant release
- Changement dans le processus de build

✅ **Modification du manifest** :
- Nouveau champ dans `release-manifest.json`
- Nouvelle méthode de vérification (ex: signature GPG)

✅ **Intégration CI/CD** :
- Ajout/modification de GitHub Actions
- Nouveau workflow d'automatisation

✅ **Breaking change** :
- Changement nécessitant une action de l'utilisateur
- Migration de données requise

❌ **Ne PAS mettre à jour pour** :
- Simple bump de version (fait automatiquement)
- Ajout de fonctionnalité sans impact sur le workflow de release
- Correction de bug mineur

## Workflow de mise à jour

### 1. Identifier l'impact sur le release

Questions à se poser :
- Le processus de build change-t-il ?
- Y a-t-il un nouveau fichier à publier ?
- La structure du manifest change-t-elle ?
- Une étape manuelle supplémentaire est-elle nécessaire ?

### 2. Sections du RELEASING.md à modifier

#### Ajout d'un nouveau profil hardware

**Exemple** : Support d'un nouveau ESP32-C6

1. **Mettre à jour "Current version"** :
```markdown
At the time of writing:
- `soundpanel7_usb` : v0.2.17
- `soundpanel7_headless_usb` : v0.2.17
- `soundpanel7_c6_usb` : v0.2.17 (NEW)
```

2. **Ajouter dans "Release checklist"** :
```markdown
3. Rebuild the release firmware:

```bash
pio run -e soundpanel7_ota
pio run -e soundpanel7_headless_ota
pio run -e soundpanel7_c6_ota  # NEW
```
```

3. **Ajouter dans "Release assets"** :
```markdown
- `.pio/build/soundpanel7_c6_ota/firmware.bin`
- `.pio/build/soundpanel7_c6_ota/bootloader.bin`
- `.pio/build/soundpanel7_c6_ota/partitions.bin`
```

4. **Mettre à jour "Release manifest"** :
```json
{
  "ota_c6": {
    "name": "soundpanel7_c6_ota-firmware.bin",
    "type": "firmware",
    "url": "https://github.com/jjtronics/SoundPanel7/releases/download/v0.2.17/soundpanel7_c6_ota-firmware.bin",
    "sha256": "..."
  }
}
```

#### Ajout d'une validation pré-release

**Exemple** : Vérification automatique des tests

Ajouter dans **"Release checklist"** après l'étape 1 :
```markdown
2. Run the test suite:

```bash
pio test -e native
```

Ensure all tests pass before proceeding.
```

#### Changement du manifest format

**Exemple** : Ajout de signatures

1. **Modifier "Release manifest"** :
```json
{
  "project": "SoundPanel7",
  "tag": "v0.2.17",
  "version": "0.2.17",
  "signature": "-----BEGIN PGP SIGNATURE-----...",  // NEW
  "ota": {
    "sha256": "...",
    "signature": "..."  // NEW per-file signature
  }
}
```

2. **Ajouter section explicative** :
```markdown
### Signature verification

Starting from v0.3.0, releases are GPG-signed for additional security.

The device verifies:
1. SHA-256 checksum of the firmware
2. GPG signature of the manifest (optional, if supported by hardware)

To verify manually:

```bash
gpg --verify release-manifest.json.sig release-manifest.json
```
```

#### Intégration GitHub Actions

**Exemple** : Automatisation complète du build

Ajouter une section **"Automated release workflow"** :
```markdown
## Automated release workflow (recommended)

If GitHub Actions is enabled, the release process is automated:

1. Update version in `AppConfig.h` and `manifest.json`
2. Commit the version bump
3. Create and push the Git tag:

```bash
git tag -a v0.2.17 -m "Release v0.2.17"
git push origin main
git push origin v0.2.17
```

4. GitHub Actions will automatically:
   - Build all firmware variants
   - Generate SHA-256 checksums
   - Create the release manifest
   - Attach all artifacts to the GitHub release

The workflow is defined in `.github/workflows/release.yml`.

### Manual release (fallback)

If GitHub Actions is not available, follow the manual process documented below.
```

### 3. Style d'écriture

**Ton** :
- Impératif et direct ("Update", "Rebuild", "Verify")
- Pas d'ambiguïté
- Checklist actionnable

**Format** :
```markdown
1. Step description
2. Command or action required

```bash
# Commented command
command --option value
```

3. Expected outcome or verification
```

**Exemples** :
✅ "Rebuild the release firmware for all hardware profiles"
✅ "Ensure the worktree is clean before proceeding"
✅ "Attach the generated artifacts to the GitHub release"

❌ "You might want to rebuild the firmware"
❌ "It's a good idea to check the worktree"
❌ "Consider attaching artifacts"

### 4. Maintenance des versions d'exemple

**Systématiquement mettre à jour** les versions dans les exemples :
- Remplacer `0.2.0` par la version actuelle dans tous les exemples
- Dates dans le manifest JSON
- URLs des releases

**Script de remplacement** :
```bash
# Après un release, mettre à jour les exemples
OLD_VERSION="0.2.0"
NEW_VERSION="0.2.17"

sed -i '' "s/$OLD_VERSION/$NEW_VERSION/g" RELEASING.md
```

## Checklist de mise à jour du RELEASING.md

### Avant de committer

- [ ] Les versions d'exemple reflètent la dernière release
- [ ] Tous les nouveaux livrables sont documentés
- [ ] Les commandes sont testées et fonctionnelles
- [ ] Le format du manifest est à jour
- [ ] La checklist de release est complète
- [ ] Les URLs d'exemple sont valides
- [ ] Pas de référence à des branches ou commits spécifiques
- [ ] Le workflow GitHub Actions est synchronisé avec la doc

### Test du processus

**Dry-run de release** :
1. Suivre le RELEASING.md à la lettre
2. Noter les étapes manquantes ou imprécises
3. Mettre à jour la documentation en conséquence

**Validation** :
```bash
# Vérifier que les paths mentionnés existent
ls .pio/build/soundpanel7_ota/firmware.bin
ls include/AppConfig.h
ls custom_components/soundpanel7/manifest.json

# Tester les commandes de build
pio run -e soundpanel7_ota
pio run -e soundpanel7_headless_ota

# Vérifier la structure du manifest JSON
cat .pio/build/soundpanel7_ota/release-manifest.json | jq .
```

## Exemples de mises à jour

### Exemple 1 : Ajout filesystem LittleFS

**Impact** : Nouveau fichier à publier dans la release

**Sections à modifier** :

1. **Release checklist** :
```markdown
3. Rebuild the release firmware and filesystem:

```bash
pio run -e soundpanel7_ota
pio run -e soundpanel7_headless_ota
pio run -e soundpanel7_ota -t buildfs  # NEW
```
```

2. **Release assets** :
```markdown
- `.pio/build/soundpanel7_ota/firmware.bin`
- `.pio/build/soundpanel7_ota/littlefs.bin`  # NEW
- `.pio/build/soundpanel7_ota/bootloader.bin`
```

3. **Ajouter note** :
```markdown
### Filesystem update

If the web UI or assets have been modified, also flash the filesystem:

```bash
pio run -e soundpanel7_usb -t uploadfs
```

For OTA updates, the filesystem is **not** updated automatically. Manual flash is required if UI changes.
```

### Exemple 2 : Migration NVS breaking

**Impact** : Utilisateurs doivent agir

**Ajouter dans "Release checklist"** :
```markdown
### Breaking changes in v0.3.0

⚠️ **NVS settings migration required**

Settings structure has changed. Users upgrading from v0.2.x must:

1. Export their configuration before upgrading:

```bash
curl http://soundpanel7.local/api/config/export > backup_v0.2.json
```

2. Upgrade to v0.3.0
3. Perform factory reset
4. Import the backup (automatic migration will apply)

Document this in the release notes.
```

### Exemple 3 : Nouveau workflow CI/CD

**Impact** : Processus de release automatisé

**Ajouter nouvelle section** :
```markdown
## GitHub Actions Workflow

The repository now includes automated release builds.

### Configuration

Ensure the following secrets are configured in GitHub repository settings:

- `RELEASE_TOKEN` : GitHub token with release write permissions

### Workflow triggers

The release workflow triggers on:
- Tag push matching `v*.*.*`
- Manual workflow dispatch

### Build matrix

The workflow builds:
- soundpanel7_ota (Waveshare 7")
- soundpanel7_headless_ota (ESP32-S3 standard)

All artifacts are automatically attached to the GitHub release.

### Workflow file

See `.github/workflows/release.yml` for the complete workflow definition.
```

## Structure d'une bonne checklist de release

**Format recommandé** :
```markdown
## Release checklist

### Pre-release

1. ✅ Ensure all tests pass
2. ✅ Update CHANGELOG.md with notable changes
3. ✅ Verify documentation is up-to-date

### Version bump

4. ✅ Update version in `include/AppConfig.h`
5. ✅ Update version in `custom_components/soundpanel7/manifest.json`
6. ✅ Commit the version bump

### Build

7. ✅ Rebuild all firmware variants
8. ✅ Verify builds complete without errors
9. ✅ Test at least one firmware variant on device

### Release

10. ✅ Create and push Git tag
11. ✅ Create GitHub release with tag
12. ✅ Attach build artifacts
13. ✅ Publish release notes

### Post-release

14. ✅ Verify OTA update works from previous version
15. ✅ Update RELEASING.md version examples (next release)
16. ✅ Announce release on communication channels
```

## Anti-patterns à éviter

❌ **Documenter des cas hypothétiques** : rester factuel sur le workflow actuel
❌ **Instructions vagues** : "Update files as needed" → lister les fichiers précis
❌ **Oublier les pré-requis** : documenter les outils nécessaires (PlatformIO version, etc.)
❌ **Exemples obsolètes** : toujours mettre à jour les versions après release
❌ **Omettre les validations** : chaque étape doit avoir un moyen de vérifier le succès

## Synchronisation avec le code

**Fichiers liés à surveiller** :
- `include/AppConfig.h` : version du firmware
- `custom_components/soundpanel7/manifest.json` : version intégration HA
- `.github/workflows/release.yml` : workflow CI/CD
- `platformio.ini` : environnements de build

**Lorsqu'un de ces fichiers change** :
→ Vérifier si RELEASING.md doit être mis à jour

## Ressources

- RELEASING.md actuel : toujours partir de l'existant
- GitHub Releases : https://github.com/jjtronics/SoundPanel7/releases
- PlatformIO build system : https://docs.platformio.org/
- GitHub Actions : https://docs.github.com/en/actions

## Template de section pour nouveau livrable

```markdown
### [Nom du livrable]

**Purpose**: [Pourquoi ce livrable existe]

**Build command**:
```bash
[Commande de build]
```

**Location**: `[Path relatif au fichier généré]`

**Verification**:
```bash
[Commande pour vérifier l'intégrité]
```

**Usage**: [Quand et comment utiliser ce livrable]

**Release manifest entry**:
```json
{
  "[key]": {
    "name": "[filename]",
    "type": "[type]",
    "url": "[download URL]",
    "sha256": "[checksum]"
  }
}
```
```

## Validation finale

Avant de committer les changements du RELEASING.md :

```bash
# 1. Vérifier que les commandes documentées fonctionnent
pio run -e soundpanel7_ota
pio run -e soundpanel7_headless_ota

# 2. Vérifier que les paths existent
ls include/AppConfig.h
ls custom_components/soundpanel7/manifest.json

# 3. Valider le format JSON du manifest
jq . .pio/build/soundpanel7_ota/release-manifest.json

# 4. Tester le workflow complet avec une version de test
# (sans push réel)
git tag -a v0.2.18-test -m "Test release process"
# Suivre le RELEASING.md
# Supprimer le tag de test
git tag -d v0.2.18-test
```
