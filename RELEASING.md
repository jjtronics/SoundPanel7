# Releasing SoundPanel 7

This repository contains two versioned deliverables:

- the ESP32 firmware
- the Home Assistant custom integration in `custom_components/soundpanel7`

For a public GitHub release, keep both versions aligned unless you intentionally release them separately.

## Current version

The version is declared in:

- `include/AppConfig.h`
- `custom_components/soundpanel7/manifest.json`

At the time of writing, both are set to `0.1.0`.

## First release recommendation

If this is the first public release of the repository and the current state is the one you want to publish, release:

- Git tag: `v0.1.0`
- GitHub release title: `SoundPanel7 v0.1.0`

## Release checklist

1. Ensure the worktree is clean.
2. Update the version in:
   - `include/AppConfig.h`
   - `custom_components/soundpanel7/manifest.json`
3. Rebuild the firmware:

```bash
pio run -e soundpanel7_usb
```

4. Optionally validate OTA build settings if you use OTA in production:

```bash
pio run -e soundpanel7_ota
```

5. Review the README and screenshots if the release changes user-facing behavior.
6. Commit the version bump if needed.

## Create the Git tag

```bash
git tag -a v0.1.0 -m "SoundPanel7 v0.1.0"
git push origin main
git push origin v0.1.0
```

If you release another version later, replace `0.1.0` everywhere consistently.

## Create the GitHub release

On GitHub:

1. Open `Releases`
2. Click `Draft a new release`
3. Select tag `v0.1.0`
4. Title it `SoundPanel7 v0.1.0`
5. Add release notes
6. Publish

Suggested sections for release notes:

- Highlights
- Firmware
- Home Assistant integration
- Notes / known limits

## Release assets

After a successful build, useful firmware files are generated in:

- `.pio/build/soundpanel7_usb/firmware.bin`
- `.pio/build/soundpanel7_usb/bootloader.bin`
- `.pio/build/soundpanel7_usb/partitions.bin`

If GitHub Actions is enabled for the repository, publishing the GitHub release will automatically build the firmware and attach these files to the release.

If you prefer to do it manually, you can still attach these files yourself.

For source-based installation, GitHub already provides the source archive automatically.

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
