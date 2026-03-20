# Agent Release - SoundPanel 7

Guide pour l'agent chargé d'exécuter le workflow de release complet : version bump, push, tag, GitHub release, et monitoring CI/CD.

## Votre rôle

Automatiser et orchestrer le processus de publication d'une nouvelle version de SoundPanel 7 sur GitHub, depuis le bump de version jusqu'à la validation complète de la release par les GitHub Actions.

## Différence avec AGENT_DOCS_RELEASE

| Agent | Rôle |
|-------|------|
| **AGENT_DOCS_RELEASE** | Maintenir la **documentation** du process (RELEASING.md) |
| **AGENT_RELEASE** (ce fichier) | **Exécuter** le workflow de release |

**Analogie** :
- AGENT_DOCS_RELEASE = Le manuel d'instructions
- AGENT_RELEASE = Le chef d'orchestre qui suit le manuel

## Workflow de release

### Vue d'ensemble

```
┌─────────────────────────────────────────────────┐
│ Release Workflow                                │
├─────────────────────────────────────────────────┤
│ 1. Pre-release checks                           │
│ 2. Version bump (AppConfig.h + manifest.json)   │
│ 3. Commit + Push                                │
│ 4. Tag creation + Push                          │
│ 5. GitHub Release creation                      │
│ 6. Monitor GitHub Actions                       │
│ 7. Notification                                 │
└─────────────────────────────────────────────────┘
```

### Étapes détaillées

#### 1. Pre-release checks

**Objectif** : Vérifier que le projet est prêt pour release.

**Checklist** :
```bash
# Worktree propre
git diff --quiet && git diff --cached --quiet
# Si erreur → commit ou stash les changements

# Branch main
git rev-parse --abbrev-ref HEAD
# Doit être "main"

# Sync avec remote
git fetch origin
git status
# Doit être "up to date with origin/main"

# Tests passent (si configurés)
pio test -e native
# Doit retourner 0

# Build réussi
pio run -e soundpanel7_ota
pio run -e soundpanel7_headless_ota
# Doit compiler sans erreur
```

**Vérifications manuelles** :
- [ ] CHANGELOG.md mis à jour avec les changements
- [ ] README.md à jour si nouvelles features
- [ ] Breaking changes documentés
- [ ] Tests manuels effectués sur hardware

#### 2. Version bump

**Objectif** : Incrémenter la version dans tous les fichiers requis.

**Fichiers à modifier** :

1. **`include/AppConfig.h`** :
```cpp
// Ligne ~6
#define SOUNDPANEL7_VERSION "0.2.18"
```

2. **`custom_components/soundpanel7/manifest.json`** :
```json
{
  "version": "0.2.18",
  ...
}
```

**Script automatisé** :
```bash
#!/bin/bash
set -e

NEW_VERSION="$1"

if [ -z "$NEW_VERSION" ]; then
  echo "Usage: $0 <version>"
  echo "Example: $0 0.2.18"
  exit 1
fi

echo "Bumping version to $NEW_VERSION..."

# Update AppConfig.h
sed -i '' "s/#define SOUNDPANEL7_VERSION \".*\"/#define SOUNDPANEL7_VERSION \"$NEW_VERSION\"/" include/AppConfig.h

# Update manifest.json
sed -i '' "s/\"version\": \".*\"/\"version\": \"$NEW_VERSION\"/" custom_components/soundpanel7/manifest.json

echo "Version bumped to $NEW_VERSION"
git diff include/AppConfig.h custom_components/soundpanel7/manifest.json
```

**Commande** :
```bash
./scripts/bump_version.sh 0.2.18
```

#### 3. Commit + Push

**Objectif** : Commit le version bump et push sur main.

**Commande** :
```bash
VERSION="0.2.18"

git add include/AppConfig.h custom_components/soundpanel7/manifest.json
git commit -m "Release v${VERSION}"
git push origin main
```

**Vérification** :
```bash
# Commit pushed
git log origin/main -1 --oneline
# Doit afficher "Release v0.2.18"
```

#### 4. Tag creation + Push

**Objectif** : Créer le tag Git et le push sur remote.

