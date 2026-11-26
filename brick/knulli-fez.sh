#!/bin/sh
# KNULLI launcher for FEZ. KNULLI lists every .sh in /userdata/roms/ports under Ports; the program itself lives in
# .data/FEZ next to this script (KNULLI hides .data from the game list). Patches and recordings go to /userdata/FEZ/
# (the app uses $HOME/FEZ), which is the top of the SHARE partition, so they are easy to find from a computer.
# If it does not start, look at .data/FEZ/fez.log.
cd "$(dirname "$(readlink -f "$0")")/.data/FEZ" || exit 1
export HOME=/userdata
# A and B: FEZ swaps them by default, because the Brick's stock firmware reports them swapped. If A and B feel backwards
# under KNULLI, change the 1 to 0. (The FEZ Probe port shows which number each button sends.)
export FEZ_PAD_SWAP=1
./fez --brick > fez.log 2>&1
