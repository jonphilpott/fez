#!/bin/sh
# KNULLI launcher for the FEZ probe: reports SDL's drivers, the screen, the sound and the button numbers, and plays a tone.
# Its report goes to .data/FEZ/probe.log. Run it once before FEZ itself on a new system.
cd "$(dirname "$(readlink -f "$0")")/.data/FEZ" || exit 1
./probe > probe.log 2>&1
