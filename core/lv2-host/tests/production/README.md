# Production LV2 compatibility checks

Results recorded on 2026-10-28, on top of Worker commit
`114e910b1e04f2277c7db48d3716cb5c397b475e`. These use the normal `loadPlugin`
adapter and mixed CLAP/LV2 renderer, not the direct-ABI sampler harness.

| Plugin | macOS arm64 | Linux arm64 | Parameter exercised |
| --- | --- | --- | --- |
| MOD MDA JX10 synthesizer | Pass | Pass | VCF Freq |
| MOD MDA DX10 FM synthesizer | Pass | Pass | Mod Init |
| MOD MDA ePiano | Pass | Pass | Treble Boost |
| MOD MDA Ambience | Pass | Pass | Mix |
| MOD MDA DubDelay | Pass | Pass | FX Mix |
| MOD MDA Overdrive | Pass | Pass | Drive |
| ZamAudio ZamComp | Not tested | Pass | Threshold |
| Guitarix GxChorus-Stereo | Not tested | Pass | Depth |
| Guitarix GxCompressor | Not tested | Pass | Threshold |
| Guitarix GxZita_rev1-Stereo | Not tested | Pass | Dry/wet mix |
| x42 fil4 stereo EQ | Not tested | Rejected | Requires larger Atom buffer |

## What passed and what did not

Each supported plugin rendered two seconds at 44.1/48 kHz, 37/512-frame blocks,
in offline and realtime host modes. Every render produced finite, nonzero audio
with zero host processing errors or dropped events. Each also passed a fresh-instance
repeat and a parameter edit that changed the audio beyond any repeat variation.
All baseline repeats were sample-identical on their respective platforms.

The three instruments were silent before the first scheduled note. Omitting note-offs
in a separate render produced a louder final tail than sending them, demonstrating
release behavior (JX10 retains its long release; DX10/ePiano reached silence).
Effects received a deterministic stereo/mono tone signal with an amplitude change
and silence at the end. ZamComp's default threshold leaves that signal unchanged;
lowering its threshold produces measurable compression.

There were **166 individual-plugin renders**, plus **12 mixed-chain renders**:
Oscairn CLAP sequencer → JX10 → Ambience, sequencer → DX10 → DubDelay, and
sequencer → ePiano → Overdrive, each at 37/512 frames on both platforms.
The chains produced finite, nonzero audio without reported processing errors.

Block-size independence is measured, not assumed. At 48 kHz, DX10, Overdrive,
ZamComp and all three Guitarix effects were sample-identical between 37/512 frames.
ePiano differed by about 2.57e-7 RMS; JX10, Ambience and DubDelay differed by
0.03625, 0.001372 and 0.13427 RMS respectively. DubDelay's source resets its
100-sample modulation update counter at each `processReplacing` call, providing
a concrete upstream source of block dependence. These results do not establish
the cause of every plugin's difference, and no plugin DSP was modified.

The packaged x42 stereo EQ requests a 131,424-byte Atom input buffer. Oscairn's
current 16,384-byte limit rejects it before instantiation, with
`LV2 atom buffer minimum exceeds 16384 bytes`. Its requirements were not stripped.
Supporting it needs a separately tested change to buffer sizing/limits; it is not
counted as a successful render. The runner records this known rejection as
`unsupported`, distinguishes it from other failures, and still tests all other cases.

This is automated functional coverage, **not an audible listening test or a realtime
CPU/deadline benchmark**. Realtime mode uses the normal live host path but runs as fast
as possible. GUI, presets/state/patch, file loading, MIDI expression, every control,
long-session stability, and actual MOD hardware were not tested. These plugins do
not replace the separate Worker fixture/upstream-sampler coverage.

## Exact external inputs and credits

The MOD selections follow
[mod-plugin-builder at 9442804](https://github.com/mod-audio/mod-plugin-builder/tree/9442804cf044ee41c19f9a62c0fd3b8915690161).
Its MDA, Zam and x42 recipes include these plugin families; its Guitarix recipe
includes GxCompressor. The additional chorus/reverb tests use the Debian Guitarix
distribution. Packaged Debian plugins are not claimed to be MOD's shipping builds.

- [MOD MDA source](https://github.com/mod-audio/mda-lv2/tree/b2df88cb28540856b7ec7d0210809efd7ca6bcd7),
  revision `b2df88cb28540856b7ec7d0210809efd7ca6bcd7`, exactly matching that MOD recipe.
- [MOD metadata](https://github.com/mod-audio/mod-lv2-data/tree/4d4fb43010296d1e672cc0759a4d1afdb7f02fe0),
  revision `4d4fb43010296d1e672cc0759a4d1afdb7f02fe0`, matching the builder's
  `lv2-data` submodule. The six `plugins-fixed/mod-mda-*.lv2` root Turtle files
  are applied without edits. GUI assets are not installed for these DSP tests.
- Debian 13 arm64 packages: `zam-plugins 4.2+ds-1+b1`,
  `guitarix-lv2 0.46.0+dfsg-1+b1`, `x42-plugins 20240611+ds-1+b1`.
- macOS 15.6.1 arm64 native build; Linux arm64 runs in a disposable Debian container.

MDA DSP is by Paul Kellett, with the LV2 wrapper by David Robillard and MOD
contributors; their source headers/licenses remain intact in the external checkout.
ZamAudio, Guitarix and x42 retain their upstream authorship and licensing. No plugin
DSP code is copied into Oscairn. `build_mod_mda.py` uses the compiler inputs/defines
from MDA's `wscript`, replacing its bundled Waf launcher because that launcher fails
to parse on current Python. It does not patch source code or weaken feature metadata.

## Repeat the tests

Build Oscairn with `OSCAIRN_LV2=ON`. Check out the two pinned MOD repositories above,
then run from the Oscairn root (replace the two source paths):

```sh
python3 core/lv2-host/tests/production/build_mod_mda.py /path/to/mda-lv2 build/production-lv2 --mod-data /path/to/mod-lv2-data
python3 core/lv2-host/tests/production/run.py build/core/lv2-host/tests/oscairn-lv2-production-probe build/production-results --mod-root build/production-lv2 --renderer build/hosts/oscairn-render --sequencer build/plugins/oscairn-step-sequencer.clap
```

For Debian package coverage, install `zam-plugins guitarix-lv2 x42-plugins` in a
test environment and add `--system-root /usr/lib/lv2` to the runner command.
The runner launches each native probe in a separate process with a 30-second
timeout, saves float WAVs plus full stdout/stderr and metrics in `results.json`,
and exits nonzero on unexpected load/render failures or ineffective parameter edits.
It needs only Python's standard library. Missing bundles are failures, not skips.

CI builds and exercises the pinned six MOD plugins and mixed chains on Linux and
macOS, uploading the JSON measurements. The packaged Linux tests remain opt-in.
[Recorded measurements](results-2026-10-28.json) preserve the local baseline,
parameter IDs/ranges, repeat/block differences and note-release comparisons.
The local full logs and WAVs are under `/private/tmp/oscairn-production-results/`;
they are disposable test artifacts, not repository assets.