**Commande** :
```bash
VERSION="0.2.18"

git tag -a "v${VERSION}" -m "Release v${VERSION}

- Feature 1
- Feature 2
- Bug fix 3
"

git push origin "v${VERSION}"
```

**Vérification** :
```bash
# Tag created locally
git tag -l "v${VERSION}"

# Tag pushed to remote
git ls-remote --tags origin | grep "v${VERSION}"
```

#### 5. GitHub Release creation

**Objectif** : Créer la release GitHub avec notes et assets.

**Méthode automatisée (gh CLI)** :
```bash
VERSION="0.2.18"

# Release notes template
NOTES=$(cat <<'EOF'
## 🎉 What's New

### Features
- Feature 1 description
- Feature 2 description

### Bug Fixes
- Fix 1 description
- Fix 2 description

### Performance
- Optimization 1

### Breaking Changes
⚠️ List breaking changes here

## 📦 Installation

### OTA Update
The firmware will auto-detect the new version. Go to Settings → OTA → Check for Updates.

### Manual Flash
Download the appropriate firmware below and flash via USB:
```bash
pio run -e soundpanel7_usb -t upload
```

## 🔗 Assets

See attached firmware binaries for manual flash.

## 📝 Full Changelog
https://github.com/jjtronics/SoundPanel7/compare/v0.2.17...v${VERSION}
EOF
)

# Create release
gh release create "v${VERSION}" \
  --title "v${VERSION}" \
  --notes "$NOTES" \
  --draft

# Release créée en DRAFT pour review
# Publier avec: gh release edit v${VERSION} --draft=false
```

**Méthode manuelle (interface web)** :
1. Aller sur https://github.com/jjtronics/SoundPanel7/releases/new
2. Sélectionner le tag `v0.2.18`
3. Titre : `v0.2.18`
4. Description : Copier template ci-dessus
5. **Draft** : Cocher (pour review)
6. Publier après validation

**Upload assets** :
```bash
# Si assets manuels requis (firmware.bin)
gh release upload "v${VERSION}" \
  .pio/build/soundpanel7_ota/firmware.bin \
  .pio/build/soundpanel7_headless_ota/firmware.bin
```

#### 6. Monitor GitHub Actions

**Objectif** : Surveiller l'exécution des workflows CI/CD déclenchés par le tag.

**Vérifier workflows actifs** :
```bash
# Lister les workflows
gh workflow list

# Vérifier run en cours
gh run list --limit 5
```

**Monitoring automatisé** :
```bash
#!/bin/bash
set -e

VERSION="$1"
TIMEOUT=600  # 10 minutes
INTERVAL=30  # Check every 30s

echo "Monitoring GitHub Actions for v${VERSION}..."

start_time=$(date +%s)

while true; do
  # Get latest run for the tag
  run_status=$(gh run list --workflow=release.yml --limit=1 --json status,conclusion -q '.[0]')

  status=$(echo "$run_status" | jq -r '.status')
  conclusion=$(echo "$run_status" | jq -r '.conclusion')

  echo "Status: $status | Conclusion: $conclusion"

  if [ "$status" = "completed" ]; then
    if [ "$conclusion" = "success" ]; then
      echo "✅ GitHub Actions PASSED!"
      gh run view --log
      exit 0
    else
      echo "❌ GitHub Actions FAILED!"
      gh run view --log
      exit 1
    fi
  fi

  # Check timeout
  current_time=$(date +%s)
  elapsed=$((current_time - start_time))

  if [ $elapsed -gt $TIMEOUT ]; then
    echo "⏱️ Timeout after ${TIMEOUT}s"
    gh run view --log
    exit 2
  fi

  sleep $INTERVAL
done
```

**Commande** :
```bash
./scripts/monitor_ci.sh 0.2.18
```

**Monitoring manuel** :
```bash
# Afficher status en temps réel
watch -n 10 'gh run list --limit 3'

# Voir logs si échec
gh run view --log
```

#### 7. Notification

**Objectif** : Notifier l'utilisateur du résultat de la release.

