#!/bin/bash
# Boots the image in a pretend PC (QEMU in Docker, no window) and records what happened: a screenshot, the serial console and
# the sound the pretend sound card was given.
#   os/run-qemu.sh [MODE] [SECONDS]
#     MODE kernel (default)  start os/out/bzImage directly
#     MODE bios              start os/out/fez.img as an old BIOS machine would (a USB stick)
#     MODE uefi              start os/out/fez.img as a UEFI machine would
#   IMG=fez-small.img GROW=1024 os/run-qemu.sh bios   boot a copy of that image on a pretend 1024 MB stick (os/out/disk-test.img), to test the
#                                                      first-start partition step; run again (without NEWDISK=1) to boot the same disk a second time
#   then wait SECONDS (default 60), press Ctrl+Q and then Enter (FEZ quits and the machine powers off) and look.
# Results in os/out/qemu/: serial.log, shot.png, audio.wav. (The pretend CPU is emulated, so it is slow: allow a minute.)
set -e
cd "$(dirname "$0")/.."
MODE=${1:-kernel}; WAIT=${2:-60}
mkdir -p os/out/qemu && rm -f os/out/qemu/*
IMG=${IMG:-fez.img}
if [ -n "$GROW" ]; then       # a bigger pretend stick than the image, as a real one would be
  if [ -n "$NEWDISK" ] || [ ! -f os/out/disk-test.img ]; then cp "os/out/$IMG" os/out/disk-test.img && truncate -s "${GROW}M" os/out/disk-test.img; fi
  IMG=disk-test.img
fi
docker build -q -t fez-os-qemu -f os/Dockerfile.qemu os > /dev/null
case $MODE in
  kernel) BOOT="-kernel /out/bzImage -append 'console=ttyS0 loglevel=4 quiet'" ;;
  bios)   BOOT="-drive if=none,id=stick,file=/out/$IMG,format=raw -device usb-storage,drive=stick,bootindex=0" ;;
  uefi)   BOOT="-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd -drive if=pflash,format=raw,file=/tmp/vars.fd -drive if=none,id=stick,file=/out/$IMG,format=raw -device usb-storage,drive=stick,bootindex=0" ;;
esac
docker run --rm -v "$PWD/os/out":/out fez-os-qemu sh -c "
  cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/vars.fd
  qemu-system-x86_64 -m 1024 -smp 2 -cpu qemu64 -display none -vga ${VGA:-std} -device usb-ehci -device usb-kbd $BOOT \\
    -serial file:/out/qemu/serial.log \\
    -device intel-hda -device hda-duplex,audiodev=snd0 -audiodev wav,id=snd0,path=/out/qemu/audio.wav \\
    -monitor unix:/tmp/mon,server,nowait &
  sleep $WAIT
  echo 'screendump /out/qemu/shot.ppm' | nc -U -q1 /tmp/mon > /dev/null
  sleep 2
  echo 'sendkey ctrl-q' | nc -U -q1 /tmp/mon > /dev/null
  sleep 1
  echo 'sendkey ret' | nc -U -q1 /tmp/mon > /dev/null      # Enter answers the quit dialog
  sleep 15
  echo quit | nc -U -q1 /tmp/mon > /dev/null
  wait
" 2>&1 | grep -v "Could not create a backend"
sips -s format png os/out/qemu/shot.ppm --out os/out/qemu/shot.png > /dev/null 2>&1 && rm -f os/out/qemu/shot.ppm
ls -l os/out/qemu
