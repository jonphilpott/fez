#!/bin/sh
# Launch script for the FEZ pak. Patches and recordings go to FEZ/ on the card (the app uses $HOME/FEZ), so they are easy to find
# from a computer. The log (fez.log) is next to the app: look there first if it does not start.
cd "$(dirname "$0")" || exit 1
export HOME="${SDCARD_PATH:-/mnt/SDCARD}"
./fez --brick > fez.log 2>&1
