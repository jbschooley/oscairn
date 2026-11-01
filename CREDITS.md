# Credits and source provenance

Oscairn builds on work by the open audio software community. Dependencies and
reference implementations are distinguished here from source copied into this
repository. Existing copyright and license notices in individual files remain
authoritative.

## Plugin infrastructure

- **CLAP:** the CLAP contributors. The vendored API headers retain their
  [MIT license](core/clap-host/third_party/clap/LICENSE) and
  [pinned provenance](core/clap-host/third_party/clap/upstream.json).
- **Lilv and LV2:** David Robillard and the LV2 contributors. The optional LV2
  adapter links to Lilv and uses the LV2 API/Atom helpers. These are external build
  dependencies, not newly vendored source. See [Lilv](https://gitlab.com/lv2/lilv)
  and [LV2](https://lv2plug.in/); retain the notices supplied with the versions
  distributed in any binary/image. The independent eg-amp and eg-fifths test
  plugins are upstream LV2 examples, installed separately.

## Signal processing

- **Split's crossovers** ([plugins/splitters](plugins/splitters/splitters.cpp)): second-order
  sections from Robert Bristow-Johnson's *Audio EQ Cookbook* (the published formulas; no
  source copied), arranged as Linkwitz-Riley crossovers (Siegfried Linkwitz and Russ Riley)
  with allpass compensation so the bands sum flat.

## Instrument engines

- **Modular:** chillbeast ([chillbeast/0700-AAM](https://github.com/chillbeast/0700-AAM)),
  AGPL-3.0-or-later. The engine, module library and patch code under
  [plugins/modular/upstream](plugins/modular/upstream) are imported unchanged with their
  history from his KICK firmware fork; see [its README](plugins/modular/README.md).
- **Pad layouts:** the in-key pad fills (rows continuing, up a fourth or up a third, in key or
  chromatic) in [core/control/src/pads.cpp](core/control/src/pads.cpp) follow chillbeast's
  `PadLayout` in the same fork (`kick_pad_layout.h`, AGPL-3.0-or-later). The per-pad note,
  channel, velocity and pressure settings follow the stock KICK firmware's MIDI Control mode.

## Interface font

- **Inter** 4.1, by Rasmus Andersson and the Inter Project Authors
  ([rsms/inter](https://github.com/rsms/inter)), SIL Open Font License 1.1. The built-in
  themes and the Midnight example carry `Inter-Regular.ttf` and `Inter-Bold.ttf` unmodified
  from the release's `extras/ttf/`, with the license as `fonts/OFL.txt`. Release archive
  `Inter-4.1.zip` SHA-256 `9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e`;
  `Inter-Regular.ttf` `40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82`,
  `Inter-Bold.ttf` `288316099b1e0a47a4716d159098005eef7c0066921f34e3200393dbdb01947f`.

## Interface icons

- **Lucide** 0.460.0, by the Lucide Contributors, with portions by Cole Bemis as part of
  Feather (MIT) ([lucide-icons/lucide](https://github.com/lucide-icons/lucide)), ISC license.
  [hosts/standalone/icons](hosts/standalone/icons) carries these SVGs unmodified from the
  `lucide-static@0.460.0` package's `icons/` (fetched from unpkg), with its license as
  `LICENSE` (SHA-256 `1e7290b35280a048667bbf0ebabac1c7fd52a75300e8b2946ac165715997f2bc`).
  The app recolors them as it draws them (`Icons.h`); the files are not changed. SHA-256:
  `check.svg` `03c8e362a064c4a094016be85e63289825cf3ecbccb73714138df2b7df2d5026`,
  `chevron-down.svg` `c1032432d9942c344c9f80008b3028458c07580c1f2895830782f6b720075f53`,
  `chevron-right.svg` `856bf29117b586fa854be23f4c6cc9249557a1a5d7cadcbc466f9fdfd7dc699f`,
  `list.svg` `5235475b1e4cb7eee653c946c3a81c224b751d6f1175bdfccf67f2a0fa29f03a`,
  `plus.svg` `bf6084d1cb578acf5b8fa48831ecba8ae8f860957b9289ba81ab562f22604df6`,
  `redo-2.svg` `512e9f81b19a3e739b240a413d61f798dbda02e788b5a81db2926543713ebbf7`,
  `square-arrow-out-up-right.svg` `968a03c02ab9ffbb4e92de4c5d2e75c7140e8c63752a63258527e764ff214998`,
  `star.svg` `ebc8483f1006df4cc16551ccbefc511e3cca2e17c7f3a727842e422fdac7a9b0`,
  `tags.svg` `724af60488ee052f78f8b3a5622adb06e8c30abea41c9c48a718f808a7bcdc4a`,
  `undo-2.svg` `63030cafa632c7cce406680b920d286696f136b3ea58218bdb21b6897a31eea9`.

## Sample decoding

- **libsndfile:** Erik de Castro Lopo and the libsndfile contributors
  ([project](https://libsndfile.github.io/libsndfile/), LGPL-2.1-or-later). The Oscairn
  Sampler links it dynamically to decode WAV samples; it is an external build dependency,
  not vendored source. Retain its notices in any distributed binary/image. The Sampler
  itself (AGPL-3.0-or-later) is in its own repository,
  [oscairn/sampler](https://github.com/oscairn/sampler), which Oscairn builds at a pinned
  commit.

## Archives

- **miniz:** Rich Geldreich, RAD Game Tools and Valve Software
  ([richgel999/miniz](https://github.com/richgel999/miniz), MIT). The unmodified 3.0.2
  `miniz.c` and `miniz.h` are vendored to read and write library and export archives
  (RFC 0021), with their [license](core/control/third_party/miniz/LICENSE) and
  [pinned provenance](core/control/third_party/miniz/upstream.json).

## LV2 host reference work

The [LV2 audit](core/lv2-host/AUDIT.md) records pinned revisions, source files and
specific findings. No implementation chunks from these hosts were imported by
that audit.

| Project | Credit | Notices in the inspected sources |
| --- | --- | --- |
| Jalv | David Robillard and contributors | Mapper, symbol map, event buffer, Worker and state sources carry ISC notices. |
| MOD mod-host | MOD Audio and mod-host contributors; David Robillard for the Worker/event-buffer lineage | `effects.c` and `uridmap.c` carry GPL-3.0-or-later notices; `worker.c` retains David Robillard's ISC notice. Licensing is file-specific. |
| Carla | Filipe Coelho and contributors | `CarlaPluginLV2.cpp` and the inspected utility sources carry GPL-2.0-or-later notices. |
| Ardour | Carl Hetherington, Paul Davis, David Robillard, Robin Gareus, John Emmas, Michael R. Fisher, Tim Mayberry, Damien Zammit, Nick Mainsbridge, Johannes Mueller and contributors | Names from the inspected `lv2_plugin.cc` header; it and `worker.cc` carry GPL-2.0-or-later notices. |

## Adapted Worker implementation

Following that audit, [Worker.cpp](core/lv2-host/Worker.cpp) and
[Worker.h](core/lv2-host/Worker.h) adapt **David Robillard's Jalv Worker design** into
Oscairn's C++ host. They retain ISC notices, with the full
[license](core/lv2-host/third_party/jalv/ISC.txt) and
[source revision/path/modification record](core/lv2-host/third_party/jalv/upstream.json).
The adaptation replaces Zix infrastructure, bounds packet/response work, and changes
offline and shutdown handling. It is not an unchanged vendored copy. LV2-enabled CMake
installs include the ISC notice; the Buildroot package records it in license metadata.

The independent Worker test builds the unmodified **LV2 eg-sampler** from source
revision `0bcde338db1c63bbc503b4d1f6d7b55ed43154af` (v1.18.10). Its source credits
David Robillard, Gabriel M. Beddingfield and James Morris under ISC. The checkout
remains external and retains its own notices; it is a test dependency, not shipped
Oscairn plugin code.

## When importing source

For copied or adapted upstream code, retain the original authors' copyright and
license notices in the files, include the applicable license text, and record the
repository URL, exact revision, upstream paths, local paths and modifications in
this file or a linked provenance manifest. Identify copied/adapted work separately
from reference-only work. Check the actual files' licenses and compatibility
before import; a project-wide license label is not a substitute for those notices.

These credits do not relicense upstream code. Oscairn's own license remains
[AGPL-3.0-or-later](LICENSE).
