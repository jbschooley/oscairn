# RFCs

Each design decision gets a short document: what was decided, why, and what it rules out.
Number them in order (`0002-...md`) and keep the status line up to date
(proposed, in progress, accepted, replaced). Each RFC has a **Progress** checklist after its
status: one box per build step or deliverable, checked in the same change that builds and
tests it, with a note on what remains of a step that is partly done. The
[roadmap](../roadmap.md) keeps the same boxes at a coarser grain.

| RFC | Title | Status |
| --- | --- | --- |
| [0001](0001-license.md) | License | accepted |
| [0002](0002-param-target.md) | Parameter changes between plugins (oscairn.param-target) | draft, implemented |
| [0003](0003-control-api.md) | Control API | partially implemented; local JSON-RPC transport and explicit engine/UI separation |
| [0004](0004-hardware-services.md) | Hardware services, event timing and engine backends | proposed |
| [0005](0005-sound-contexts.md) | Native prepared sound contexts | instrument contexts and full snapshots implemented; SFZ patches via the Sampler; effects covered by 0007 |
| [0006](0006-packages-and-portability.md) | Packages and cross-device portability | proposed |
| [0007](0007-effect-contexts.md) | Independent effects on shared native engines | implemented Delay/bus foundation; automatic chain management planned |
| [0008](0008-sfz-native-samples-and-conversion.md) | SFZ-based native samples and Yamaha conversion | one-region Sampler implemented; multisample, extensions and converters planned |
| [0009](0009-sound-assets-and-dependencies.md) | Sound assets and dependencies | steps 1–2 implemented; LV2 mapping and packages planned |
| [0010](0010-plugin-worker-isolation.md) | Plugin worker isolation | in progress: steps 1–5 implemented on macOS |
| [0011](0011-performance-ui-and-editors.md) | Performance UI, routing views and plugin editors | in progress: steps 1–4 and undo implemented |
| [0012](0012-sync-external-midi-and-multi-device.md) | Sync, external MIDI devices and multi-device setlists | in progress: input device selectors, MIDI out and clock out implemented |
| [0013](0013-awm2-class-sample-engine.md) | An AWM2-class sample engine | proposed |
| [0014](0014-fm-engine.md) | An 8-operator FM engine | proposed |
| [0015](0015-analog-modeling-engine.md) | An analog-modeling engine | proposed |
| [0016](0016-effects-suite.md) | An effects suite covering the MODX M platform | proposed |
| [0017](0017-host-tuning.md) | Host-level tuning | proposed |
| [0018](0018-voice-budget-and-stealing.md) | Voice budget and stealing | in progress: steps 1 to 3 implemented |
| [0019](0019-adaptive-ui.md) | One UI across desktop, touch panel and small screens | in progress: step 1 done, steps 2, 3 and 5 started |
| [0020](0020-recording.md) | Recording | proposed |
| [0021](0021-sounds-performances-and-library.md) | Sounds, Performances and the library | in progress |
| [0022](0022-apps-and-requests.md) | Apps that open each other | in progress |
| [0023](0023-item-types.md) | Item types | proposed |
| [0024](0024-device-profiles.md) | Device profiles | in progress: step 1 built, steps 2 and 3 started |
| [0025](0025-resource-budgets.md) | Resource budgets: knowing before loading whether it fits | in progress: steps 1–4 built |
| [0026](0026-network-control-and-web-ui.md) | Network control and the web UI | proposed |
| [0027](0027-reliability-levels.md) | Reliability levels and the guaranteed audio path | proposed |
| [0028](0028-system-image-updates-boot-and-power.md) | The system image: updates, boot, storage and power | proposed |
| [0029](0029-internationalization.md) | Internationalization | proposed |
| [0030](0030-system-shell-launcher-and-navigation.md) | The system shell: launchers, the top bar and how screens flow | proposed; steps 1 and 2 started (Home, `home` and `back` buttons) |
| [0031](0031-package-repository-and-distribution.md) | The package repository and distribution | proposed |
| [0032](0032-arpeggiator.md) | The arpeggiator | proposed |
| [0033](0033-script-block.md) | The script block | proposed |
| [0034](0034-parallel-graph-rendering.md) | Parallel graph rendering | proposed |
