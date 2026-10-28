# RFC 0004: Hardware services, event timing and engine backends

**Status:** proposed; architecture direction agreed, interfaces and implementation pending

## Problem

Oscairn must run on CPU-only computers and instruments, and on devices with auxiliary
real-time cores, panel microcontrollers, dedicated DSPs or FPGAs. Engines should use those
resources where a compatible implementation improves measured performance. A saved Sound
must remain independent of the processor that happens to execute it.

There are two distinct needs: predictable physical I/O timing, and audio algorithm
acceleration. A processor may support either, both or neither. Its presence does not mean
its firmware is available, its peripherals are accessible, or arbitrary plugins can run
on it. Board wiring, firmware, memory and driver support determine usable capabilities.

Today the graph processes nodes sequentially on the calling audio thread. The desktop
host uses JUCE audio/MIDI I/O and places incoming notes and pressure at the next block's
start. There is no Oscairn auxiliary-core firmware, accelerator backend, hardware clock
mapping or scheduled external-output implementation. The QEMU image does not validate
any of these on a physical board.

## Decision

### Responsibilities

| Component | Owns |
| --- | --- |
| Plugins | Musical decisions, DSP algorithms, parameters, portable state and explicitly supported execution backends |
| Host resource manager | Capability matching, admission, reservations, preparation, backend selection and recovery |
| Hardware services | Device discovery, clock mapping, audio and event I/O, peripheral ownership, firmware lifecycle and mediated accelerator access |
| Board adapter and auxiliary firmware | Actual drivers, timers, DMA, peripheral scheduling and device-specific data transfer |
| UI / control API | Configuration, user policy and status; outside the played-event and audio paths |

Keep the core a library without JUCE or board drivers. The host supplies the services; a
desktop, Linux instrument, DAW plugin and deterministic test host can supply different
implementations. Device services may run in a separate privileged process, but their
audio-facing interface must have bounded execution and explicit transfer latency.

The full host still requires a suitable application CPU and operating environment. An
engine compiled into an MCU-only or DSP-only product is a separate reduced port, not proof
that the complete host, dynamic plugin ecosystem and UI can run there.

### Discover capabilities, not just processors

Expose versioned capability records for:

- **Compute:** backend family and ABI, supported engine kernels/firmware, sample rates,
  block constraints, numeric formats, local/shared memory and transfer requirements.
- **Timing:** input timestamp resolution, output scheduling support, clock domains,
  queue capacity, minimum scheduling lead time, cancellation support and measured jitter.
- **I/O:** audio channels, MIDI ports, CV ranges/calibration, gate/clock endpoints, panel
  controls and which service owns each peripheral.
- **Availability:** resource capacity, reservations, firmware/driver versions and health.

Capabilities distinguish documented limits from measured performance and unknown values.
An unavailable firmware image or inaccessible peripheral is reported as unavailable, even
if the silicon could support it. Timing firmware and audio kernels reserve shared CPU,
memory, DMA and bus resources together so audio work cannot starve scheduled I/O.

An auxiliary core running bare-metal or RTOS firmware is different from a Linux CPU core
reserved for audio. CPU affinity, realtime scheduling and parallel graph processing are
host policies; they do not create a DSP backend or allow arbitrary code on an MCU.

### Engines provide compatible backends

Every official portable engine provides a CPU backend. It may additionally supply DSP,
FPGA or real-time-core implementations of the entire engine or selected algorithms. Its
host-facing plugin remains CLAP (or another supported format); acceleration is beneath
that interface. Existing plugins without this capability continue on the CPU.

A backend declares its identity/version, required service ABI and kernel/firmware,
supported sample rates and block sizes, quality modes, state schema, voice/resource
limits, processing/transfer latency and whether it can render offline. The plugin owns
algorithm translation; the host does not compile arbitrary CPU plugin code for a DSP.

Backends for the same portable engine use the same musical parameters, engine identity
and portable state. Device handles, physical addresses and FPGA registers are runtime
details, never saved patch requirements. Backends must pass sound/state compatibility
tests with defined tolerances; they need not produce identical floating-point bits.
An audibly different algorithm is a distinct saved mode or engine, not a transparent
substitute. Hardware-specific third-party engines can declare mandatory hardware; the
host preserves their state and reports missing requirements on incompatible devices.

### Select and reserve before activation

