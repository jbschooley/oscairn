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

## When importing source

For copied or adapted upstream code, retain the original authors' copyright and
license notices in the files, include the applicable license text, and record the
repository URL, exact revision, upstream paths, local paths and modifications in
this file or a linked provenance manifest. Identify copied/adapted work separately
from reference-only work. Check the actual files' licenses and compatibility
before import; a project-wide license label is not a substitute for those notices.

These credits do not relicense upstream code. Oscairn's own license remains
[AGPL-3.0-or-later](LICENSE).
