# The build machine for the FEZ live USB image: Ubuntu with what Buildroot needs. Buildroot refuses to run as root, so a user does the work.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential ca-certificates wget curl cpio unzip rsync bc file python3 perl patch gzip bzip2 xz-utils \
      libssl-dev libelf-dev flex bison libncurses-dev git sed which \
      dosfstools mtools \
 && rm -rf /var/lib/apt/lists/*
RUN useradd -m build && mkdir /build && chown build /build
USER build
WORKDIR /build