1. Match the Sound's engine, quality, resource and latency requirements to supported
   backends and available hardware.
2. Estimate the complete cost, including preparation, transfers, buffering, other active
   engines and transition overlap. Report uncertain estimates rather than promising fit.
3. Apply device/session policy: automatic compatible selection, CPU preference, or a
   specific backend for diagnostics. Automatic selection uses measured profiles and
   preserves the requested sound and quality; the choice is visible to the user.
4. Reserve resources, load approved firmware/kernels, allocate buffers, prepare state and
   validate readiness outside the audio callback. Failed preparation releases reservations
   and leaves the currently playing graph intact.
5. Activate at an agreed audio boundary. Keep the selection stable while playing unless
   an explicit, prepared transition supports a change.

CPU fallback is allowed only when the CPU can sustain the requested configuration. If no
backend fits, report the reason and offer an explicit change in quality or polyphony, or
refuse activation. Do not silently reduce either. Device policy is separate from portable
Sound data; Sounds save musical requirements and explicit quality choices.

### Realtime execution and failure

Preallocate buffers and use bounded queues. No firmware loading, device discovery,
allocation or unbounded driver/IPC waits occur in the audio callback. A hardware backend
must state whether results are available in the current processing interval or require a
fixed pipeline delay. Count DMA, cache maintenance, copies, IPC and buffering in the
deadline and latency budget. A fast DSP kernel alone is not evidence of a faster path.

Any added delay must be reported and aligned with other graph paths. Audio, notes and
parameter changes keep their intended relationship through the pipeline; same-sample
parameter locks must remain attached to the samples they affect. Reject unsupported
latency arrangements until the graph can compensate them; it has no complete latency
compensation implementation today.

On a missed deadline, ignore late results and use a defined mute/bypass policy. Recovery
runs outside audio processing. Switching to CPU without interruption requires a prepared
CPU context, sufficient reserved capacity and a compatible state/DSP-history transition;
otherwise report the interruption. Never promise that a parameter-state reload recreates
held voices or effect tails. A shared accelerator can fail several nodes together; track
that failure domain and avoid resetting hardware still serving healthy nodes.

Plugins use mediated handles and validated buffers, not unrestricted registers or device
memory. Resource leases, firmware ownership and revocation belong to the host. Loading a
kernel or FPGA image must not overwrite another engine's resources. Define supported
coexistence and reconfiguration boundaries before admitting such a backend.

### One event timeline, explicit device clocks

Use an absolute audio sample counter and a timeline epoch for event scheduling. The epoch
changes when the device/session timeline is reset, such as an audio-device restart.
Musical beat position and loop/seek position are separate from this monotonic timeline.
Each hardware clock is mapped to it using offset and rate estimates, with drift and
uncertainty reported. Do not assume two nominally identical clock rates stay aligned.

Inputs carry source identity, sequence information and their capture timestamp, not just
the time Linux received the packet. Preserve the timestamp to graph ingress, translating
it into a block offset under an explicit latency policy. Late events cannot change audio
already played: apply or discard them according to event type and report lateness.
Where a device supplies no capture timestamp, record the earliest available receive time
and identify its origin and uncertainty; never present it as a hardware capture time.

Outputs carry endpoint, payload, target presentation time, epoch, cancellation generation
and deterministic ordering for equal deadlines. Endpoint semantics specify what the
deadline means: a gate edge, calibrated CV validity, or a MIDI byte/message boundary.
Account for MIDI serialisation, DAC settling and device buffering when relating external
events to audio. Do not promise simultaneous MIDI messages on one serial wire.

### Scheduling lead time and musical changes

A real-time processor executes events queued ahead of their deadlines. Sending an untimed
message through Linux/RPMsg at the desired instant does not eliminate Linux scheduling
jitter. The service advertises how much lead time it needs and how far ahead it can queue.

Ordinary plugins emit events for the block currently being processed, not arbitrary future
blocks. Initially use a bounded presentation delay, budgeted with audio output buffering,
so current-block events can reach hardware before their deadlines. Align internal audio
and external outputs against that presentation timeline. Report the resulting end-to-end
latency and reject a configuration that cannot meet its requested timing budget.
Do not run a stateful sequencer twice to invent lookahead. A future optional prediction
interface would need explicit state and edit semantics; it is not part of the first version.

