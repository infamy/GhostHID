#!/usr/bin/env bash
# Print the GhostHID identity of whatever board is plugged in, in either state.
#
# Blank/bootloader boards have no console, so fall back to reading the MAC:
# the SSID suffix is the last two bytes of the Wi-Fi MAC, which on the S2 is
# the base MAC esptool reports.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
[ -n "${PORT:-}" ] || { echo "no board connected"; exit 1; }

# Booted firmware: ask it directly.
OUT=$("$ROOT/firmware/.venv-flash/bin/python" - "$PORT" <<'PY' 2>/dev/null
import serial, sys, time
try:
    s = serial.Serial(); s.port = sys.argv[1]; s.baudrate = 115200; s.timeout = 0.3
    s.dtr = True; s.rts = False; s.open(); time.sleep(0.35); s.reset_input_buffer()
    s.write(b"\nshow\n"); s.flush(); time.sleep(1.1)
    o = s.read(6000).decode("utf-8", "replace"); s.close()
    ver = next((l.split()[1] for l in o.splitlines() if l.strip().startswith("GhostHID 0.")), "?")
    ssid = next((l.split()[-1] for l in o.splitlines() if "ssid" in l and "ghosthid-" in l), "")
    # The station line reads "(not set - use: wifi <ssid>)" when unconfigured;
    # taking the last token off that yields "<ssid>)", which looks like a value.
    sta = "none"
    for l in o.splitlines():
        if "ssid" in l and "ghosthid-" not in l:
            sta = "none" if "not set" in l else l.split()[-1]
            break
    addr = next((l.split()[-1] for l in o.splitlines()
                 if "address" in l and "http" in l and "192.168.4.1" not in l), "")
    print(f"FLASHED  {ssid}  fw {ver}  joined={sta}" + (f"  {addr}" if addr else ""))
except Exception:
    pass
PY
)
if [ -n "$OUT" ]; then echo "$PORT  $OUT"; exit 0; fi

# Otherwise assume ROM bootloader and read the MAC.
MAC=$("$ROOT/firmware/.venv-flash/bin/esptool.py" --chip esp32s2 --port "$PORT" \
        --after no_reset read_mac 2>/dev/null | awk '/MAC:/{print $2; exit}')
if [ -n "${MAC:-}" ]; then
    SUF=$(echo "$MAC" | awk -F: '{print toupper($5$6)}')
    echo "$PORT  BLANK/BOOTLOADER  mac $MAC  -> would become ghosthid-$SUF"
else
    echo "$PORT  present but not responding (wrong mode?)"
fi
