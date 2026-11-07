#!/usr/bin/env python3
"""Boots a Bluetooth measurement image (RFC 0036) in QEMU and runs guest.sh on it.

Same machine as os/scripts/boot-test.py (virt, Cortex-A76, 4 cores, 2 GB, dummy sound card).
guest.sh, bluetooth-only.conf and BTVIRT go on a second disk, mounted at /mnt. Prints the time
from QEMU's start to the login prompt, then guest.sh's report; the console goes to OUT/console.log.

    measure.py IMAGES_DIR VARIANT OUT MKFS_EXT4 [BTVIRT]

The whole measurement, VARIANT being baseline, bluealsa or pipewire and $O its output folder,
/work/output/experiments/bluetooth_VARIANT_defconfig, in the build container (os/build.sh's
image, with the oscairn-os volume at /work):
    os/build.sh experiments/bluetooth_VARIANT_defconfig                 (on the host)
    btvirt.sh $O                                                        (not for baseline)
    make -C /work/buildroot O=$O graph-size    (needs python3-matplotlib, which the image lacks)
    measure.py $O/images VARIANT OUT $O/host/sbin/mkfs.ext4 [the btvirt path printed above]
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

images, variant, out, mkfs = sys.argv[1:5]
btvirt = sys.argv[5] if len(sys.argv) > 5 else None
here = os.path.dirname(os.path.abspath(__file__))
os.makedirs(out, exist_ok=True)

files = tempfile.mkdtemp()
for name in ("guest.sh", "bluetooth-only.conf"):
    shutil.copy(os.path.join(here, name), files)
if btvirt:
    shutil.copy(btvirt, os.path.join(files, "btvirt"))
extra = os.path.join(out, "extra.ext4")
subprocess.run([mkfs, "-q", "-F", "-d", files, extra, "16M"], check=True)

sock = os.path.join(tempfile.mkdtemp(), "serial.sock")
start = time.time()
qemu = subprocess.Popen([
    "qemu-system-aarch64", "-M", "virt", "-cpu", "cortex-a76", "-smp", "4", "-m", "2048",
    "-kernel", os.path.join(images, "Image"), "-append", "rootwait root=/dev/vda console=ttyAMA0",
    # virtio-mmio disks are numbered in reverse, so the extra disk comes first to become vdb
    "-drive", f"file={extra},if=none,format=raw,id=hd1", "-device", "virtio-blk-device,drive=hd1",
    "-drive", f"file={os.path.join(images, 'rootfs.ext4')},if=none,format=raw,id=hd0,snapshot=on",
    "-device", "virtio-blk-device,drive=hd0",
    "-device", "virtio-gpu-pci,xres=1024,yres=600,romfile=",
    "-nic", "none", "-display", "none",
    "-chardev", f"socket,id=s0,path={sock},server=on,wait=off", "-serial", "chardev:s0",
])
while not os.path.exists(sock):
    time.sleep(0.05)
s = socket.socket(socket.AF_UNIX)
s.connect(sock)
s.settimeout(1)
log = open(os.path.join(out, "console.log"), "w")
text = ""


def wait_for(marker, timeout):
    global text
    end = time.time() + timeout
    while marker not in text:
        if time.time() > end:
            raise SystemExit(f"timed out waiting for {marker!r}")
        try:
            data = s.recv(65536).decode(errors="replace")
        except socket.timeout:
            continue
        text += data
        log.write(data)
        log.flush()


try:
    wait_for("login:", 900)
    print(f"login prompt {time.time() - start:.1f} s after QEMU started")
    s.sendall(b"root\n")
    time.sleep(3)
    s.sendall(f"mount /dev/vdb /mnt && sh /mnt/guest.sh {variant}; echo MEASURE-DONE\n".encode())
    mark = len(text)
    wait_for("\nMEASURE-DONE", 600)
    print(text[mark:])
finally:
    qemu.kill()
    qemu.wait()
