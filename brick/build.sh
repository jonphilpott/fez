#!/bin/sh
# Build FEZ for the TrimUI Brick and lay out the two paks to copy onto the SD card. Needs Docker (Colima is fine) running.
#   brick/build.sh            builds dist/FEZ-probe.pak and dist/FEZ.pak
# Copy a pak folder to the card as Tools/tg5040/<name>.pak (NextUI and MinUI list it under Tools).
set -eu
cd "$(dirname "$0")/.."

command -v docker >/dev/null || { echo "docker not found. With Colima: brew install docker, then colima start"; exit 1; }
docker info >/dev/null 2>&1 || { echo "docker is installed but not running. With Colima: colima start"; exit 1; }

# 1) the toolchain image: an aarch64 compiler plus the Brick's own SDL2 headers and libraries, published by LoveRetro (the NextUI project).
#    (Building it from their Dockerfile failed in a side library we do not need, so we take the published image.)
IMAGE=ghcr.io/loveretro/tg5040-toolchain:latest
docker image inspect "$IMAGE" >/dev/null 2>&1 || docker pull "$IMAGE"

# 2) compile inside the container; the repo is mounted at /src and the binaries land in dist/
docker run --rm -v "$PWD":/src -w /src "$IMAGE" make brick

# 3) lay out the paks: a folder per app with its launch.sh
rm -rf dist/FEZ-probe.pak dist/FEZ.pak
mkdir -p dist/FEZ-probe.pak dist/FEZ.pak
cp dist/probe dist/FEZ-probe.pak/probe
cp brick/probe.pak.sh dist/FEZ-probe.pak/launch.sh
cp dist/fez dist/FEZ.pak/fez
cp brick/fez.pak.sh dist/FEZ.pak/launch.sh
chmod +x dist/FEZ-probe.pak/launch.sh dist/FEZ.pak/launch.sh
file dist/probe dist/fez || true
echo "done: copy dist/FEZ-probe.pak (first) and dist/FEZ.pak to the card under Tools/tg5040/"
