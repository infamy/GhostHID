# Reproducible GhostHID firmware build environment.
#
# Only the toolchain lives here. Sources are bind-mounted at run time and the
# PlatformIO package cache lives in a named volume, so an edit-rebuild cycle
# does not re-download the ~500MB Xtensa toolchain.

FROM python:3.12-slim-bookworm

# git: PlatformIO fetches some packages from git
# ca-certificates: TLS for the package registry
RUN apt-get update \
 && apt-get install -y --no-install-recommends git ca-certificates \
 && rm -rf /var/lib/apt/lists/*

ARG PLATFORMIO_VERSION=6.1.16
RUN pip install --no-cache-dir "platformio==${PLATFORMIO_VERSION}"

ENV PLATFORMIO_CORE_DIR=/pio
WORKDIR /project

CMD ["pio", "run"]
