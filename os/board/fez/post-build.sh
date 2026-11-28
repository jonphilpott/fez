#!/bin/sh
# Run by Buildroot on the finished root filesystem ($1). Remove the X start-up script the xorg package installs:
# fez-boot starts X itself, with FEZ as its client.
rm -f "$1/etc/init.d/S40xorg"
