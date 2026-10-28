#!/bin/sh
# Builds an Oscairn OS image with Buildroot, in a Linux container (Docker). The build
# lives in the Docker volume "oscairn-os"; the first build takes an hour or more.
#
#   os/build.sh [defconfig] [make targets...]     default: oscairn_qemu_aarch64_defconfig
#   os/build.sh test                              boot the QEMU image and check Oscairn starts;
#                                                 results in build-os/test
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)

docker build -q -t oscairn-os-builder "$here/docker" > /dev/null
docker volume create oscairn-os > /dev/null
mkdir -p "$repo/build-os"
run() {
	docker run --rm -v oscairn-os:/work -v "$repo":/src:ro -v "$repo/build-os":/out -e HOME=/tmp oscairn-os-builder \
		sh -c "chown builder: /work /out && exec runuser -u builder -- env HOME=/tmp $*"
}

if [ "${1:-}" = test ]; then
	run python3 /src/os/scripts/boot-test.py /work/output/oscairn_qemu_aarch64_defconfig/images /out/test
	exit
fi
defconfig=${1:-oscairn_qemu_aarch64_defconfig}
[ $# -gt 0 ] && shift
run /src/os/scripts/container-build.sh "$defconfig" "$@"