**Si succès ✅** :
```
🎉 Release v0.2.18 SUCCESSFUL!

✅ Version bumped
✅ Committed and pushed
✅ Tag created: v0.2.18
✅ GitHub Release created (DRAFT)
✅ GitHub Actions: PASSED

Next steps:
1. Review release notes: https://github.com/jjtronics/SoundPanel7/releases/tag/v0.2.18
2. Publish release (remove DRAFT)
3. Test OTA update from device
4. Announce on communication channels
```

**Si échec ❌** :
```
❌ Release v0.2.18 FAILED

✅ Version bumped
✅ Committed and pushed
✅ Tag created: v0.2.18
❌ GitHub Actions: FAILED

Error logs:
[Afficher logs pertinents]

Rollback commands:
git tag -d v0.2.18
git push origin :refs/tags/v0.2.18
gh release delete v0.2.18 --yes

Investigate:
gh run view --log
```

## Script complet automatisé

**`scripts/release.sh`** :
```bash
#!/bin/bash
set -e

VERSION="$1"

if [ -z "$VERSION" ]; then
  echo "Usage: $0 <version>"
  echo "Example: $0 0.2.18"
  exit 1
fi

echo "🚀 Starting release workflow for v${VERSION}..."

# Pre-checks
echo "1️⃣ Pre-release checks..."
git diff --quiet || { echo "❌ Worktree not clean"; exit 1; }
[ "$(git rev-parse --abbrev-ref HEAD)" = "main" ] || { echo "❌ Not on main branch"; exit 1; }
git fetch origin
git status | grep "up to date" > /dev/null || { echo "❌ Not synced with remote"; exit 1; }
echo "✅ Pre-checks passed"

# Version bump
echo "2️⃣ Bumping version to ${VERSION}..."
sed -i '' "s/#define SOUNDPANEL7_VERSION \".*\"/#define SOUNDPANEL7_VERSION \"${VERSION}\"/" include/AppConfig.h
sed -i '' "s/\"version\": \".*\"/\"version\": \"${VERSION}\"/" custom_components/soundpanel7/manifest.json
echo "✅ Version bumped"

# Commit + Push
echo "3️⃣ Committing and pushing..."
git add include/AppConfig.h custom_components/soundpanel7/manifest.json
git commit -m "Release v${VERSION}"
git push origin main
echo "✅ Pushed to main"

# Tag
echo "4️⃣ Creating and pushing tag..."
git tag -a "v${VERSION}" -m "Release v${VERSION}"
git push origin "v${VERSION}"
echo "✅ Tag v${VERSION} created"

# GitHub Release
echo "5️⃣ Creating GitHub Release (DRAFT)..."
gh release create "v${VERSION}" \
  --title "v${VERSION}" \
  --notes "Release v${VERSION}

See CHANGELOG.md for details.
" \
  --draft
echo "✅ GitHub Release created (DRAFT)"

# Monitor CI
echo "6️⃣ Monitoring GitHub Actions..."
TIMEOUT=600
start_time=$(date +%s)

while true; do
  run_status=$(gh run list --limit=1 --json status,conclusion -q '.[0]' 2>/dev/null || echo '{"status":"queued","conclusion":null}')
  status=$(echo "$run_status" | jq -r '.status')
  conclusion=$(echo "$run_status" | jq -r '.conclusion')

  echo "   Status: $status"

  if [ "$status" = "completed" ]; then
    if [ "$conclusion" = "success" ]; then
      echo "✅ GitHub Actions PASSED!"
      echo ""
      echo "🎉 Release v${VERSION} SUCCESSFUL!"
      echo ""
      echo "Next steps:"
      echo "1. Review release: https://github.com/jjtronics/SoundPanel7/releases/tag/v${VERSION}"
      echo "2. Publish release: gh release edit v${VERSION} --draft=false"
      exit 0
    else
      echo "❌ GitHub Actions FAILED!"
      gh run view --log
      exit 1
    fi
  fi

  current_time=$(date +%s)
  elapsed=$((current_time - start_time))
  if [ $elapsed -gt $TIMEOUT ]; then
    echo "⏱️ Timeout after ${TIMEOUT}s"
    exit 2
  fi

  sleep 30
done
```

**Usage** :
```bash
chmod +x scripts/release.sh
./scripts/release.sh 0.2.18
```

