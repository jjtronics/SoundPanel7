#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_asset(repository: str, tag: str, name: str, asset_type: str, path: Path) -> dict[str, str]:
    return {
        "name": name,
        "type": asset_type,
        "url": f"https://github.com/{repository}/releases/download/{tag}/{name}",
        "sha256": sha256_file(path),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate SoundPanel7 release manifest")
    parser.add_argument("--tag", required=True, help="Git tag, for example v0.1.5")
    parser.add_argument("--repository", required=True, help="GitHub repository, for example jjtronics/SoundPanel7")
    parser.add_argument("--published-at", required=True, help="Release publication timestamp")
    parser.add_argument("--build-dir", required=True, help="Directory containing screen firmware artifacts")
    parser.add_argument("--headless-build-dir", required=True, help="Directory containing headless firmware artifacts")
    parser.add_argument("--output", required=True, help="Output manifest path")
    args = parser.parse_args()

    build_dir = Path(args.build_dir)
    headless_build_dir = Path(args.headless_build_dir)
    output = Path(args.output)
    version = args.tag[1:] if args.tag.startswith("v") else args.tag

    firmware = build_asset(
        args.repository,
        args.tag,
        "soundpanel7_ota-firmware.bin",
        "firmware",
        build_dir / "firmware.bin",
    )
    firmware_headless = build_asset(
        args.repository,
        args.tag,
        "soundpanel7_headless_ota-firmware.bin",
        "firmware",
        headless_build_dir / "firmware.bin",
    )
    bootloader = build_asset(
        args.repository,
        args.tag,
        "soundpanel7_ota-bootloader.bin",
        "bootloader",
        build_dir / "bootloader.bin",
    )
    bootloader_headless = build_asset(
        args.repository,
        args.tag,
        "soundpanel7_headless_ota-bootloader.bin",
        "bootloader",
        headless_build_dir / "bootloader.bin",
    )
    partitions = build_asset(
        args.repository,
        args.tag,
        "soundpanel7_ota-partitions.bin",
        "partitions",
        build_dir / "partitions.bin",
    )
    partitions_headless = build_asset(
        args.repository,
        args.tag,
        "soundpanel7_headless_ota-partitions.bin",
        "partitions",
        headless_build_dir / "partitions.bin",
    )

    manifest = {
        "project": "SoundPanel7",
        "tag": args.tag,
        "version": version,
        "published_at": args.published_at,
        "release_url": f"https://github.com/{args.repository}/releases/tag/{args.tag}",
        "ota": firmware,
        "ota_screen": firmware,
        "ota_headless": firmware_headless,
        "otaScreenUrl": firmware["url"],
        "otaScreenSha256": firmware["sha256"],
        "otaHeadlessUrl": firmware_headless["url"],
        "otaHeadlessSha256": firmware_headless["sha256"],
        "assets": [
            firmware,
            bootloader,
            partitions,
            firmware_headless,
            bootloader_headless,
            partitions_headless,
        ],
    }

    output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
