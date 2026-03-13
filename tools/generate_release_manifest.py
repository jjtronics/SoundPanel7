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
    parser.add_argument("--build-dir", required=True, help="Directory containing firmware artifacts")
    parser.add_argument("--output", required=True, help="Output manifest path")
    args = parser.parse_args()

    build_dir = Path(args.build_dir)
    output = Path(args.output)
    version = args.tag[1:] if args.tag.startswith("v") else args.tag

    firmware = build_asset(
        args.repository,
        args.tag,
        "firmware.bin",
        "firmware",
        build_dir / "firmware.bin",
    )
    bootloader = build_asset(
        args.repository,
        args.tag,
        "bootloader.bin",
        "bootloader",
        build_dir / "bootloader.bin",
    )
    partitions = build_asset(
        args.repository,
        args.tag,
        "partitions.bin",
        "partitions",
        build_dir / "partitions.bin",
    )

    manifest = {
        "project": "SoundPanel7",
        "tag": args.tag,
        "version": version,
        "published_at": args.published_at,
        "release_url": f"https://github.com/{args.repository}/releases/tag/{args.tag}",
        "ota": firmware,
        "assets": [firmware, bootloader, partitions],
    }

    output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
