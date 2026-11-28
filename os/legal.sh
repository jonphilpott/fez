#!/bin/bash
# Makes os/out/FEZ-usb-source.tar.gz: the source code behind the USB image, which the GPL and other licences oblige us to offer
# with it (about 350 MB). tools/release.sh runs this and attaches the file to the release.
#   os/legal.sh            (run os/build.sh first: this collects what that build used)
# It holds:
#   legal-info/       Buildroot's own collection: the source archive of every package in the image (sources/), each licence text
#                     (licenses/), manifest.csv (package, version, licence, where from), buildroot.config
#   buildroot-VERSION/ the Buildroot build system (the scripts that control compilation, which the GPL counts as source)
#   fez/              this repository's os/ folder (the Buildroot configuration, kernel options, start-up scripts) and LICENSE
#   README-SOURCE.txt how to rebuild
# The build tools Buildroot compiled for itself (host-sources, host-licenses) are left out: they are not in the image.
set -e
cd "$(dirname "$0")/.."
BR=buildroot-2026.02.3                                         # keep in step with os/in-container.sh
OUT=os/out; STAGE=$OUT/source-stage
rm -rf $STAGE && mkdir -p $STAGE
docker build -q -t fez-os-build os > /dev/null
echo "collecting the licence information (Buildroot's legal-info)"
os/build.sh legal-info > /dev/null
# The two folders come out of the build volume as one tar stream. legal-info's own README says the Buildroot scripts are not saved
# by it, so they are added here, without Buildroot's download cache (dl/), which legal-info/sources already covers.
docker run --rm -v fez-os-build:/build fez-os-build sh -c "cd /build && tar -cf - \
    --exclude=$BR/dl --exclude=out/legal-info/host-sources --exclude=out/legal-info/host-licenses --exclude=out/legal-info/host-manifest.csv \
    $BR out/legal-info" | tar -xf - -C $STAGE
mv $STAGE/out/legal-info $STAGE/legal-info && rmdir $STAGE/out
mkdir $STAGE/fez
git archive HEAD os LICENSE | tar -xf - -C $STAGE/fez
cp os/SOURCE.txt $STAGE/README-SOURCE.txt
cat >> $STAGE/README-SOURCE.txt <<EOT

How to rebuild the image
------------------------
Install Docker, check out the FEZ repository at the release's tag (git clone https://github.com/jonphilpott/fez), and run
os/build.sh then os/make-image.sh (os/README.md explains both). The fez/ folder here is that repository's os/ folder; the
download addresses of the other packages are in legal-info/manifest.csv, and their archives are in legal-info/sources/.
EOT
tar -czf $OUT/FEZ-usb-source.tar.gz -C $STAGE .
rm -rf $STAGE
ls -l $OUT/FEZ-usb-source.tar.gz
