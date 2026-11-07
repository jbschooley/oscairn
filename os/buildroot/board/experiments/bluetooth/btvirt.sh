#!/bin/sh
# Builds BlueZ's virtual controller, btvirt, in a variant's built BlueZ tree, for measure.py.
# Buildroot does not pass --enable-testing, so btvirt's automake variables are given here.
# Runs in the build container: btvirt.sh OUTPUT_DIR  (prints the binary's path)
set -eu
cd "$1"/build/bluez5_utils-*
export PATH="$1/host/bin:$PATH"
make emulator/btvirt \
	am_emulator_btvirt_OBJECTS="emulator/main.o emulator/serial.o emulator/server.o emulator/vhci.o emulator/btdev.o emulator/bthost.o emulator/smp.o emulator/phy.o emulator/le.o" \
	emulator_btvirt_LDADD="lib/libbluetooth-internal.la src/libshared-mainloop.la" \
	emulator_btvirt_DEPENDENCIES="lib/libbluetooth-internal.la src/libshared-mainloop.la" > /dev/null
echo "$PWD/emulator/btvirt"
