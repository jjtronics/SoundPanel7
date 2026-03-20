# Agents Claude Code - SoundPanel 7

Ce projet utilise des **agents spécialisés** pour structurer le développement avec Claude Code. Chaque agent a un rôle précis et des instructions détaillées.

## 📋 Vue d'ensemble

| Agent | Rôle | Quand l'utiliser |
|-------|------|------------------|
| [AGENT_DEV](#agent_devmd) | Développement features | Nouvelle fonctionnalité |
| [AGENT_CODE_REVIEW](#agent_code_reviewmd) | Revue de code | Refactoring, cleanup |
| [AGENT_TDD](#agent_tddmd) | Tests et validation | Avant release, détection régressions |
| [AGENT_PERF](#agent_perfmd) | Optimisation performance | Monitoring, optimisation |
| [AGENT_SECURITY](#agent_securitymd) | Audit sécurité | Avant production, audit endpoints |
| [AGENT_RELEASE](#agent_releasemd) | Exécution release | Publication nouvelle version |
| [AGENT_DOCS_USER](#agent_docs_usermd) | Documentation utilisateur | Mise à jour README |
| [AGENT_DOCS_RELEASE](#agent_docs_releasemd) | Documentation process | Mise à jour RELEASING.md |

---

## Agents de développement

### AGENT_DEV.md

**Rôle** : Développer des nouvelles fonctionnalités en respectant l'architecture existante.

**Responsabilités** :
- Implémenter nouvelles features
- Respecter les patterns du projet
- Gérer la persistance NVS
- Créer endpoints API
- Intégrer MQTT

**Quand l'utiliser** :
- Ajout de nouvelle source audio
- Nouveau dashboard UI
- Nouvel endpoint API
- Intégration nouvelle plateforme

**Fichier** : [AGENT_DEV.md](AGENT_DEV.md)

---

### AGENT_CODE_REVIEW.md

**Rôle** : Analyser le code pour identifier et corriger les problèmes de qualité.

**Responsabilités** :
- Factoriser code dupliqué
- Simplifier logique complexe
- Optimiser allocations mémoire
- Standardiser patterns
- Nettoyer TODOs obsolètes

**Quand l'utiliser** :
- Après développement feature importante
- Refactoring session
- Nettoyage codebase
- Avant release majeure

**Fichier** : [AGENT_CODE_REVIEW.md](AGENT_CODE_REVIEW.md)

---

## Agents de qualité

### AGENT_TDD.md

**Rôle** : Assurer la qualité et la stabilité via tests et validation.

**Responsabilités** :
- Créer tests unitaires
- Définir procédures validation manuelle
- Détecter régressions
- Tests d'intégration

**Quand l'utiliser** :
- Avant chaque release
- Après refactoring majeur
- Test de non-régression
- Validation composant critique

**Fichier** : [AGENT_TDD.md](AGENT_TDD.md)

---

### AGENT_PERF.md

**Rôle** : Analyser et optimiser les performances (CPU, mémoire, latence).

**Responsabilités** :
- Profiling heap/CPU
- Optimiser loop time
- Détecter fuites mémoire
- Benchmark temps d'exécution

**Quand l'utiliser** :
- Monitoring continu performance
- Optimisation après feature lourde
- Investigation spike LVGL
- Avant déploiement production

**Métriques cibles** :
- Heap libre : > 50KB
- CPU idle : > 60%
- Loop LVGL : < 30ms

**Fichier** : [AGENT_PERF.md](AGENT_PERF.md)

---

### AGENT_SECURITY.md

**Rôle** : Auditer et renforcer la sécurité du firmware.

**Responsabilités** :
- Audit endpoints API
- Validation inputs
- Protection credentials
- OWASP IoT Top 10
- Hardening production

**Quand l'utiliser** :
- Avant déploiement production
- Après ajout endpoint exposé
- Audit régulier (trimestriel)
- Avant passage environnement critique

**Checklist OWASP** :
- Validation entrées ✅
- OTA sécurisé (SHA-256) ✅
- Protection credentials ✅
- Authentification API ✅

**Fichier** : [AGENT_SECURITY.md](AGENT_SECURITY.md)

---

## Agent de release

### AGENT_RELEASE.md

**Rôle** : Exécuter le workflow de release complet (version bump, push, tag, GitHub release, monitoring CI).

**Responsabilités** :
- Orchestrer le processus de release
- Version bump automatique
- Création tag + GitHub release
- Monitoring GitHub Actions
- Notification résultat

**Quand l'utiliser** :
- Publication nouvelle version
- Création release GitHub
- Automatisation workflow release
- Monitoring CI/CD

**Workflow** :
1. Pre-release checks
2. Version bump
3. Commit + push
4. Tag création
5. GitHub release (DRAFT)
6. Monitor GitHub Actions
7. Notification

**Fichier** : [AGENT_RELEASE.md](AGENT_RELEASE.md)

---

## Agents de documentation

### AGENT_DOCS_USER.md

**Rôle** : Maintenir la documentation utilisateur (README.md) à jour.

**Responsabilités** :
- Documenter nouvelles features
- Mettre à jour captures d'écran
- Maintenir tableaux de référence
- Traduction FR/EN

**Quand l'utiliser** :
- Après ajout feature visible utilisateur
- Changement UI majeur
- Nouveau hardware supporté
- Modification workflow utilisateur

**Fichier** : [AGENT_DOCS_USER.md](AGENT_DOCS_USER.md)

---

### AGENT_DOCS_RELEASE.md

**Rôle** : Maintenir la documentation du processus de release (RELEASING.md).

**Responsabilités** :
- Documenter workflow release
- Mettre à jour checklist
- Gérer manifest format
- Process GitHub release

**Quand l'utiliser** :
- Changement process build
- Nouveau profil hardware
- Modification workflow CI/CD
- Breaking change nécessitant migration

**Fichier** : [AGENT_DOCS_RELEASE.md](AGENT_DOCS_RELEASE.md)

---

## Usage avec Claude Code

### Invoquer un agent

Dans Claude Code, mentionner l'agent dans votre prompt :

```
@AGENT_PERF.md Analyse les performances du firmware et identifie les goulots
```

```
@AGENT_SECURITY.md Audite tous les endpoints de l'API Web
```

```
@AGENT_DEV.md Implémente support pour micro digital I2S array
```

### Workflow complet

**Pour une nouvelle feature** :

1. **Développement** : `@AGENT_DEV.md`
   - Implémenter la feature
   - Respecter architecture
   - Ajouter persistance si nécessaire

2. **Revue** : `@AGENT_CODE_REVIEW.md`
   - Factoriser code dupliqué
   - Vérifier nommage
   - Optimiser allocations

3. **Tests** : `@AGENT_TDD.md`
   - Valider feature
   - Tests de non-régression
   - Validation manuelle

4. **Performance** : `@AGENT_PERF.md` *(si feature lourde)*
   - Profiler impact heap/CPU
   - Optimiser si nécessaire

5. **Sécurité** : `@AGENT_SECURITY.md` *(si endpoints exposés)*
   - Auditer validation inputs
   - Vérifier authentification

6. **Documentation** : `@AGENT_DOCS_USER.md`
   - Mettre à jour README
   - Captures d'écran si applicable

7. **Release** : `@AGENT_DOCS_RELEASE.md`
   - Préparer notes de version
   - Suivre checklist release

---

## Contribution

### Ajouter un nouvel agent

Si vous identifiez un besoin non couvert :

1. Créer `AGENT_NOUVEAU.md`
2. Suivre la structure des agents existants :
   - Rôle et responsabilités
   - Quand l'utiliser
   - Checklist
   - Exemples concrets
   - Anti-patterns
3. Ajouter dans cet index

### Maintenir les agents

Les agents doivent **évoluer avec le projet** :

- ✅ Mettre à jour si architecture change
- ✅ Ajouter patterns découverts
- ✅ Corriger exemples obsolètes
- ✅ Enrichir checklist

---

## Principes de conception des agents

### Un agent = Une responsabilité claire

Chaque agent a un **périmètre bien défini** sans overlap.

### Exemples concrets

Les agents contiennent des **exemples de code réels** du projet, pas des concepts abstraits.

### Actionnable

Chaque agent fournit des **checklists et actions concrètes**, pas juste de la théorie.

### Évolutif

Les agents sont des **documents vivants** qui s'enrichissent avec l'expérience du projet.

---

## Ressources complémentaires

- **AI.md** : Instructions codebase pour AI assistants (Claude, Codex, Cursor, etc.)
- **README.md** : Documentation utilisateur
- **RELEASING.md** : Process de release détaillé

---

## Support

Pour toute question sur l'utilisation des agents :
- Consulter le fichier agent concerné
- Voir les exemples dans le code source
- Issues GitHub : https://github.com/jjtronics/SoundPanel7/issues
