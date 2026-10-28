# Core

The platform core, built as a library: realtime graph, transport and clock sync, sample-accurate
event routing, plugin hosting, stable parameter addressing, and the library index. It must not
assume it owns the audio device, its threads or the process.

`control/` contains the JUCE-free `Session` and the initial in-process `Control` interface.
The host supplies output buffers and sample-timed events; the session owns graph execution,
transport and the UI note queue. See [control](control/README.md) for the current scope and
thread contract. The library index and the complete wire control API are not implemented.