Live input uses the same clock mapping but cannot be predicted. Measure its input-to-audio
latency separately from scheduled sequence jitter. Keep queues only as deep as justified
by measurements, since longer lookahead makes edits harder to apply promptly.

Stop, seek, tempo changes and pattern edits invalidate affected queued events using a
cancellation generation. The device reports the boundary after which cancellation can
take effect; already transmitted events cannot be recalled. Keep note ownership so
cancelled future note-offs are replaced where needed, without cutting unrelated live notes.
Reschedule from a known boundary, applying clock/transport changes consistently to audio
and external outputs. This protocol must not wait for an acknowledgement on the audio
thread.

Define queue overflow, stale-epoch, disconnect and watchdog behaviour. Reserve a path for
stop and note/gate release; report dropped or late events and queue occupancy. Loss of the
host link must not leave gates high or notes indefinitely held. An auxiliary processor can
execute only its queued work during a host stall; continuing a complete performance after
a host failure is a separate feature.

### Firmware, portability and offline use

Provide CPU-only and simulated implementations of the same contracts. Their capabilities
may advertise weaker timing guarantees; do not claim hardware-timer precision for an
unmeasured desktop MIDI backend. Sequencer and Sound definitions do not change by device.

Packages can include per-target backend binaries and required firmware/kernel identities,
along with licences and compatibility versions. A CPU build is not binary-compatible with
a DSP, nor does one vendor's DSP binary imply support for another. Board services control
firmware loading, peripheral assignment and restart, with an explicit recovery route.

Offline rendering uses the engine's CPU reference backend by default, with repeatability
for a fixed configuration. Live accelerated output must meet documented equivalence
tolerances. Hardware-only engines must declare whether offline rendering is supported;
report limitations instead of silently substituting another sound. A DAW host's audio
clock and callback constraints remain authoritative.

## Implementation sequence and validation

1. Define capability, clock and event contracts in the core; supply CPU-only and fake-clock
   services. Test timestamp conversion, drift, late input, equal-time ordering, cancellation,
   overflow, reconnects and safe releases deterministically.
2. Route desktop I/O through those contracts, preserving existing operation. Measure live
   latency and output scheduling; keep the control API outside this event path.
3. Add one board's timing backend after confirming wiring, peripheral ownership and
   firmware availability. Test MIDI/CV/audio alignment and worst-case jitter under CPU,
   storage, UI and bus load with physical loopback or measurement equipment. QEMU alone
   cannot verify this.
4. Implement backend discovery, reservations and one reference engine with a CPU backend
   and one available accelerator. Test state round trips, reference audio tolerances,
   parameter timing, buffer sizes, resource contention and refusal when no backend fits.
5. Exercise accelerator timeout/disconnect, firmware restart and CPU recovery with and
   without prepared capacity; measure complete latency and sustained deadline margin.
   Expand only after a backend shows a useful measured benefit.

## Open interface details

- Exact versioned C extension/ABI for plugin backend negotiation and service handles;
  keep format adapters separate and use standard plugin features where sufficient.
- Clock estimation method, timestamp representation and endpoint-specific timing budgets.
- Per-board queue/cancellation protocol, minimum presentation delay and recovery behaviour.
- Accelerator buffer ownership, sandbox transport and firmware/kernel packaging format.

These need prototypes and measurements before an ABI is frozen. None requires moving
sequencer decisions into board firmware or making the UI responsible for timing.

## Hardware evidence

- [NXP i.MX 8M Mini specifications](https://docs.nxp.com/bundle/IMX8MMCEC/page/topics/imx_8m_mini_introduction.html):
  application Cortex-A53 cores and a Cortex-M4; separate execution domains.
- [NXP heterogeneous multicore framework](https://github.com/nxp-real-time-edge-sw/heterogeneous-multicore):
  Linux/RTOS communication, resource sharing and lifecycle infrastructure. This supplies
  transport mechanisms, not Oscairn's musical scheduling contract.
- [Ignite Linux documentation](https://github.com/MusicTribeOpenSource/0716-AAF/blob/main/Linux/README.md):
  i.MX8MM board support and an MCU firmware image omitted from the release. Verify the
  actual I/O owner before assigning timing work to the on-chip M4.

Sources checked 2026-10-28. This RFC defines the software design; it does not claim that
any particular instrument has working Oscairn acceleration or scheduled hardware I/O.
