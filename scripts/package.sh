#!/usr/bin/env bash
# Assemble a downloadable release bundle from a PlatformIO build.
#
# Used by both `make dist` and CI, so what you can build locally is exactly what
# CI publishes.
#
#   scripts/package.sh <build-dir> <env-name> <out-dir>

set -euo pipefail

# Resolve everything from the repository root, derived from this script's own
# location, so the caller's working directory is irrelevant. Gitea's runner did
# not put us where the workflow implied, and a path that is correct from the
# root and wrong from firmware/ is not a bug worth having twice.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="${1:?build dir required}"
ENV_NAME="${2:?env name required}"
OUT="${3:?output dir required}"

# Accept a relative path (resolved against the repo root) or an absolute one.
case "$BUILD_DIR" in
    /*) ;;
    *) BUILD_DIR="$REPO_ROOT/$BUILD_DIR" ;;
esac
case "$OUT" in
    /*) ;;
    *) OUT="$REPO_ROOT/$OUT" ;;
esac

APP="$BUILD_DIR/firmware.bin"
MERGED="$BUILD_DIR/ghosthid-merged.bin"

for f in "$APP" "$MERGED"; do
    if [ ! -f "$f" ]; then
        {
            echo "missing $f"
            echo "  cwd:       $(pwd)"
            echo "  build dir: $BUILD_DIR"
            echo "  contents:"
            ls -la "$BUILD_DIR" 2>&1 | sed 's/^/    /' || echo "    (does not exist)"
        } >&2
        exit 1
    fi
done

DEST="$OUT/ghosthid-$ENV_NAME"
rm -rf "$DEST"; mkdir -p "$DEST"
cp "$APP" "$MERGED" "$DEST/"

# grep -m1 rather than `grep | head -1`: under `set -o pipefail`, head closing
# the pipe early gives grep a SIGPIPE and fails the whole substitution. It only
# works locally because grep usually finishes first - a race, not a guarantee.
VERSION=$(grep -m1 -oE '"[0-9]+\.[0-9]+\.[0-9]+[^"]*"' firmware/include/board_config.h | tr -d '"')
[ -n "$VERSION" ] || VERSION="unknown"
COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
CHIP=esp32s2
[ "$ENV_NAME" = "esp32-s3" ] && CHIP=esp32s3 || true

sum() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1"; else shasum -a 256 "$1"; fi \
        | awk '{print $1}'
}

cat > "$DEST/MANIFEST.txt" <<EOF
GhostHID $VERSION
target   $ENV_NAME ($CHIP)
commit   $COMMIT
built    $(date -u +"%Y-%m-%dT%H:%M:%SZ")

firmware.bin          $(wc -c < "$APP" | tr -d ' ') bytes  sha256 $(sum "$APP")
ghosthid-merged.bin   $(wc -c < "$MERGED" | tr -d ' ') bytes  sha256 $(sum "$MERGED")
EOF

cat > "$DEST/FLASHING.md" <<EOF
# Flashing GhostHID $VERSION ($ENV_NAME)

Two images, and picking the wrong one is the usual mistake:

| File | Use | Settings |
|---|---|---|
| \`ghosthid-merged.bin\` | **a new or unconfigured board**, over USB | **erased** |
| \`firmware.bin\` | **updating a working device**, over Wi-Fi or USB | **kept** |

\`ghosthid-merged.bin\` contains the bootloader and partition table and is
written at offset 0. It spans the settings partition, so it wipes stored Wi-Fi
credentials, token and device name — which is what you want on a fresh board and
not what you want on a working one.

---

## A fresh board (USB)

You need \`esptool\`:

\`\`\`bash
python3 -m venv ~/.ghosthid-flash
~/.ghosthid-flash/bin/pip install esptool
\`\`\`

**Put the board into its ROM bootloader.** An ESP32-S2 whose only USB is the
native port will not enumerate at all until it has firmware, so this step is not
optional on a blank board:

> Hold **BOOT** (GPIO0), plug the cable in (or tap **RESET**), then release BOOT.

Find it — it appears as an Espressif device, vendor \`0x303a\`:

\`\`\`bash
ls /dev/cu.usbmodem*        # macOS
ls /dev/ttyACM* /dev/ttyUSB*  # Linux
\`\`\`

Write it:

\`\`\`bash
~/.ghosthid-flash/bin/esptool.py --chip $CHIP --port <PORT> --baud 921600 \\
  write_flash --flash_mode keep --flash_freq keep --flash_size keep \\
  0x0 ghosthid-merged.bin
\`\`\`

Unplug and replug. It now enumerates as **GhostHID Keyboard/Mouse** and brings up
its own Wi-Fi access point.

### First-time setup

Either open the serial console on the same cable —

\`\`\`bash
~/.ghosthid-flash/bin/python -m serial.tools.miniterm --dtr 1 --rts 0 <PORT> 115200
\`\`\`

\`\`\`
> help
> wifi YourNetwork
> wifipass YourPassword
> token something-private
> show
> reboot
\`\`\`

— or join the access point it is already broadcasting:

\`\`\`
SSID:     ghosthid-XXXX          (XXXX derived from the board's MAC)
Password: ghosthid-setup
Open:     http://192.168.4.1/    then use the Settings tab
\`\`\`

**Change the pairing token before putting it on a shared network.** The default
is \`ghosthid\`, and the token is the only thing standing between anyone on that
network and both typing on the target and installing firmware.

---

## Updating a working device (over the air)

\`\`\`bash
curl -sS -H "Content-Type: application/octet-stream" \\
     -H "X-GhostHID-Token: <your-token>" \\
     --data-binary @firmware.bin \\
     "http://<device>/api/ota"
\`\`\`

The \`Content-Type\` header is required. Without it curl sends
\`x-www-form-urlencoded\`, the device tries to parse the whole image as form
fields, runs out of memory and resets — which looks like a network fault.

Or drag \`firmware.bin\` onto the device's **Settings** tab in a browser.

Writes go to the spare flash slot and only take effect once the image validates,
so an interrupted upload leaves the running firmware untouched. Upload
\`firmware.bin\`, never \`ghosthid-merged.bin\` — both the browser and the device
check and will reject the wrong one.

## Verifying a download

\`\`\`bash
shasum -a 256 firmware.bin ghosthid-merged.bin   # compare against MANIFEST.txt
\`\`\`
EOF

echo "packaged -> $DEST"
ls -1 "$DEST"
