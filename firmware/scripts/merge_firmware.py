"""PlatformIO post-build step: emit one flashable image at offset 0x0.

The firmware is compiled inside a Linux container, but Docker Desktop on macOS
cannot pass a USB serial port through to a container, so flashing has to happen
on the host. Rather than make the host juggle four binaries and their offsets,
we merge bootloader + partition table + boot_app0 + application into a single
`ghosthid-merged.bin` that the host writes at 0x0.
"""

import os

Import("env")  # noqa: F821  (injected by PlatformIO)

# Bootloader offset differs by chip family. Everything else is fixed by the
# default arduino partition scheme.
BOOTLOADER_OFFSET = {
    "esp32":   0x1000,
    "esp32s2": 0x1000,
    "esp32s3": 0x0000,
    "esp32c3": 0x0000,
}


def merge_firmware(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    prog_name = env.subst("$PROGNAME")
    board = env.BoardConfig()
    mcu = board.get("build.mcu", "esp32s2")

    app = os.path.join(build_dir, prog_name + ".bin")
    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")

    framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    boot_app0 = os.path.join(framework_dir, "tools", "partitions", "boot_app0.bin")

    missing = [p for p in (app, bootloader, partitions, boot_app0) if not os.path.isfile(p)]
    if missing:
        print("[ghosthid] skipping merge, missing: %s" % ", ".join(missing))
        return

    out = os.path.join(build_dir, "ghosthid-merged.bin")
    flash_size = board.get("upload.flash_size", "4MB")
    flash_mode = board.get("build.flash_mode", "dio")
    flash_freq = board.get("build.f_flash", "80000000L")
    flash_freq = str(flash_freq).replace("000000L", "m").replace("L", "")

    env.Execute(
        " ".join([
            '"$PYTHONEXE"',
            '"%s"' % os.path.join(
                env.PioPlatform().get_package_dir("tool-esptoolpy"), "esptool.py"),
            "--chip", mcu,
            "merge_bin",
            "-o", '"%s"' % out,
            "--flash_mode", flash_mode,
            "--flash_freq", flash_freq,
            "--flash_size", flash_size,
            hex(BOOTLOADER_OFFSET.get(mcu, 0x1000)), '"%s"' % bootloader,
            "0x8000", '"%s"' % partitions,
            "0xe000", '"%s"' % boot_app0,
            "0x10000", '"%s"' % app,
        ])
    )
    print("[ghosthid] merged image -> %s" % out)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_firmware)  # noqa: F821
