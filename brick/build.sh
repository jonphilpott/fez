#!/bin/sh
# Build FEZ for the TrimUI Brick and lay out the two paks to copy onto the SD card. Needs Docker (Colima is fine) running.
#   brick/build.sh            builds dist/FEZ-probe.pak and dist/FEZ.pak (NextUI, MinUI), and dist/knulli/ (KNULLI)
# NextUI / MinUI: copy a pak folder to the card as Tools/tg5040/<name>.pak (they list it under Tools).
# KNULLI: copy what is inside dist/knulli/ports/ (FEZ.sh, FEZ Probe.sh and the hidden .data folder) into roms/ports on
# the SHARE partition (/userdata/roms/ports), then Start > Game settings > Update gamelists. Both appear under Ports.
# TrimUI's own (stock) firmware: copy dist/trimui/Apps/FEZ and FEZProbe into Apps/ on the card; they appear under Apps.
# The same binary serves them all: it needs only libSDL2, libm and libc, and KNULLI has its own SDL2, so none is bundled.
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
# 4) the KNULLI layout: launchers in ports/, the program in ports/.data/FEZ/
rm -rf dist/knulli
mkdir -p dist/knulli/ports/.data/FEZ
cp dist/fez dist/probe dist/knulli/ports/.data/FEZ/
cp brick/knulli-fez.sh "dist/knulli/ports/FEZ.sh"
cp brick/knulli-probe.sh "dist/knulli/ports/FEZ Probe.sh"
chmod +x dist/knulli/ports/*.sh dist/knulli/ports/.data/FEZ/fez dist/knulli/ports/.data/FEZ/probe
# 5) the TrimUI stock firmware layout: Apps/FEZ and Apps/FEZProbe, each a folder with config.json, launch.sh and the icon
#    ("icontop" is the icon centred in its tile, firmware 1.0.5 and later; "icon" stays empty or it would be stretched)
rm -rf dist/trimui
for app in FEZ FEZProbe; do
  mkdir -p dist/trimui/Apps/$app
  cp brick/icon.png dist/trimui/Apps/$app/icon.png
done
cp dist/fez dist/trimui/Apps/FEZ/fez
cp brick/trimui-fez.sh dist/trimui/Apps/FEZ/launch.sh
printf '{\n  "label": "FEZ",\n  "icon": "",\n  "icontop": "icon.png",\n  "launch": "launch.sh",\n  "description": "Personal Sound Computer"\n}\n' > dist/trimui/Apps/FEZ/config.json
cp dist/probe dist/trimui/Apps/FEZProbe/probe
cp brick/trimui-probe.sh dist/trimui/Apps/FEZProbe/launch.sh
printf '{\n  "label": "FEZ Probe",\n  "icon": "",\n  "icontop": "icon.png",\n  "launch": "launch.sh",\n  "description": "Reports the screen, sound and buttons to probe.log"\n}\n' > dist/trimui/Apps/FEZProbe/config.json
chmod +x dist/trimui/Apps/*/launch.sh dist/trimui/Apps/FEZ/fez dist/trimui/Apps/FEZProbe/probe

file dist/probe dist/fez || true
echo "done:"
echo "  NextUI / MinUI: copy dist/FEZ-probe.pak (first) and dist/FEZ.pak to the card under Tools/tg5040/"
echo "  KNULLI: copy the contents of dist/knulli/ports/ (including .data) into roms/ports, then update gamelists"
echo "  TrimUI stock firmware: copy dist/trimui/Apps/FEZ (and FEZProbe) into Apps/ on the card"
