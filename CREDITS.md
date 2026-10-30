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

## Instrument engines

- **Modular:** chillbeast ([chillbeast/0700-AAM](https://github.com/chillbeast/0700-AAM)),
  AGPL-3.0-or-later. The engine, module library and patch code under
  [plugins/modular/upstream](plugins/modular/upstream) are imported unchanged with their
  history from his KICK firmware fork; see [its README](plugins/modular/README.md).

## Interface font

- **Inter** 4.1, by Rasmus Andersson and the Inter Project Authors
  ([rsms/inter](https://github.com/rsms/inter)), SIL Open Font License 1.1. The built-in
  themes and the Midnight example carry `Inter-Regular.ttf` and `Inter-Bold.ttf` unmodified
  from the release's `extras/ttf/`, with the licence as `fonts/OFL.txt`. Release archive
  `Inter-4.1.zip` SHA-256 `9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e`;
  `Inter-Regular.ttf` `40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82`,
  `Inter-Bold.ttf` `288316099b1e0a47a4716d159098005eef7c0066921f34e3200393dbdb01947f`.

## Sample decoding

- **libsndfile:** Erik de Castro Lopo and the libsndfile contributors
  ([project](https://libsndfile.github.io/libsndfile/), LGPL-2.1-or-later). The Oscairn
  Sampler links it dynamically to decode WAV samples; it is an external build dependency,
  not vendored source. Retain its notices in any distributed binary/image.

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
