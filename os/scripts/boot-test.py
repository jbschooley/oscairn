#!/usr/bin/env python3
"""Boots an Oscairn QEMU image and checks that Oscairn starts.

Runs QEMU's generic ARM64 machine with a Cortex-A76, a 1024 x 600 virtio display and the
kernel's dummy sound card. Passes when, within the time limit:
- the session reports that Oscairn is running;
- Oscairn has not exited;
- two screenshots taken 3 s apart both show a picture and differ (the playhead moves,
  so the audio callback is running).
Writes the console log and the screenshots to the output folder.

    boot-test.py IMAGES_DIR OUTPUT_DIR [--timeout SECONDS]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

from PIL import Image, ImageChops


def monitor(sock_path, command):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    s.settimeout(5)
    time.sleep(0.2)
    try:
        s.recv(4096)  # banner
    except socket.timeout:
        pass
    s.sendall((command + "\n").encode())
    time.sleep(1.0)
    s.close()


def screenshot(sock_path, out_dir, name):
    ppm = f"/tmp/{name}.ppm"
    if os.path.exists(ppm):
        os.remove(ppm)
    monitor(sock_path, f"screendump {ppm}")
    for _ in range(20):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.25)
    image = Image.open(ppm).convert("RGB")
    image.save(os.path.join(out_dir, f"{name}.png"))
    return image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("images")
    parser.add_argument("out")
    parser.add_argument("--timeout", type=float, default=600)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    console = os.path.join(args.out, "console.log")
    sock = "/tmp/oscairn-qemu-monitor.sock"
    for path in (console, sock):
        if os.path.exists(path):
            os.remove(path)

    qemu = subprocess.Popen([
        "qemu-system-aarch64", "-M", "virt", "-cpu", "cortex-a76", "-smp", "4", "-m", "2048",
        "-kernel", os.path.join(args.images, "Image"),
        "-append", "rootwait root=/dev/vda console=ttyAMA0",
        "-drive", f"file={os.path.join(args.images, 'rootfs.ext4')},if=none,format=raw,id=hd0,snapshot=on",
        "-device", "virtio-blk-device,drive=hd0",
        "-device", "virtio-gpu-pci,xres=1024,yres=600,romfile=",
        "-device", "virtio-keyboard-pci", "-device", "virtio-tablet-pci",
        "-nic", "none", "-display", "none", "-serial", f"file:{console}",
        "-monitor", f"unix:{sock},server,nowait",
    ])
    start = time.time()
    result = 1
    try:
        running = False
        while time.time() - start < args.timeout:
            if qemu.poll() is not None:
                print("FAIL: QEMU exited", qemu.returncode)
                break
            text = open(console, errors="replace").read() if os.path.exists(console) else ""
            if "oscairn-session: Oscairn running" in text:
                running = True
                break
            time.sleep(2)
        elapsed = time.time() - start
        if not running:
            print(f"FAIL: Oscairn did not report running within {elapsed:.0f} s")
        else:
            print(f"Oscairn running {elapsed:.0f} s after power-on")
            time.sleep(5)
            a = screenshot(sock, args.out, "screen-1")
            time.sleep(3)
            b = screenshot(sock, args.out, "screen-2")
            text = open(console, errors="replace").read()
            lit = a.convert("L").getextrema()[1] > 64
            moved = ImageChops.difference(a, b).getbbox() is not None
            exited = "Oscairn exited" in text
            print(f"screen {a.size[0]}x{a.size[1]}, has content: {lit}, changes over 3 s: {moved}, Oscairn exited: {exited}")
            result = 0 if lit and moved and not exited else 1
            print("PASS" if result == 0 else "FAIL")
    finally:
        qemu.kill()
        qemu.wait()
        print("console log:", console)
        lines = open(console, errors="replace").read().splitlines() if os.path.exists(console) else []
        print("\n".join(lines[-25:]))
    return result


if __name__ == "__main__":
    sys.exit(main())