## Rollback en cas d'échec

### Supprimer tag local + remote

```bash
VERSION="0.2.18"

# Supprimer tag local
git tag -d "v${VERSION}"

# Supprimer tag remote
git push origin ":refs/tags/v${VERSION}"
```

### Supprimer GitHub Release

```bash
VERSION="0.2.18"

# Supprimer release
gh release delete "v${VERSION}" --yes
```

### Reverter commit version bump

```bash
# Si pas encore pushé
git reset --soft HEAD~1

# Si déjà pushé (ATTENTION: force push)
git reset --hard HEAD~1
git push origin main --force
```

## Troubleshooting

### GitHub Actions failed

**Diagnostic** :
```bash
# Voir logs complets
gh run view --log

# Voir workflow file
cat .github/workflows/release.yml

# Re-run si échec temporaire
gh run rerun <run-id>
```

**Causes fréquentes** :
- Build failure : vérifier compilation locale
- Test failure : run tests locally
- Upload assets failed : vérifier credentials GitHub

### Tag already exists

```bash
# Erreur: tag 'v0.2.18' already exists
# Solution: supprimer le tag d'abord
git tag -d v0.2.18
git push origin :refs/tags/v0.2.18
```

### gh CLI not authenticated

```bash
# Login GitHub CLI
gh auth login

# Vérifier status
gh auth status
```

## Checklist complète

### Pre-release
- [ ] CHANGELOG.md mis à jour
- [ ] README.md à jour si features visibles
- [ ] Tests passent (local + CI)
- [ ] Build réussi (tous les envs)
- [ ] Worktree clean
- [ ] Branch main sync avec remote

### Release execution
- [ ] Version bump (AppConfig.h + manifest.json)
- [ ] Commit + push
- [ ] Tag creation + push
- [ ] GitHub Release (DRAFT)
- [ ] Monitor GitHub Actions (PASS)

### Post-release
- [ ] Publier release (remove DRAFT)
- [ ] Tester OTA update sur device
- [ ] Vérifier release-manifest.json généré
- [ ] Annoncer sur canaux communication
- [ ] Update RELEASING.md si process changé

## Integration avec AGENTS.md

**Invoquer cet agent** :
```
@AGENT_RELEASE.md Exécute le workflow de release pour v0.2.18
```

**Workflow typique** :
1. Développement feature → `@AGENT_DEV.md`
2. Tests validation → `@AGENT_TDD.md`
3. Update README → `@AGENT_DOCS_USER.md`
4. **Execute release** → `@AGENT_RELEASE.md` ⭐
5. Update doc release process si nécessaire → `@AGENT_DOCS_RELEASE.md`

## Ressources

- gh CLI : https://cli.github.com/
- GitHub Releases : https://docs.github.com/en/repositories/releasing-projects-on-github
- Semantic Versioning : https://semver.org/
- RELEASING.md : Documentation process détaillé

## Template de release notes

```markdown
## 🎉 What's New in v0.2.18

### ✨ Features
- **Dual-core audio isolation**: Audio acquisition now runs on dedicated Core 1 with high priority, ensuring uninterrupted measurements even during WiFi/MQTT activity
- New performance monitoring endpoints

### 🐛 Bug Fixes
- Fixed audio freeze during WiFi AP scan (analog mic limitation documented)
- Optimized WiFi connection timeout (8s → 5s) for faster boot while preventing audio freeze

### ⚡ Performance
- Optimized MQTT reconnection interval (5s → 15s)
- Reduced network blocking in main loop

### 📚 Documentation
- Added 7 specialized AI agents (DEV, CODE_REVIEW, TDD, PERF, SECURITY, DOCS)
- Comprehensive performance and security audit guides

### ⚠️ Breaking Changes
None

## 📦 Installation

### OTA Update (Recommended)
1. Go to Settings → OTA → Check for Updates
2. Click "Install Update"
3. Device will reboot automatically

### Manual Flash
```bash
pio run -e soundpanel7_usb -t upload
```

## 🔗 Full Changelog
https://github.com/jjtronics/SoundPanel7/compare/v0.2.17...v0.2.18
```
