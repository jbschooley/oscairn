# Sample players, formats and the first native sample milestone

Research and implementation plan, 2026-10-29. **Native sample patches are not implemented
yet.** This records the prior-art pass and a headless reuse experiment before changing
Oscairn's plugin contract. It is not a claim of full format compatibility or a production
engine selection. [ESP observations](yamaha-esp-samples-research.md) are separate.

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

The next implementation is a **bounded, preloaded WAV patch** using the existing native
context lifecycle, before a full multisample editor or streaming engine. Reuse libsndfile
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
4. **Persist:** owned asset files plus relative references in the Performance snapshot;
   deduplicate repeated references within the engine's saved state. Delete the original
   WAV, move the Performance and prove identical playback after reopening. Missing,
   malformed or escaping asset paths must fail before replacing the live graph.
5. **Expose:** import/prepare/select through the engine/control API and the shared themed
   sound picker; display preparation failures. The desktop UI must not become the only
   place that knows how to prepare sample state.

This reaches `plugins/basic-synth`, the native context extension, `core/clap-host`,
`core/plugin-host`, typed/JSON/remote controls, snapshot handling, the desktop sound
picker, schemas/docs, fixtures, dependency configuration and headless/concurrent tests.
Reuse the Basic Synth's proven context ownership rather than duplicating it in a new
plugin. Keep single-patch preparation separate from exclusive full-engine restoration.
The current numeric-only preparation ABI needs an explicit asset-bearing input; it cannot
be implemented by treating a filename as a numeric parameter. Assess CLAP's vendored
resource-directory draft for collection/restoration before inventing a parallel asset API.

Acceptance requires zero audio-thread loading/allocation, held-note and two-part reference
renders, stereo separation, exact sample timing, malformed/truncated/oversized inputs,
budget/capacity rejection, safe retirement under concurrency and portable save/reopen.
Audible acceptance is queued only after these pass and Jacob is available.

Then evaluate SFZ via liquidsfz and SoundFont via FluidSynth against those same contracts.
Do not build a new full SFZ/SF2 parser or adopt a library solely from a feature list.
Streaming, multisample/velocity mapping, loops, slicing, resampling and package-wide asset
deduplication are later increments; none is promised by this first slice.
