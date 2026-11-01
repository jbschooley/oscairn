#!/bin/sh
# The ports' checks on Linux arm64, inside oscairn-ports-linux:local.
B=/ports/linux/oscairn; L=/ports/linux/lv2; T=http://moddevices.com/plugins/tap; C=http://moddevices.com/plugins/caps
O=/ports/linux/odin2/Odin2_artefacts/Release/CLAP/Odin2.clap
rm -rf /ports/results-linux
python3 /oscairn/core/lv2-host/tests/production/run.py $B/core/lv2-host/tests/oscairn-lv2-production-probe /ports/results-linux \
  --case "Odin2,$O,com.thewavewarden.odin2,synth,1489561359,0.3" \
  --case "TAP-ChorusFlanger,$L/tap-chorusflanger.lv2,$T/chorusflanger,effect,2,20" \
  --case "TAP-Tremolo,$L/tap-tremolo.lv2,$T/tremolo,effect,1,100" \
  --case "TAP-RotarySpeaker,$L/tap-rotspeak.lv2,$T/rotspeak,effect,0,8" \
  --case "TAP-Vibrato,$L/tap-vibrato.lv2,$T/vibrato,effect,3,-20" \
  --case "CAPS-PhaserII,$L/mod-caps-PhaserII.lv2,$C/PhaserII,effect,2,0.2" \
  --case "CAPS-ChorusI,$L/mod-caps-ChorusI.lv2,$C/ChorusI,effect,3,0.9"
for c in "$O com.thewavewarden.odin2 synth 1489561359=0.3" "$L/tap-chorusflanger.lv2 $T/chorusflanger effect 2=20" \
         "$L/mod-caps-PhaserII.lv2 $C/PhaserII effect 2=0.2" "$L/mod-caps-ChorusI.lv2 $C/ChorusI effect 3=0.9"; do
  set -- $c; echo "== check $2"; $B/core/control/oscairn-plugin-check $1 $2 $3 --param $4 2>&1 | grep -v "^oscairn:" | grep -E "FAIL|PASS|differs"
done
