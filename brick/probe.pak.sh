#!/bin/sh
# Launch script for the FEZ probe pak. The launcher runs this with the pak folder as the place the script lives.
cd "$(dirname "$0")" || exit 1
./probe > probe.log 2>&1
