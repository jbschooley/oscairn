#!/bin/sh
# Linux arm64 builds of the ports and Oscairn's checks, run inside oscairn-ports-linux:local.
set -e
out=/ports/linux; mkdir -p $out/lv2
cmake -S /oscairn -B $out/oscairn -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOSCAIRN_LV2=ON > $out/oscairn-configure.log 2>&1
cmake --build $out/oscairn --target oscairn-lv2-production-probe oscairn-plugin-check > $out/oscairn-build.log 2>&1
echo oscairn built
for p in chorusflanger tremolo rotspeak vibrato; do
  b=$out/lv2/tap-$p.lv2; mkdir -p $b
  gcc -O3 -ffast-math -Wall -fPIC -I/ports/tap-lv2/utils -shared -o $b/tap_$p.so /ports/tap-lv2/$p/*.c -lm
  cp /ports/tap-lv2/$p/*.ttl $b/
done
for p in PhaserII:Phaser ChorusI:Chorus; do
  n=${p%%:*}; s=${p##*:}; b=$out/lv2/mod-caps-$n.lv2; mkdir -p $b
  g++ -O3 -ffast-math -Wall -fPIC -I/ports/caps-lv2 -fvisibility=hidden -fvisibility-inlines-hidden -shared -o $b/$n.so \
    /ports/caps-lv2/$s.cc /ports/caps-lv2/dsp/polynomials.cc /ports/caps-lv2/plugins/mod-caps-$n.lv2/interface.cc
  cp /ports/caps-lv2/plugins/mod-caps-$n.lv2/*.ttl $b/
done
echo lv2 built
cmake -S /ports/odin2 -B $out/odin2 -G Ninja -DCMAKE_BUILD_TYPE=Release > $out/odin2-configure.log 2>&1
cmake --build $out/odin2 --target Odin2_CLAP -j 2 > $out/odin2-build.log 2>&1
echo odin2 built
