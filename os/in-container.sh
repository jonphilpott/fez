#!/bin/sh
# Runs inside the build container (started by build.sh). Configures Buildroot with this tree and builds.
set -e
BR=/build/buildroot-2026.02.3
OUT=/build/out
make -C "$BR" O="$OUT" BR2_EXTERNAL=/fez/os fez_x86_64_defconfig
# FEZ is rebuilt from the fresh copy of the tree every time (it takes seconds); everything else is reused.
make -C "$OUT" FEZ_SRCDIR=/build/fez-src fez-dirclean
make -C "$OUT" FEZ_SRCDIR=/build/fez-src "$@"
