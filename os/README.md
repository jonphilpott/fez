# FEZ live USB image (x86_64)

A USB stick that starts straight into FEZ on an Intel or AMD PC, old laptops included. Nothing is installed: the laptop's own disk is not touched.

## What it is

- **Buildroot** (pinned: 2026.02.3, in `in-container.sh`) builds one Linux kernel with the whole system inside it (an initramfs): BusyBox, udev, ALSA, SDL2, Xorg (no window manager, no cursor) and FEZ. It is about 34 MB and runs from RAM.
- The stick is laid out like this (both partitions are FAT32, so any computer can read them):
  - **Boot partition**, 96 MB, labelled `FEZBOOT`: the kernel once (`EFI/BOOT/BOOTX64.EFI`, which syslinux starts on old BIOS machines and the firmware starts on UEFI ones). It is all the image holds, so it is quick to download and flash.
  - **Data partition**, labelled `FEZ`: made **on the first start**, in all the free space after the boot partition, so a 32 GB stick gives about 32 GB for recordings. The new partition is empty, so nothing can be lost; the script touches only a disk whose single partition is `FEZBOOT`, and falls back to RAM if anything fails.
  - The data partition has a `FEZ/` folder: `patches/`, `recordings/`, `setup.txt` and, after each run, `fez.log` and `xorg.log` for finding out why a machine would not play.
- At start `fez-boot` finds the partition by its label, mounts it with the `flush` option, sets `HOME` to it and runs `xinit fez --fullscreen`. **Ctrl+Q**, then Enter or Ctrl+Q again at the "quit?" box (so a slip cannot end a performance), ends FEZ, flushes and unmounts the stick, and powers the machine off. Alt+F2 gives a shell on the console.

## Building

Needs Docker (Colima works; give it 8 CPUs and 8 GB, or expect a long wait). Nothing is installed on the Mac.

    os/build.sh                 # Buildroot in a Docker volume (an hour the first time, minutes after); gives os/out/bzImage
    os/make-image.sh            # os/out/fez.img, 96 MB: write it to a stick with Etcher, or dd
    DEBUG=1 os/make-image.sh    # the same, with boot messages on the serial port (for the QEMU tests)
    os/run-qemu.sh bios 90      # boot it as an old BIOS PC would, on a 1 GB pretend stick; also: uefi, kernel
                                # leaves os/out/qemu/shot.png (screen), serial.log, audio.wav
    KEEP=1 os/run-qemu.sh bios  # boot the same pretend stick again (a second start)

A release is made by `tools/release.sh` (it runs the two build steps above, zips the image, and attaches the source code too). **The source offer:** the image holds Linux, glibc, BusyBox, X.Org and other free software, whose licences ask us to provide the source. `os/legal.sh` makes `os/out/FEZ-usb-source.tar.gz` (about 350 MB: Buildroot's `legal-info` sources and licence texts, the Buildroot scripts, this `os/` folder); every release carries it as `FEZ-usb-source-vX.Y.Z.tar.gz`, and `os/SOURCE.txt` on the image's boot partition says so. If you change what goes into the image, nothing else is needed: it collects what the build used.

Buildroot does not notice a changed option on a package it has already built: after editing a package's options in the defconfig, run `os/build.sh <package>-dirclean all` once.

The tree is a Buildroot external one: `configs/fez_x86_64_defconfig` (what goes in), `board/fez/linux.fragment` (kernel options), `board/fez/overlay/` (files for the root filesystem: the start-up scripts), `package/fez/` (how FEZ is built from this repository's own Makefile). `os/build.sh` copies the working tree (uncommitted edits too) into the build volume.

## Known gaps

- Tested in QEMU (BIOS and UEFI) and on real machines (27 Nov 2026): a 2014 MacBook Air and a Ryzen ThinkPad T14, both from the small image on a 32 GB stick, with recordings landing on the data partition. Not yet tried on a BIOS-only machine. The first-start step has not been tried on a stick that holds other partitions (it should refuse), nor on one smaller than about 160 MB.
- No GPU firmware is included, so some newer AMD and Radeon cards will not start (the Xorg `vesa` and `fbdev` drivers are the fallback). Old Intel graphics need none.
- Secure Boot must be off. US keyboard layout only.
- The sound has not been judged: the pretend sound card only proves that FEZ opens a device and writes samples.
- The default sound card may be the HDMI one on some machines; the F2 setup page picks another and remembers it in `setup.txt`.
- Needs a 64-bit CPU and about 1 GB of RAM; the CPU bar in the status bar says whether the machine copes.
