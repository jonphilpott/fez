#!/bin/bash
# Builds the live USB image. Needs Docker (Colima works); takes an hour or more the first time and minutes after.
#   os/build.sh             build everything: results in the Docker volume fez-os-build, copied to os/out/
#   os/build.sh fez-rebuild only rebuild FEZ (after a change to the app), then repack
# The build lives in a Docker volume, not in the repository: Buildroot needs Linux and tens of gigabytes.
set -e
cd "$(dirname "$0")/.."
docker build -q -t fez-os-build os > /dev/null
# Copy the working tree (what git knows about, uncommitted edits included, build products left out) into the volume.
git ls-files -z -co --exclude-standard | while IFS= read -r -d '' f; do [ -e "$f" ] && printf '%s\0' "$f"; done \
  | COPYFILE_DISABLE=1 tar --no-xattrs --null -T - -cf - \
  | docker run --rm -i -v fez-os-build:/build fez-os-build sh -c 'rm -rf /build/fez-src && mkdir /build/fez-src && tar -xf - -C /build/fez-src'
docker run --rm -t -v fez-os-build:/build -v "$PWD":/fez:ro fez-os-build /fez/os/in-container.sh "$@"
mkdir -p os/out
docker run --rm -v fez-os-build:/build fez-os-build sh -c 'cat /build/out/images/bzImage' > os/out/bzImage
ls -l os/out
