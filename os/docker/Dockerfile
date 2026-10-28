# Build environment for the Oscairn OS images: Buildroot's host requirements, and QEMU to
# boot the test image. Debian 13 (trixie).
FROM debian:trixie

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        bash bc binutils build-essential bzip2 ca-certificates cpio file g++ gcc git gzip \
        libncurses-dev make patch perl python3 rsync sed tar unzip wget xz-utils \
        cmake ninja-build pkg-config \
        qemu-system-arm qemu-utils python3-pil \
    && rm -rf /var/lib/apt/lists/*

# Buildroot should not run as root.
RUN useradd --create-home --uid 1000 builder
