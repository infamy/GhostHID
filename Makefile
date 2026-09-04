# GhostHID build orchestration
#
# The firmware is compiled inside a container so the toolchain never touches
# your machine. Flashing runs on the host, because Docker Desktop on macOS
# cannot pass a USB serial port into a Linux container.

SHELL       := /bin/bash
ENV         ?= esp32-s2-key
IMAGE       ?= ghosthid/build
PIO_VOLUME  ?= ghosthid-pio-cache
FIRMWARE    := $(CURDIR)/firmware
BUILD_DIR   := $(FIRMWARE)/.pio/build/$(ENV)
MERGED      := $(BUILD_DIR)/ghosthid-merged.bin
APP_BIN     := $(BUILD_DIR)/firmware.bin     # app image only - what OTA takes
TOKEN       ?= ghosthid

# Host-side flashing tools, kept in a project-local venv so nothing is
# installed globally.
FLASH_VENV  := $(FIRMWARE)/.venv-flash
ESPTOOL     := $(FLASH_VENV)/bin/esptool.py

# Optional build-time configuration. Example:
#   make build WIFI_SSID=HomeNet WIFI_PASS=hunter2 AP_PASS=s3cret TOKEN=abc123
EXTRA_FLAGS :=
ifneq ($(WIFI_SSID),)
EXTRA_FLAGS += -DGHOSTHID_STA_SSID=\"$(WIFI_SSID)\"
endif
ifneq ($(WIFI_PASS),)
EXTRA_FLAGS += -DGHOSTHID_STA_PASSWORD=\"$(WIFI_PASS)\"
endif
ifneq ($(AP_PASS),)
EXTRA_FLAGS += -DGHOSTHID_AP_PASSWORD=\"$(AP_PASS)\"
endif
ifneq ($(TOKEN),)
EXTRA_FLAGS += -DGHOSTHID_AUTH_TOKEN=\"$(TOKEN)\"
endif

PIO_ENVVARS = -e PLATFORMIO_CORE_DIR=/pio
LOCAL_INI  := $(FIRMWARE)/ghosthid_local.ini

DOCKER_RUN = docker run --rm -t \
	-v "$(FIRMWARE)":/project \
	-v $(PIO_VOLUME):/pio \
	$(PIO_ENVVARS) \
	$(IMAGE)

.PHONY: help image build rebuild clean distclean shell flash monitor ports size localini ota

help:
	@echo "GhostHID"
	@echo "  make build              compile firmware in a clean container"
	@echo "  make flash PORT=...     flash the merged image over USB"
	@echo "  make ota IP=... TOKEN=.. update over the network (no cable)"
	@echo "  make monitor PORT=...   open the USB CDC serial console"
	@echo "  make ports              list candidate serial ports"
	@echo "  make shell              interactive shell in the build container"
	@echo "  make clean              remove build output"
	@echo "  make distclean          also drop the toolchain cache volume"
	@echo
	@echo "  ENV=$(ENV)  (set ENV=esp32-s3 to cross-check portability)"
	@echo
	@echo "Build-time config (all optional):"
	@echo "  WIFI_SSID=... WIFI_PASS=...   also join your LAN (AP stays up)"
	@echo "  AP_PASS=...                   WPA2 pass for the device's own AP"
	@echo "  TOKEN=...                     application-layer pairing token"

image:
	@docker build -q -t $(IMAGE) $(FIRMWARE) >/dev/null && echo "image $(IMAGE) ready"

localini:
	@printf '[local]\nbuild_flags =%s\n' '$(EXTRA_FLAGS)' > "$(LOCAL_INI)"

build: image localini
	$(DOCKER_RUN) pio run -e $(ENV)
	@echo
	@echo "Merged image: $(MERGED)"
	@ls -lh "$(MERGED)" 2>/dev/null || echo "  (merge step did not run - see output above)"

rebuild: image localini
	$(DOCKER_RUN) pio run -e $(ENV) -t clean
	$(DOCKER_RUN) pio run -e $(ENV)

size: image localini
	$(DOCKER_RUN) pio run -e $(ENV) -t size

shell: image
	docker run --rm -it -v "$(FIRMWARE)":/project -v $(PIO_VOLUME):/pio \
		-e PLATFORMIO_CORE_DIR=/pio $(IMAGE) bash

clean:
	rm -rf "$(FIRMWARE)/.pio"

distclean: clean
	-docker volume rm $(PIO_VOLUME)
	-docker rmi $(IMAGE)

# --- host-side flashing ----------------------------------------------------

$(ESPTOOL):
	python3 -m venv "$(FLASH_VENV)"
	"$(FLASH_VENV)/bin/pip" install -q --upgrade pip esptool

ports:
	@echo "Serial ports:"; ls /dev/cu.* 2>/dev/null || true
	@echo; echo "Espressif USB devices:"
	@system_profiler SPUSBDataType 2>/dev/null \
		| grep -B6 -i "0x303a" || echo "  none found - hold BOOT and replug to enter the ROM bootloader"

flash: $(ESPTOOL)
	@test -n "$(PORT)" || { echo "PORT is required, e.g. make flash PORT=/dev/cu.usbmodem01"; exit 1; }
	@test -f "$(MERGED)" || { echo "no merged image - run 'make build' first"; exit 1; }
	@set -o pipefail; \
	"$(ESPTOOL)" --chip esp32s2 --port "$(PORT)" --baud 921600 \
		write_flash --flash_mode keep --flash_freq keep --flash_size keep 0x0 "$(MERGED)" \
		2>&1 | tee /tmp/ghosthid-flash.log; \
	rc=$$?; \
	if grep -q "Hash of data verified" /tmp/ghosthid-flash.log; then \
		echo; echo "Flash verified OK."; \
		grep -q "serial exception" /tmp/ghosthid-flash.log \
			&& echo "(The reset error above is benign: the bootloader's CDC port"; \
		grep -q "serial exception" /tmp/ghosthid-flash.log \
			&& echo " disappears the moment the chip reboots into the new firmware.)"; \
		echo "Replug if it does not re-enumerate within a few seconds."; \
		exit 0; \
	else \
		echo "FLASH FAILED - image was not verified."; exit $${rc:-1}; \
	fi

# Wireless update. Sends the app image only - the merged image is for USB.
ota:
	@test -n "$(IP)" || { echo "IP is required, e.g. make ota IP=192.168.7.113 TOKEN=ghosthid"; exit 1; }
	@test -f "$(APP_BIN)" || { echo "no firmware.bin - run 'make build' first"; exit 1; }
	@echo "Uploading $$(du -h "$(APP_BIN)" | cut -f1) to $(IP)…"
	@curl -sS --fail-with-body -X POST \
		-H "Content-Type: application/octet-stream" \
		-H "X-GhostHID-Token: $(TOKEN)" \
		--data-binary "@$(APP_BIN)" \
		"http://$(IP)/api/ota" && echo && echo "Device is rebooting."

monitor: $(ESPTOOL)
	@test -n "$(PORT)" || { echo "PORT is required, e.g. make monitor PORT=/dev/cu.usbmodem01"; exit 1; }
	"$(FLASH_VENV)/bin/python" -m serial.tools.miniterm "$(PORT)" 115200
