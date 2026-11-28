#!/bin/bash
# Makes os/out/fez.img from os/out/bzImage: a disk image for a USB stick (write it with Etcher or dd).
#   os/make-image.sh [SIZE_MB]      (default 512; the stick must be at least that big)
#   DEBUG=1 os/make-image.sh        also send the boot messages to the serial port (for the QEMU tests)
#   SMALL=1 os/make-image.sh        a small image, os/out/fez-small.img (default 96 MB), for release: only the boot partition, labelled
#                                   FEZBOOT. On its first start the stick adds the FEZ data partition in all the space after it,
#                                   so any size of stick gives room for recordings. (fez-boot does this.)
# One FAT32 partition labelled FEZ, so any computer can read it. It holds:
#   bzImage + syslinux          for old machines that start from BIOS
#   EFI/BOOT/BOOTX64.EFI        for machines that start from UEFI (the same kernel file: Linux can be its own UEFI program)
#   FEZ/patches, FEZ/recordings where FEZ keeps its files, in plain view
set -e
cd "$(dirname "$0")/.."
SIZE=${1:-$([ -n "$SMALL" ] && echo 96 || echo 512)}
OUTNAME=$([ -n "$SMALL" ] && echo fez-small.img || echo fez.img)
LABEL=$([ -n "$SMALL" ] && echo FEZBOOT || echo FEZ)
# The tools run in an x86 container (--platform linux/amd64): the BIOS boot loader's installer, syslinux, has no ARM build. It only
# moves a few files around, so running it emulated is fine. (Made with run and commit because `docker build` ignored the platform here.)
if ! docker image inspect fez-os-image > /dev/null 2>&1; then
  docker rm -f fez-os-image-tmp > /dev/null 2>&1 || true
  docker run --platform linux/amd64 --name fez-os-image-tmp ubuntu:24.04 sh -c \
    'apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends syslinux syslinux-common dosfstools mtools fdisk' > /dev/null
  docker commit fez-os-image-tmp fez-os-image > /dev/null && docker rm fez-os-image-tmp > /dev/null
fi
docker run --rm --platform linux/amd64 -e DEBUG="$DEBUG" -e SMALL="$SMALL" -e OUTNAME="$OUTNAME" -e LABEL="$LABEL" -v "$PWD/os/out":/out fez-os-image sh -c '
  set -e
  IMG=/out/$OUTNAME; MB='"$SIZE"'; OFF=1048576
  rm -f $IMG; truncate -s ${MB}M $IMG
  # an MBR partition table (the kind every BIOS and UEFI firmware understands), one FAT32 partition, marked bootable
  echo "start=2048, type=c, bootable" | sfdisk -q $IMG
  dd if=/usr/lib/syslinux/mbr/mbr.bin of=$IMG bs=440 count=1 conv=notrunc 2>/dev/null
  mformat -i $IMG@@$OFF -F -v $LABEL -T $(( (MB * 2048) - 2048 )) ::
  mmd -i $IMG@@$OFF ::/EFI ::/EFI/BOOT
  mcopy -i $IMG@@$OFF /out/bzImage ::/EFI/BOOT/BOOTX64.EFI
  if [ -n "$SMALL" ]; then KERNEL=/EFI/BOOT/BOOTX64.EFI      # one copy of the kernel: the BIOS loader can start the UEFI file too
  else KERNEL=/bzImage; mmd -i $IMG@@$OFF ::/FEZ ::/FEZ/patches ::/FEZ/recordings; mcopy -i $IMG@@$OFF /out/bzImage ::/bzImage; fi
  printf "DEFAULT fez\nPROMPT 0\nTIMEOUT 1\nLABEL fez\n  KERNEL $KERNEL\n  APPEND quiet loglevel=3 ${DEBUG:+console=ttyS0}\n" > /tmp/syslinux.cfg
  mcopy -i $IMG@@$OFF /tmp/syslinux.cfg ::/syslinux.cfg
  mcopy -i $IMG@@$OFF /usr/lib/syslinux/modules/bios/ldlinux.c32 ::/ldlinux.c32
  syslinux --offset $OFF --install $IMG
  mdir -i $IMG@@$OFF ::/
'
ls -l os/out/${OUTNAME}
