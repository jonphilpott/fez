#!/bin/sh
# Launch script for FEZ as an app of the TrimUI's own (stock) firmware: Apps/FEZ/ on the card, listed under Apps.
# The program is the same one the NextUI pak runs. Patches and recordings go to FEZ/ on the card ($HOME/FEZ); the log
# (fez.log) is next to this script: look there first if it does not start.
cd "$(dirname "$0")" || exit 1
export HOME=/mnt/SDCARD
export LD_LIBRARY_PATH="/usr/trimui/lib:${LD_LIBRARY_PATH:-}"   # where the stock firmware keeps its SDL2
./fez --brick > fez.log 2>&1
