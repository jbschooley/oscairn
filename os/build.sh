#!/bin/sh
# Builds an Oscairn OS image with Buildroot, in a Linux container (Docker). The build
# lives in the Docker volume "oscairn-os"; the first build takes an hour or more.
#
#   os/build.sh [defconfig] [make targets...]     default: oscairn_qemu_aarch64_defconfig
#   os/build.sh test                              boot the QEMU image and check Oscairn starts;
#                                                 results in build-os/test
#   os/build.sh run                               boot the QEMU image to look at and use, on
#                                                 VNC at localhost:5900 (password oscairn)
#   os/build.sh stop                              stop it
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

if [ "${1:-}" = run ]; then
	images=/work/output/oscairn_qemu_aarch64_defconfig/images
	docker run -d --rm --name oscairn-qemu -p 127.0.0.1:5900:5900 -v oscairn-os:/work oscairn-os-builder \
		qemu-system-aarch64 -M virt -cpu cortex-a76 -smp 4 -m 2048 \
		-kernel $images/Image -append "rootwait root=/dev/vda console=ttyAMA0" \
		-drive file=$images/rootfs.ext4,if=none,format=raw,id=hd0,snapshot=on \
		-device virtio-blk-device,drive=hd0 \
		-device virtio-gpu-pci,xres=1024,yres=600,romfile= \
		-device virtio-keyboard-pci -device virtio-tablet-pci -nic none -serial null \
		-object secret,id=vncpass,data=oscairn -display vnc=0.0.0.0:0,password-secret=vncpass > /dev/null
	echo "QEMU running. Open vnc://localhost:5900 (password: oscairn); stop with os/build.sh stop"
	exit
fi
if [ "${1:-}" = stop ]; then
	docker stop oscairn-qemu > /dev/null && echo stopped
	exit
fi
if [ "${1:-}" = test ]; then
	run python3 /src/os/scripts/boot-test.py /work/output/oscairn_qemu_aarch64_defconfig/images /out/test
	exit
fi
defconfig=${1:-oscairn_qemu_aarch64_defconfig}
[ $# -gt 0 ] && shift
run /src/os/scripts/container-build.sh "$defconfig" "$@"
