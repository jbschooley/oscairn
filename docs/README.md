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
| Emit parameter-lock lanes from a sequencer | [Parameter-target extension](rfcs/0002-param-target.md) |
| Add a desktop panel layout | [Layout format](../hosts/standalone/layouts/README.md) |
| Configure audio, MIDI and helper processes | [Hosts](../hosts/README.md) |
| Build the Linux image | [OS instructions](../os/README.md) |
| Understand hardware acceleration and timestamped I/O plans | [Hardware-services proposal](rfcs/0004-hardware-services.md) |
| Understand the overall design and priorities | [Architecture](architecture.md), [roadmap](roadmap.md), [RFC index](rfcs/README.md) |

When changing a public behavior, update its owning document, schema if applicable, and
an executable test in the same change. Keep implemented behavior here and proposals in
the RFCs; avoid instructions that depend on a chat transcript or an unmerged branch.
