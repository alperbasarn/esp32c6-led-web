# Builder image for esp32c6-led-web firmware (ESP-IDF + esp-matter, ESP32-C6).
#
# Base already ships ESP-IDF at $IDF_PATH with its toolchain installed; its
# entrypoint sources export.sh. On top of that we bake esp-matter (pinned to a
# specific SHA) so the publish job never has to clone/install it at runtime.
#
# Tag scheme (see .github/workflows/build-builder-image.yml):
#   idf-<IDF_VERSION>-matter-<short-sha>
# Bumping either pin below means a new tag and an image rebuild.
FROM espressif/idf:v5.4.1

# Keep these in sync with .github/workflows/publish-firmware.yml and the tag.
ARG ESP_MATTER_REF=c6f767254f6267e057916981646a44c65d034254
ENV ESP_MATTER_PATH=/opt/esp-matter

# esp-matter's install.sh builds some Python C-extensions (notably cffi) from
# source with --no-build-isolation. The slim espressif/idf image lacks the
# headers/toolchain they need; the fat GitHub runner has them preinstalled,
# which is why the clone-based workflow never hit this. Install them first.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential pkg-config python3-dev libffi-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Shallow-fetch esp-matter at the pinned SHA, then shallow-init submodules. A
# full recursive clone lands around 28-38 GB; the shallow path is what makes
# this tractable.
RUN set -eux; \
    mkdir -p "$ESP_MATTER_PATH"; \
    cd "$ESP_MATTER_PATH"; \
    git init --quiet; \
    git remote add origin https://github.com/espressif/esp-matter.git; \
    git fetch --depth=1 origin "$ESP_MATTER_REF"; \
    git checkout FETCH_HEAD; \
    git submodule update --init --recursive --depth=1

# Strip `bluezoo` from connectedhomeip's pip requirements/constraints. It's a
# BlueZ mock for host-side tests (we pass --no-host-tool anyway), and recent
# PyPI releases pull in transitive deps that conflict with other CHIP-pinned
# packages, breaking pip-tools compile. Purely a CI/build workaround.
RUN set -eux; \
    chip="$ESP_MATTER_PATH/connectedhomeip/connectedhomeip"; \
    for f in "$chip/scripts/setup/requirements.build.txt" \
             "$chip/scripts/setup/requirements.txt" \
             "$chip/scripts/setup/constraints.txt" \
             "$chip/scripts/tests/requirements.txt"; do \
      if [ -f "$f" ]; then \
        sed -i '/^[[:space:]]*bluezoo/d; /via bluezoo/d' "$f"; \
      fi; \
    done

# Install esp-matter into the image. Source the IDF environment first so
# install.sh sees the toolchain; --no-host-tool skips host-only components.
# IDF_PATH_FORCE=1 makes export.sh trust the IDF_PATH env var instead of trying
# to auto-detect its own location, which fails under `docker RUN`'s /bin/sh
# (dash) because it has no BASH_SOURCE.
RUN set -eux; \
    export IDF_PATH_FORCE=1; \
    . "$IDF_PATH/export.sh" >/dev/null; \
    cd "$ESP_MATTER_PATH"; \
    ./install.sh --no-host-tool

# Bake the build environment so the publish job needs no export step. These
# mirror the "Export build environment" step of publish-firmware.yml.
ENV CHIP_ROOT=$ESP_MATTER_PATH/connectedhomeip/connectedhomeip
ENV PW_PROJECT_ROOT=$CHIP_ROOT
ENV PW_ROOT=$CHIP_ROOT/third_party/pigweed/repo
ENV PATH=$CHIP_ROOT/.environment/cipd/packages/pigweed:$PATH
