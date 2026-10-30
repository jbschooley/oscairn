# Sample players, formats and the first native sample milestone

Research and implementation plan, 2026-10-29. The first native slice below is implemented;
see [RFC 0008 step 2 status](rfcs/0008-sfz-native-samples-and-conversion.md#step-2-status). This records the prior-art pass and a headless reuse experiment before changing
Oscairn's plugin contract. It is not a claim of full format compatibility or a production
engine selection. [RFC 0008](rfcs/0008-sfz-native-samples-and-conversion.md) owns the
SFZ-based native format and conversion plan. [ESP observations](yamaha-esp-samples-research.md)
are separate.

## Players worth reusing or learning from

| Project | Evidence and suitability | Decision for Oscairn |
| --- | --- | --- |
| [FluidSynth](https://www.fluidsynth.org/) | Established embeddable SoundFont engine; SF2/SF3 and current DLS support. LGPL-2.1-or-later in inspected source. Separate presets, voices and sample storage. | Strong candidate for a SoundFont/General MIDI engine, particularly Ignite. Use its library rather than reimplement SoundFont synthesis. It does not replace native editable sample-patch semantics automatically. |
| [liquidsfz](https://github.com/swesterfeld/liquidsfz) | Embeddable SFZ library and LV2 plugin, also imports Hydrogen kits. MPL-2.0. Public API documents shared sample caching, preload control and cache-miss telemetry. Source checkout describes release 0.4.2. | First SFZ reuse candidate to test. The local experiment below proves a limited sharing case; native note ownership, expression and resource guarantees remain integration work. |
| [sfizz](https://github.com/sfztools/sfizz) | BSD-2-Clause SFZ library. Reviewed its file pool, reader pins, preload data and background loading. GitHub marks this repository archived on June 21, 2026. | Valuable source/design reference and comparison renderer. Do not select it as a maintained dependency without establishing a supported upstream or accepting maintenance responsibility. |
| [samplv1](https://samplv1.sourceforge.io/) / [drumkv1](https://drumkv1.sourceforge.io/) | Rui Nuno Capela's established sampler/drum-plugin family. samplv1 is GPL-2.0-or-later, uses libsndfile and offers an LV2 plugin. | Keep samplv1 as a production-host interoperability reference; Oscairn already has its portable-state regression. Hosting a sampler is distinct from giving it native shared contexts. drumkv1 is a drum-workflow candidate, not newly tested here. |
| [LinuxSampler](https://www.linuxsampler.org/features.html) | Long-running sampler with GIG/SFZ/SF2 engines and disk streaming. Its [published license terms](https://linuxsampler.org/downloads) restrict use in commercial products without written permission; subprojects such as libgig have separate terms. | Architectural reference, not an unrestricted default dependency. Do not assume ordinary GPL reuse conditions apply to the engine. |
| [Hydrogen](https://hydrogen-music.org/documentation/manual/manual_en.html) | Established drum application. Kits package samples with an XML instrument description; useful examples of layers, kit management and sample attribution metadata. | Learn from its musician workflow and consider kit import through liquidsfz; do not embed the entire application to play samples. |
| [Shortcircuit XT](https://github.com/surge-synthesizer/shortcircuit-xt) | Surge Synth Team's creative sampler, currently advertised as beta. Core source outside `libs/` is MIT; its own notices warn that dependencies can make the combined binary GPLv3. | Useful sample-manager/zone/persistence prior art, not labeled production-stable merely because its predecessor was mature. |

Source inspections are pinned, rather than assuming a moving branch always behaves the
same way:

| Project / revision | Files inspected and concrete finding |
| --- | --- |
| FluidSynth `fc1c55f4627f0050c820d72784c5463e12620140` | `src/sfloader/fluid_samplecache.c`, `fluid_sfont.h`: a process-wide sample-data cache with reference counts; cache identity includes filename, modification time and sample ranges. Voice/sample references are distinct from preset selection. This is not content-hash deduplication across unrelated copies. |
| liquidsfz `4cc901d18b316cd377276f7eb6b8c325329b9860` | `lib/liquidsfz.hh`, `synth.hh`, `samplecache.cc`: Synth instances share a `Global` sample cache; lookup uses filename. `load_internal()` first stops existing voices. Events have a preallocated queue; exhaustion drops an event and logs. Live cache misses substitute zeros; offline mode may wait for loading. |
| sfizz `f5c6e29f23b8057867c08e88f5f6ac6738baa30b` | `src/sfizz/FilePool.h`, `Resources.h`, `Synth.cpp`: multiple regions can reference the same preloaded sample; playback holders pin data, and background work publishes more frames. This inspection does not prove sharing between independent plugin instances. |
| Shortcircuit XT `72ad42336a8cbc2d5f74fecc303c1a48a1a5a426` | `src/scxt-core/sample/sample_manager.{h,cpp}`: sample loading reuses an existing path; unreferenced samples are purged on its serial thread; restore supports relative addresses and missing-sample placeholders. |
| LV2 examples `0bcde338db1c63bbc503b4d1f6d7b55ed43154af` | David Robillard's `plugins/eg-sampler.lv2/sampler.c`: worker-side load/free and state path mapping. Its response handler stops old playback when replacing a sample, so that operation is not Oscairn's seamless context switching. |

These are design references, not copied implementations. Authors retain their credits;
any later source reuse must preserve the applicable notices and record the exact files
and revision. The experiment links separately obtained liquidsfz and libsndfile, neither
vendored nor added to Oscairn's normal build by this change.

## Headless liquidsfz experiment

The [probe](../tools/sampler-research/liquidsfz_probe.cpp) creates its own two-second mono
48 kHz PCM WAV and two SFZ patches, one transposed seven semitones. It fully preloads the
sample, plays the first patch, prepares a second Synth and later destroys it while the
first note remains held. A third Synth supplies an independent first-patch reference.

Result on this Mac with libsndfile 1.2.2: **408,576 bytes reported decoded-sample cache,
one cached file**, unchanged when loading the other two instances. Both patches render
nonzero, different audio. The old voice matches its reference sample for sample before
and after the second instance's lifetime. No cache misses or guarded C++ allocations
during the tested render calls. No audio device is opened.

This measures the library's decoded-cache counter, **not total process RAM**, per-Synth
voice buffers, mapped-file residency or CPU savings. The test is sequential on one thread,
uses a fully preloaded tiny file and does not exercise streaming under pressure. It does
not establish cross-process sharing, arbitrary SFZ correctness or real-time safety of all
library paths. The existing allocation guard covers C++ allocation, not every C allocator,
deallocation, lock or page fault.

Reproduce from the Oscairn root, using an unused output directory:

```sh
git clone https://github.com/swesterfeld/liquidsfz.git /tmp/oscairn-liquidsfz
git -C /tmp/oscairn-liquidsfz checkout 4cc901d18b316cd377276f7eb6b8c325329b9860
c++ -std=c++17 -O2 -pthread -I /tmp/oscairn-liquidsfz/lib \
  $(pkg-config --cflags sndfile) tools/sampler-research/liquidsfz_probe.cpp \
  /tmp/oscairn-liquidsfz/lib/*.cc $(pkg-config --libs sndfile) \
  -o /tmp/oscairn-liquidsfz-probe
/tmp/oscairn-liquidsfz-probe /tmp/oscairn-liquidsfz-probe-assets
```

This deliberately compiles a research executable, not a supported production packaging
configuration. The generated WAV/SFZ files remain in the chosen directory for inspection.

Important integration gaps: loading a new SFZ into a playing Synth kills its voices;
separate prepared Synth objects can share samples but still have separate voice pools.
The inspected public event API has notes, CC and pitch bend, without Oscairn's CLAP note
IDs or explicit poly/channel pressure entry points. Repeated-key ownership, pressure,
NOTE_END, tail completion and event overflow need explicit solutions and tests before
calling a wrapper a native Oscairn engine. A filename cache also requires stable immutable
asset paths; rewriting a file in place must not silently reuse old decoded content.

### Integration gaps verified in source

Checked at liquidsfz `4cc901d18b316cd377276f7eb6b8c325329b9860` for the
[RFC 0008 engine decision](rfcs/0008-sfz-native-samples-and-conversion.md#engine-decision-native-voice-engine-reused-sfz-parts):

- `lib/liquidsfz.hh`: the event API is `add_event_note_on/off(time, channel, key[, velocity])`,
  `add_event_cc` and `add_event_pitch_bend`; telemetry is aggregate `active_voice_count()`.
  `load()` takes a filename only.
- `lib/synth.hh` `note_off()`: releases every active attack voice matching channel and key.
- `lib/samplecache.cc`: a `std::thread` background loader per process-wide cache.
- `lib/` is about 10,000 lines of liquidsfz plus 14,400 of vendored pugixml (Hydrogen import).

### Reference renders against liquidsfz

[`liquidsfz_compare.cpp`](../tools/sampler-research/liquidsfz_compare.cpp) renders one
second of a generated mono 48 kHz tone through liquidsfz and the Oscairn Sampler with the
same one-region patches (2026-10-30, liquidsfz `4cc901d1`, macOS arm64):

| Case | Oscairn / liquidsfz RMS | Residual after matching level |
| --- | --- | --- |
| Key 60, velocity 127 | 2.295 | 0.34% |
| Key 60, velocity 64 | 2.295 | 0.34% |
| Up 7 semitones | 2.295 | 0.31% |
| Down 5 semitones | 2.295 | 0.01% |
| `volume=-6 tune=30` | 2.295 | 0.27% |
| `ampeg_release=0.1`, released at 0.25 s | 2.295 | 0.01% |

The constant level ratio (+7.2 dB) comes from two liquidsfz conventions in `lib/voice.cc`:
a default CC7 volume of 100 through its volume curve ((100/127)², about −4.2 dB) and a
constant-power pan law (centred mono at 1/√2 per channel, −3 dB). The release row was 16%
before the Sampler adopted liquidsfz's exponential release (`lib/envelope.hh`: −60 dB at
the release time, then silence). Velocity follows the same squared curve in both.

Reproduce with the liquidsfz checkout from the probe above, from the Oscairn root after
building `oscairn_sampler`:

```sh
c++ -std=c++17 -O2 -pthread -I core/clap-host/third_party/clap/include -I core/clap-ext/include \
  -I /tmp/oscairn-liquidsfz/lib $(pkg-config --cflags sndfile) tools/sampler-research/liquidsfz_compare.cpp \
  /tmp/oscairn-liquidsfz/lib/*.cc $(pkg-config --libs sndfile) -o /tmp/oscairn-liquidsfz-compare
/tmp/oscairn-liquidsfz-compare build/plugins/oscairn-sampler.clap /tmp/oscairn-liquidsfz-compare-out
```

This compares rendered numbers, not listening. It does not cover stereo samples,
sample-rate conversion, sustain pedal or simultaneous voices.

FluidSynth's [dynamic sample-loading setting](https://www.fluidsynth.org/api/settings_synth.html)
can allocate during preset changes and is documented as unsuitable for real-time use.
Preparation must happen before a performance-time selection, regardless of player choice.

## Formats: different jobs, not one universal replacement

| Format | Useful role | Boundary |
| --- | --- | --- |
| WAV, AIFF, FLAC | Audio assets; WAV first, lossless FLAC useful for transfer/storage. | An audio file alone is not a complete instrument. Root key, regions, envelope and playback policy need a patch definition. Compressed file size does not determine decoded RAM. |
| [SFZ](https://sfzformat.com/) | Text instrument mapping over external samples: keys, velocity ranges, loops, layers and articulations. Best first interchange candidate for editable multisamples. | Players implement different opcode sets/extensions. Publish a tested subset or use a tested engine; report unsupported behavior rather than silently losing it. Collect referenced samples/includes for transfer. |
| [SoundFont SF2/SF3](https://www.fluidsynth.org/wiki/GettingStarted/) | Packaged banks with instrument definitions and sample content; suitable for General MIDI and existing banks. SF3 uses compressed sample data. | Use an established interpreter such as FluidSynth; do not assume conversion to another engine preserves modulation, envelopes or sound. |
| Hydrogen `.h2drumkit` / `drumkit.xml` | Portable drum-kit packaging and instrument/layer metadata. | Import is a mapping between feature sets, not automatically lossless. Preserve content attribution separately from the player's code license. |
| GIG / DLS / Korg / other established sampler banks | Existing libraries and device interchange. [libgig](https://www.linuxsampler.org/libgig/) documents several supported file families; FluidSynth also documents DLS support. | Add when there is a concrete library/user need and an appropriate reader. A reader is not a guarantee of matching another sampler's synthesis. |
| [DecentSampler](https://decentsampler-developers-guide.readthedocs.io/en/latest/index.html) | Documented preset/library format worth comparing for musician-created libraries. | Public format documentation and a free download do not establish an open-source engine license. No engine reuse or importer selected here. |
| Oscairn Performance JSON + owned assets | Graph, sound assignments, native contexts, plugin state and portability metadata across different kinds of engines. | Keep the [existing container](performance-format.md); SFZ/SF2 describe instruments, not the entire Performance, MIDI graph or setlist. |

An importer must preserve the source and give a clear unsupported-feature result. Sample
content rights are separate from format specifications and engine licenses. Do not export
Yamaha factory samples or infer redistribution permission from an installed library.

## First native sample slice

After selecting the minimal SFZ profile and engine approach in RFC 0008, the next slice
is a **bounded, preloaded WAV patch described by that SFZ profile**, using the existing
native context lifecycle, before a full multisample editor or streaming engine. Reuse libsndfile
for decoding: it is installed here and already used by upstream LV2 integration tests.
It must become an explicit production dependency in desktop CI and Buildroot if selected
for the implementation; a test-only dependency is not sufficient. [dr_wav](https://github.com/mackron/dr_libs)
is a smaller WAV-only alternative, but would add another decoder to maintain.

The complete slice must cover:

1. **Prepare:** validate a mono/stereo WAV and root note outside the audio callback;
   enforce supported encodings, finite decoded data, frame/rate/channel bounds and a
   decoded-memory budget before allocation. Failure leaves the playing sound intact.
2. **Own and share:** immutable decoded samples separate from patch settings. Contexts
   using the same asset share PCM; each voice retains position, envelope and owning
   context. Reclaim only on the control side after selection/voice/tail pins disappear.
   Start with one engine's sharing; do not claim a global cross-process asset service.
3. **Play:** pitched mono/stereo playback, defined sample-end and note-release behavior,
   independent channel sounds and held-note overlap. Reuse current sustain, pressure,
   note-ID, routing and event-timing contracts. Document interpolation quality/limits.
4. **Persist:** samples are dependencies of the Performance, referenced with their
   content identity rather than copied ([RFC 0008](rfcs/0008-sfz-native-samples-and-conversion.md#runtime-resources-and-persistence)).
   A moved Performance reopens identically; missing, changed, malformed or escaping
   samples fail before replacing the live graph.
5. **Expose:** import/prepare/select through the engine/control API and the shared themed
   sound picker; display preparation failures. The desktop UI must not become the only
   place that knows how to prepare sample state.

This reaches the selected native instrument, the native context extension, `core/clap-host`,
`core/plugin-host`, typed/JSON/remote controls, snapshot handling, the desktop sound
picker, schemas/docs, fixtures, dependency configuration and headless/concurrent tests.
Reuse the Basic Synth's proven context ownership without creating competing lifetime
implementations. RFC 0008's engine evaluation determines DSP placement. Keep single-patch
preparation separate from exclusive full-engine restoration.
The preparation ABI (draft 3) carries patch text and an asset directory explicitly.

Acceptance requires zero audio-thread loading/allocation, held-note and two-part reference
renders, stereo separation, exact sample timing, malformed/truncated/oversized inputs,
budget/capacity rejection, safe retirement under concurrency and save/reopen with sample dependencies.
Audible acceptance is queued only after these pass and Jacob is available.

Use these contracts for the initial SFZ engine evaluation, including liquidsfz; evaluate
SoundFont via FluidSynth separately when that format is needed. Do not build a new full
SFZ/SF2 parser or adopt a library solely from a feature list.
Streaming, multisample/velocity mapping, loops, slicing and package-wide asset
deduplication are later increments; none is promised by this first slice.

## Richer open formats and Yamaha conversion (planned)

Jacob's requirement: eventually migrate his MODX M sounds and Performances into Oscairn,
and provide conversion between Oscairn, SFZ and supported Yamaha files, with explicit
warnings for lost or approximated behavior. This is a future tool, not current support.

SFZ already describes many AWM2-like behaviors: release/legato triggers, sequential or
random alternates, key switches, controller conditions, crossfades and modulation. Its
documented dialects and different player subsets require named compatibility profiles;
an arbitrary `.sfz` file is not a promise of identical playback everywhere. The proposed
approach in [RFC 0008](rfcs/0008-sfz-native-samples-and-conversion.md) is to build the native
sampler patch description on SFZ, with documented extensions only for demonstrated gaps.
Keep Oscairn's public Performance container and other engines' state contracts for complete
sounds, routing, controllers and other synthesis types. Do not introduce a parallel sampler
format or force every engine into a sampler-shaped state model.

Other candidates do not eliminate that distinction:

- [Bitwig/PreSonus `.multisample`](https://github.com/bitwig/multisample) is explicitly open,
  but intentionally covers basic sample mapping and excludes complex articulations,
  filters, envelopes and effects. Useful interchange, not a full workstation preset.
- [DecentSampler's documented format](https://decentsampler-developers-guide.readthedocs.io/en/latest/index.html)
  includes effects, modulators, buses and UI descriptions. It is a richer vendor format,
  not evidence of an independently standardized implementation across engines.
- AWM2 names Yamaha's synthesis system, not a neutral file specification or an available
  open-source DSP library. Independent compatible behavior and Yamaha file adapters are
  separate work from adopting Yamaha implementation code. Preserve the no-Yamaha-code
  requirement from the ESP research.

### Existing converter work to evaluate first

- Jürgen Moßgraber's [ConvertWithMoss](https://github.com/git-moss/ConvertWithMoss)
  (LGPL-3.0) is a reuse candidate for SFZ/multisample conversion. Its
  [September 2026 manual](https://www.mossgrabers.de/Software/ConvertWithMoss/ConvertWithMoss-Installers-Manual/ConvertWithMoss-Manual.pdf)
  documents Yamaha support, but MONTAGE M Y2U/Y2L reading is waveform-only; Performance
  reading/writing targets the older MONTAGE/MODX families. Compressed Yamaha samples and
  cross-library sample references are listed limitations. Do not infer MODX M Performance
  conversion from an older MODX checkbox.
- Johan Adolfsson and contributors' [YSFC Forge](https://github.com/YSFCforge/ysfc-forge)
  specifically targets MODX M/ESP Y2U/Y2L and publishes parameter mappings. Its reported
  binary-verification coverage is upstream evidence, not independently verified here.
  Documented gaps include Smart Morph, incomplete Scene mappings and omitted Live Sets/
  Patterns in Library Builder. Review its [file-specific notices](https://github.com/YSFCforge/ysfc-forge/blob/main/NOTICE.md)
  before reuse; the repository includes both MIT project material and third-party notices.
- Chris Webb's [arachsys/montage](https://github.com/arachsys/montage) provides older
  X7/X8 file-analysis work. John Melas' [editors](https://www.jmelas.gr/montagem/) are
  behavioral/interoperability references, not open-source dependencies.

These projects were reviewed at documentation level in this follow-up; no parser was
adopted or run against Jacob's files. Pin revisions, audit the needed reader/writer paths
and credit authors before incorporating code. Yamaha documents the shared MONTAGE M/
MODX M `.Y2U`, `.Y2L`, `.Y2A` family in its
[compatibility guide](https://fr.yamaha.com/fr/musical-instruments/keyboards/explore/montage-portal/essentials/compatibility.html).

### Conversion workflow and fidelity

The canonical [conversion contract](rfcs/0008-sfz-native-samples-and-conversion.md#conversion-and-modx-m-migration)
and [build order](rfcs/0008-sfz-native-samples-and-conversion.md#build-order-and-completion-gates)
are in RFC 0008. Start with a read-only MODX M inventory, preserve source data, report
mapped/approximated/rendered/unavailable items, and validate each supported destination.
Full-library accounting and identical sound reproduction are separate goals. No conversion
implementation is supplied by this research note.
