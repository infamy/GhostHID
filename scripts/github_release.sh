#!/usr/bin/env bash
# Build, sign, and publish a GhostHID release to GitHub - run locally.
#
#   scripts/github_release.sh v0.9.1
#
# Builds the ESP32-S3 firmware, packages the bundles, signs the .zip + raw .bin
# with minisign using the operator's key, and creates (or updates) the matching
# GitHub Release with notes pulled from CHANGELOG.md.
#
# Requires: platformio (pio), minisign, gh (authenticated), python3, zip, and
# ghosthid.key in the repo root (gitignored). The tag should already exist on
# GitHub (push it to Gitea and let the push-mirror carry it over, or create it
# here - gh will make it at the default branch if missing).
#
# Overridable: PIO=/path/to/pio, GH_REPO=owner/name, SIGN_KEY=/path/to/key.
set -euo pipefail

TAG="${1:-}"
[ -n "$TAG" ] || { echo "usage: $(basename "$0") vX.Y.Z" >&2; exit 2; }
VER="${TAG#v}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PIO="${PIO:-pio}"
SIGN_KEY="${SIGN_KEY:-ghosthid.key}"
export GH_REPO="${GH_REPO:-infamy/GhostHID}"

command -v "$PIO"     >/dev/null 2>&1 || { echo "platformio (pio) not found - set PIO=/path/to/pio" >&2; exit 1; }
command -v minisign   >/dev/null 2>&1 || { echo "minisign not found - brew install minisign" >&2; exit 1; }
command -v gh         >/dev/null 2>&1 || { echo "gh CLI not found" >&2; exit 1; }
[ -f "$SIGN_KEY" ] || { echo "signing key not found: $SIGN_KEY (operator's minisign key)" >&2; exit 1; }

echo ">> building firmware (esp32-s3-lcd147)"
"$PIO" run -d firmware -e esp32-s3-lcd147

echo ">> packaging into dist/"
rm -rf dist
./scripts/package.sh firmware/.pio/build/esp32-s3-lcd147 esp32-s3-lcd147 dist
( cd dist && zip -qr ghosthid-esp32-s3-lcd147.zip ghosthid-esp32-s3-lcd147 )
cp firmware/.pio/build/esp32-s3-lcd147/ghosthid-merged.bin webflasher/ghosthid-s3.bin
zip -qr dist/ghosthid-webflasher.zip webflasher
cp firmware/.pio/build/esp32-s3-lcd147/ghosthid-merged.bin dist/ghosthid-firmware.bin

echo ">> signing with minisign"
for f in dist/*.zip dist/*.bin; do
  minisign -S -s "$SIGN_KEY" -t "GhostHID $TAG" -m "$f"   # -> $f.minisig
done

echo ">> release notes from CHANGELOG"
python3 scripts/changelog_section.py "$VER" > /tmp/ghnotes.md 2>/dev/null || echo "See CHANGELOG.md." > /tmp/ghnotes.md

FILES=( dist/ghosthid-esp32-s3-lcd147.zip
        dist/ghosthid-webflasher.zip
        dist/ghosthid-firmware.bin
        dist/ghosthid-esp32-s3-lcd147.zip.minisig
        dist/ghosthid-webflasher.zip.minisig
        dist/ghosthid-firmware.bin.minisig )

if gh release view "$TAG" >/dev/null 2>&1; then
  echo ">> updating existing release $TAG"
  gh release upload "$TAG" "${FILES[@]}" --clobber
  gh release edit   "$TAG" --title "GhostHID $TAG" --notes-file /tmp/ghnotes.md
else
  echo ">> creating release $TAG"
  gh release create "$TAG" --title "GhostHID $TAG" --notes-file /tmp/ghnotes.md "${FILES[@]}"
fi

echo ">> done: $(gh release view "$TAG" --json url -q .url)"
