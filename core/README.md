# Core

The platform core, built as a library: realtime graph, transport and clock sync, sample-accurate
event routing, plugin hosting, stable parameter addressing, prepared instrument instances, and native sound-context controls. It must not
assume it owns the audio device, its threads or the process.

`control/` contains the JUCE-free `Session` and the typed `Control` interface, with optional JSON-RPC and Unix socket adapters.
The host supplies output buffers and sample-timed events; the session owns graph execution,
transport and the UI note queue. See [control](control/README.md) for the current scope and
thread contract. The library index and the complete wire control API are not implemented.

`plugin-host/` is the graph's shared plugin-host interface. The CLAP adapter delegates to
`clap-host/`; optional `lv2-host/` uses Lilv. Event and metadata structures retain the
existing CLAP-shaped representation internally, translated at the LV2 boundary. See
[LV2 hosting](lv2-host/README.md) for the supported subset and build option.

`Graph::add` wraps eligible ordinary instruments in an eight-instance sound pool;
`loadPlugin` returns the raw format adapter. Native-context plugins bypass the pool.
The [developer guide](../docs/developer-guide.md) explains the distinction and links
plugin, host, controller and hardware integration contracts.
