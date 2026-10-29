# Yamaha ESP effect-memory observations

Read-only inspection on 2026-10-29 of the locally installed Expanded Softsynth Plugin
for MONTAGE M, version **3.0.1**, build **427**. This is evidence about the desktop
plugin, not a verified description of MONTAGE/MODX hardware memory or an Oscairn backend.
No Yamaha implementation code is incorporated into Oscairn.

## Reproducibility

Inspected the arm64 slice of
`/Library/Audio/Plug-Ins/VST3/Yamaha/Expanded Softsynth Plugin for MONTAGE M.vst3/Contents/MacOS/Expanded Softsynth Plugin for MONTAGE M`.
The universal executable is 136,163,984 bytes; its SHA-256 is
`2d54e3d3617ff74a7421436b7a8374938f6922ed6d2810f7051af7b0e011ffb4`.
Used `nm -arch arm64` with `c++filt`, and Xcode's `llvm-objdump` arm64 disassembly.
Addresses below are unslid virtual addresses in that slice, not runtime pointers.
Only observations and symbol references are retained here, not binary/disassembly copies.

## Direct observations

Unless otherwise qualified, classes below belong to
`yamaha::dmi::de::clone_22maisy::tg::effect`.

| Area | Evidence | What it establishes |
| --- | --- | --- |
| Central infrastructure | `Manager::Impl::Impl()` at `0x7ff3a8` constructs a firmware-interface wrapper, `VsyInOuts`, a `Buffer` and a `Mixer`. | A manager coordinates common effect routing/infrastructure. This alone does not establish sharing across plugin instances. |
| Routing buffer | `Manager::Impl::reallocBuffer()` at `0x7ff9a8` gets valid input connections, builds their channel list, and passes it and the maximum sample count to `Manager::Buffer::realloc()` at `0x7fec24`. | Buffer sizing follows the current connection list and block capacity. |
| Concrete buffer sizes | `Buffer::realloc()` allocates and zeroes a contiguous `4 * N * F` byte sample array, an `8 * N` byte pointer array, and an `N` byte flag array. It sets channel pointers at `4 * F` byte strides. Here `N` is the supplied channel-list length and `F` the sample capacity. | Planar audio storage is managed centrally. This is block audio storage, not evidence that independent delay/reverb histories share the same samples. These figures exclude metadata and allocator overhead. |
| Per-block allocations | `Block::Impl::Impl()` at `0x7f5e34` receives `BlockArraySizes`, allocates several arrays, and stores their pointers in that block. For example, `0x7f6470–0x7f6510` allocates two separate arrays of four-byte elements from supplied counts. | Individual blocks own additional storage; a central manager does not make each additional block free. Field meanings and actual per-algorithm counts have not all been established. |
| Per-block reset and algorithm replacement | `Block::Impl::changeType()` at `0x7f729c` clears block-owned arrays and resets its LFO/random state. At `0x7f7528` it calls `yamaha::dmi::emu::swp70::Interface::genInstance(...)` and replaces the algorithm object held by that block. | There is block-local mutable state and an algorithm instance, even inside the common effect system. This path does not establish seamless preservation of an old effect tail. |
| Hardware-related vocabulary | Symbols include `Up::Swp70::DspResourceAllocator`, `WriteRegisterDspExternalMemory`, `FinalizeDspResourceAllocation`, and SDRAM/SRAM capacity constants. Software effect code calls into `yamaha::dmi::emu::swp70`. | The desktop implementation contains SWP70-related software machinery. Symbol presence is not proof that every hardware-oriented path executes in ESP or that an accessible hardware driver exists. |

Symbol names alone can mislead: `Block::getNumWork()` at `0x7f7cd0` simply returns
zero in this build despite the allocations above. Generic `EvtSrv::MemoryPool` symbols
were not treated as evidence of audio-history sharing.

## Implications for Oscairn

These findings support the separation in [RFC 0007](rfcs/0007-effect-contexts.md):
common engine infrastructure and routing storage, with separately owned mutable effect
state. They do **not** demonstrate a particular RAM saving, shared impulse-response
storage, or shared independent delay lines. Keep shared/per-context memory accounting
explicit and measure our implementation against separate reference instances.

Scratch buffers can be reused only when their lifetimes do not overlap. Persistent
histories must survive across blocks and outgoing tails, regardless of common ownership.
No architectural change is justified by the inspected ESP code; it strengthens the reason
to preserve this distinction and budget for each independent context.

## Not established

No runtime heap/CPU measurements, one-versus-many-Part comparison, full call-graph audit,
or hardware inspection was performed. We have not established exact effect-slot limits,
eager versus lazy construction of all slots, which tables/assets are shared, or exact
per-effect history sizes. A future runtime comparison must separate fixed reservation,
committed/resident pages, sample-library loading and GUI/host overhead from effect state.
