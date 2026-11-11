#!/usr/bin/env python3
"""Reconstitute the signed Arch repository payload for GitHub Pages."""
import hashlib
import json
import os
import re
import subprocess
import sys
import tarfile
from pathlib import Path


def assemble(repository: Path, github_repository: str) -> None:
    manifest = json.loads((repository / "release-asset.json").read_text())
    name = manifest["package_name"]
    asset_id = manifest["asset_id"]
    if not re.fullmatch(r"vibeshine-[A-Za-z0-9.+_]+-1-x86_64\.pkg\.tar\.zst", name):
        raise ValueError("Invalid package name in release asset manifest")
    if not isinstance(asset_id, str) or not re.fullmatch(r"[1-9][0-9]*", asset_id):
        raise ValueError("Invalid release asset ID")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", github_repository):
        raise ValueError("Invalid GitHub repository")
    keyring = repository / "nonary-vibeshine.gpg"
    database = repository / "vibeshine.db"
    subprocess.run(["gpgv", "--keyring", str(keyring.resolve()), str(database) + ".sig", str(database)], check=True)
    with tarfile.open(database, "r:gz") as archive:
        descriptions = [member for member in archive.getmembers() if member.isfile() and member.name.endswith("/desc")]
        if len(descriptions) != 1:
            raise ValueError("Expected exactly one package in signed database")
        description = archive.extractfile(descriptions[0]).read().decode("utf-8")
    fields = dict(re.findall(r"(?m)^%([A-Z0-9]+)%\n([^\n]+)", description))
    if fields.get("NAME") != "vibeshine" or fields.get("FILENAME") != name:
        raise ValueError("Release asset does not match signed database")
    expected = fields.get("SHA256SUM", "")
    if not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise ValueError("Invalid package hash in signed database")
    destination = repository / name
    try:
        with destination.open("wb") as output:
            subprocess.run(["gh", "api", "-H", "Accept: application/octet-stream",
                            f"repos/{github_repository}/releases/assets/{asset_id}"], stdout=output, check=True)
        with destination.open("rb") as package:
            digest = hashlib.file_digest(package, "sha256").hexdigest()
        if digest != expected:
            raise ValueError("Downloaded package hash does not match signed database")
        subprocess.run(["gpgv", "--keyring", str(keyring.resolve()), str(destination) + ".sig", str(destination)], check=True)
    except Exception:
        destination.unlink(missing_ok=True)
        raise


if __name__ == "__main__":
    assemble(Path(sys.argv[1]), os.environ["GITHUB_REPOSITORY"])
