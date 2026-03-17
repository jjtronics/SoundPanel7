# Releasing SoundPanel 7

This repository contains two versioned deliverables:

- the ESP32 firmware
- the Home Assistant custom integration in `custom_components/soundpanel7`

For a public GitHub release, keep both versions aligned unless you intentionally release them separately.

## Current version

The version is declared in:

- `include/AppConfig.h`
- `custom_components/soundpanel7/manifest.json`

At the time of writing, both are set to `0.2.0`.

## First release recommendation

If the current state is the one you want to publish, release:

- Git tag: `v0.2.0`
- GitHub release title: `SoundPanel7 v0.2.0`

## Release checklist

1. Ensure the worktree is clean.
2. Update the version in:
   - `include/AppConfig.h`
   - `custom_components/soundpanel7/manifest.json`
3. Rebuild the release firmware:

```bash
pio run -e soundpanel7_ota
pio run -e soundpanel7_headless_ota
```

4. Optionally validate the USB build too:

```bash
pio run -e soundpanel7_usb
```

5. Review the README and screenshots if the release changes user-facing behavior.
6. Commit the version bump if needed.

## Create the Git tag

```bash
git tag -a v0.2.0 -m "SoundPanel7 v0.2.0"
git push origin main
git push origin v0.2.0
```

If you release another version later, replace `0.2.0` everywhere consistently.

## Create the GitHub release

On GitHub:

1. Open `Releases`
2. Click `Draft a new release`
3. Select tag `v0.2.0`
4. Title it `SoundPanel7 v0.2.0`
5. Add release notes
6. Publish

Suggested sections for release notes:

- Highlights
- Firmware
- Home Assistant integration
- Notes / known limits

## Release assets

After a successful release build, useful firmware files are generated in:

- `.pio/build/soundpanel7_ota/firmware.bin`
- `.pio/build/soundpanel7_ota/bootloader.bin`
- `.pio/build/soundpanel7_ota/partitions.bin`
- `.pio/build/soundpanel7_headless_ota/firmware.bin`
- `.pio/build/soundpanel7_headless_ota/bootloader.bin`
- `.pio/build/soundpanel7_headless_ota/partitions.bin`
- `.pio/build/soundpanel7_ota/release-manifest.json`

If GitHub Actions is enabled for the repository, publishing the GitHub release will automatically build the firmware and attach these files to the release.

If you prefer to do it manually, you can still attach these files yourself.

For source-based installation, GitHub already provides the source archive automatically.

## Release manifest

The release workflow also publishes `release-manifest.json`.

Its purpose is to give the device a stable machine-readable source for:

- latest version
- OTA firmware URLs per hardware profile
- SHA-256 checksum
- release metadata

Current structure:

```json
{
  "project": "SoundPanel7",
  "tag": "v0.2.0",
  "version": "0.2.0",
  "published_at": "2026-03-13T12:00:00Z",
  "release_url": "https://github.com/jjtronics/SoundPanel7/releases/tag/v0.2.0",
  "ota": {
    "name": "soundpanel7_ota-firmware.bin",
    "type": "firmware",
    "url": "https://github.com/jjtronics/SoundPanel7/releases/download/v0.2.0/soundpanel7_ota-firmware.bin",
    "sha256": "..."
  },
  "ota_screen": {
    "name": "soundpanel7_ota-firmware.bin",
    "type": "firmware",
    "url": "https://github.com/jjtronics/SoundPanel7/releases/download/v0.2.0/soundpanel7_ota-firmware.bin",
    "sha256": "..."
  },
  "ota_headless": {
    "name": "soundpanel7_headless_ota-firmware.bin",
    "type": "firmware",
    "url": "https://github.com/jjtronics/SoundPanel7/releases/download/v0.2.0/soundpanel7_headless_ota-firmware.bin",
    "sha256": "..."
  },
  "assets": [
    {
      "name": "soundpanel7_ota-firmware.bin",
      "type": "firmware",
      "url": "https://github.com/jjtronics/SoundPanel7/releases/download/v0.2.0/soundpanel7_ota-firmware.bin",
      "sha256": "..."
    }
  ]
}
```

For the future update-check task, the recommended flow is:

1. fetch `release-manifest.json`
2. compare `version` with the current firmware version
3. choose the correct OTA entry for the current hardware profile
4. verify the matching SHA-256 before applying the update

## Suggested first release notes

Example:

```text
First public release of SoundPanel 7.

Includes:
- ESP32-S3 firmware for the Waveshare 7" touch display
- live sound monitoring with dB / Leq / Peak
- NTP clock UI
- embedded web administration
- MQTT support and MQTT Discovery
- OTA updates
- Home Assistant custom integration with Zeroconf discovery

Known points:
- hardware setup and analog microphone calibration still require project-specific adjustment
- some PlatformIO settings are currently tuned for the main development machine
```

## HACS note

The repository now includes `hacs.json`, so the custom integration can be added in HACS as a custom repository after the first GitHub release is published.

Remaining repository-level items may still need to be adjusted directly on GitHub, for example:

- repository topics
- issue tracker enabled
- optional branding assets if you want stricter HACS ecosystem polish
