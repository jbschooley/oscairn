#!/bin/sh
# Runs inside the build container (os/build.sh). /src is the Oscairn checkout (read only);
# /work is a Docker volume holding Buildroot, downloads, ccache and outputs.
set -eu
defconfig=$1
shift
BUILDROOT_VERSION=2026.08.1

cd /work
if [ ! -d buildroot ]; then
	git clone -q --depth 1 --branch "$BUILDROOT_VERSION" https://gitlab.com/buildroot.org/buildroot.git buildroot
fi

# Snapshot the files git knows about (committed or not, ignoring build output), so the
# image builds from the working tree without its build folders.
rm -rf src && mkdir src
git config --global --add safe.directory /src
git -C /src ls-files -z --cached --others --exclude-standard | rsync -a --from0 --files-from=- /src/ src/

out=/work/output/$defconfig
export BR2_DL_DIR=/work/dl BR2_CCACHE_DIR=/work/ccache
make -C buildroot O="$out" BR2_EXTERNAL=/work/src/os/buildroot "$defconfig"
make -C buildroot O="$out" "$@"
