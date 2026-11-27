#!/bin/sh
# Launch script for the FEZ probe as a stock-firmware app (Apps/FEZProbe/). Its report goes to probe.log next to it.
cd "$(dirname "$0")" || exit 1
export LD_LIBRARY_PATH="/usr/trimui/lib:${LD_LIBRARY_PATH:-}"
./probe > probe.log 2>&1
