#!/bin/sh
# Remakes the manual's screenshots, docs/shots/*.png, from the real screen-drawing code (no window is opened).
# Run it after a change to how a screen looks, then `make manual` (which embeds the pictures in the page).
# It needs `sips`, the picture converter that comes with macOS. On another system, replace the `conv` line with
# ImageMagick's `convert $1 $2` or netpbm's `pnmtopng`.
set -e
cd "$(dirname "$0")/.."
make deskshot padshot
t=$(mktemp -d)
conv() { sips -s format png "$1" --out "docs/shots/$2.png" >/dev/null; }

# the handheld screens, each from the example patch that shows it best
./padshot 1  "$t/gb"  >/dev/null     # Glass Bells
./padshot 3  "$t/r30" >/dev/null     # Rule 30 Rain (the cellular automaton)
./padshot 5  "$t/lz"  >/dev/null     # Lorenz Walk
./padshot 8  "$t/sm"  >/dev/null     # Step Melody (the sequencer)
./padshot 13 "$t/dl"  >/dev/null     # Deep Loop (the buffer)
conv "$t/gb_00_editor.ppm"        pad-editor
conv "$t/gb_10_palette.ppm"       pad-palette
conv "$t/gb_11b_palette_regs.ppm" pad-palette-regs
conv "$t/gb_11d_palette_reference.ppm" pad-palette-reference
conv "$t/gb_12_menu.ppm"          pad-menu
conv "$t/gb_12b_actions.ppm"      pad-actions
conv "$t/gb_12c_menu_main.ppm"    pad-menu-main
conv "$t/gb_14_name.ppm"          pad-name
conv "$t/gb_16_confirm.ppm"       pad-confirm
conv "$t/gb_15_help.ppm"          pad-help
conv "$t/gb_01_patches.ppm"       pad-patches
conv "$t/gb_02_ctrl.ppm"          pad-ctrl
conv "$t/gb_07_matrix.ppm"        pad-matrix
conv "$t/gb_05_pulsers.ppm"       pad-pulsers
conv "$t/gb_09_sticks.ppm"        pad-sticks
conv "$t/r30_03b_cellular.ppm"    pad-cellular
conv "$t/lz_03c_lorenz.ppm"       pad-lorenz
conv "$t/sm_06_seq.ppm"           pad-seq
conv "$t/dl_04_buffer.ppm"        pad-buffer

# the desktop layouts, at the width of the manual's text column so they are shown at full sharpness
FEZ_SHOT_CLEAN=1 ./deskshot 1 648 380 "$t/dk"  >/dev/null
FEZ_SHOT_CLEAN=1 ./deskshot 3 648 380 "$t/dkc" >/dev/null
conv "$t/dk_layout1.ppm"  desk-layout1
conv "$t/dk_layout2.ppm"  desk-layout2
conv "$t/dkc_layout3.ppm" desk-layout3
FEZ_SHOT_CLEAN=1 ./deskshot 14 1296 380 "$t/dkw" >/dev/null      # layout 4 is for wide screens, so its picture is twice as wide
conv "$t/dkw_layout4.ppm" desk-layout4
rm -rf "$t"
ls -l docs/shots
