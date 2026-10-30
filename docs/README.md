# Oscairn documentation

Start with [Developing for Oscairn](developer-guide.md) to build a plugin, controller,
host or hardware integration. It describes the current contracts and runnable examples.
This is an early project: APIs marked `draft` can change, and a design proposal is not
an implemented capability.

| Task | Documentation |
| --- | --- |
| Build and run Oscairn | [Project README](../README.md) |
| Write an instrument, effect or sequencer | [Developer guide](developer-guide.md), [reference plugins](../plugins/README.md) |
| Use the C++ graph or embed the engine | [Core](../core/README.md), [Session and Control](../core/control/README.md) |
| Write a UI, editor, script or controller | [Control protocol](control-protocol.md), [JSON Schema](control-protocol.schema.json) |
| Load CLAP plugins and measure them | [CLAP host](../core/clap-host/README.md) |
| Port or load an LV2 plugin | [LV2 support and limits](../core/lv2-host/README.md) |
| Preserve held notes across sound changes | [Native context contract](rfcs/0005-sound-contexts.md), [prepared-sound control API](control-protocol.md#prepared-sounds) |
| Write a MIDI effect or route audio-generated notes | [MIDI references](../plugins/midi-effects/README.md), [hybrid routing and feedback](architecture.md#midi-effects-hybrid-plugins-and-feedback) |
| Export plugin state or edit sample properties | [Sound-management controls](control-protocol.md#state-export-presets-and-patch-properties) |
| Choose or build the main sequencer | [Sequencer evaluation](sequencer-evaluation.md) |
| Port chillbeast's Modular instrument | [Modular engine evaluation](modular-engine-evaluation.md) |
| Design parts, routing, mixer, plugin editors or control assignments | [Performance UI RFC](rfcs/0011-performance-ui-and-editors.md) (proposed) |
| Build native sample support or choose a sample format | [SFZ-based native samples and conversion plan](rfcs/0008-sfz-native-samples-and-conversion.md), [player/format research](sample-players-and-formats.md), [ESP observations](yamaha-esp-samples-research.md) (first one-region slice implemented; multisample planned) |
| Save, reopen, import or export a Performance | [Format and behavior](performance-format.md), [JSON Schema](performance.schema.json) |
| Emit parameter-lock lanes from a sequencer | [Parameter-target extension](rfcs/0002-param-target.md) |
| Add a desktop panel layout | [Layout format](../hosts/standalone/layouts/README.md) |
| Configure audio, MIDI and helper processes | [Hosts](../hosts/README.md) |
| Build the Linux image | [OS instructions](../os/README.md) |
| Use native plugins in other DAWs or standalone | [Planned delivery contract](rfcs/0006-packages-and-portability.md#native-plugins-in-daws-and-standalone-applications) |
| Plan installer-free transfers and shared plugin dependencies | [Packages and cross-device portability](rfcs/0006-packages-and-portability.md) (proposed) |
| Understand hardware acceleration and timestamped I/O plans | [Hardware-services proposal](rfcs/0004-hardware-services.md) |
| Understand the overall design and priorities | [Architecture](architecture.md), [roadmap](roadmap.md), [RFC index](rfcs/README.md) |

When changing a public behavior, update its owning document, schema if applicable, and
an executable test in the same change. Keep implemented behavior here and proposals in
the RFCs; avoid instructions that depend on a chat transcript or an unmerged branch.
