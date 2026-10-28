# RFC 0002: Parameter changes between plugins (oscairn.param-target)

**Status:** draft, implemented

## Problem

A sequencer's per-step parameter locks, and automation lanes in general, need to change
parameters of *other* plugins: the synth's cutoff on step 3, the delay's feedback on step 9.
CLAP has no way to do this. A plugin's output `CLAP_EVENT_PARAM_VALUE` names only its own
parameters (`param_id` and `cookie`, no plugin instance), and no stable or draft extension
addresses another plugin's parameters (prior-art review, CLAP ecosystem notes).

## Decision

A small CLAP extension, defined in `core/clap-ext/include/oscairn-clap/param-target.h`.

- **Lanes, not addresses.** A plugin sends `oscairn_event_param_target` events on numbered
  output *lanes* (for the step sequencer, lane 0 is "Lock A"). It does not know what a lane
  controls. The host keeps a mapping from (plugin, lane) to (target plugin, parameter), set
  by the user or UI: `Graph::mapParamTarget(source, lane, target, param)`. Plugins stay
  independent of each other and of the host's node IDs, and one sequencer works with any
  instrument.
- **Values are relative to the target's range.** `SET` carries 0..1 over the target
  parameter's range; `MODULATE` carries an amount of -1..1 of that range. The host converts
  to the target's plain values (rounding stepped parameters).
- **Standard CLAP events at the target.** The host turns each lane event into a
  `CLAP_EVENT_PARAM_VALUE` (SET) or `CLAP_EVENT_PARAM_MOD` (MODULATE) for the target, at the
  same sample, with the note, port, channel and key fields copied over. Target plugins need
  no knowledge of the extension.
- **Same block, before notes.** A mapping is a dependency: the target is processed after
  the source in every block, so the converted events arrive in the block they were sent, at
  their exact sample. They are ordered before notes at the same sample, so a step's lock
  applies to that step's note.
- **Discovered through CLAP's own mechanisms.** The event space is obtained with the stable
  `clap.event-registry` extension (name `oscairn.param-target`); the Oscairn host returns
  space 1000. A host that does not know it returns false and the plugin sends nothing.
  Lanes are listed through the plugin extension `oscairn.param-target-lanes/draft` (count,
  and a name for each lane) so a UI can offer them.

## Implementation

- `core/clap-host`: the host offers `clap.event-registry`, accepts param-target events from
  plugins (validated: size, mode, finite value), returns them in `outputEvents()`, and never
  delivers events from non-core spaces to a plugin.
- `core/engine` (`Graph`): mappings in the graph, as dependencies in the processing order;
  conversion to CLAP parameter events for the target in the same block.
- `plugins/step-sequencer`: lane 0, a lock value per step, sent at the step's sample before
  its note. `plugins/basic-synth` has parameters to lock (Brightness, Level).
- `oscairn-render --map SRC.LANE=DST.PARAM`.

## Verification

`param-target` test: lane 0 mapped to the gain of a test instrument whose output while a note
is held is exactly its gain. Every step's output equals that step's lock value from the
step's first sample, with 256- and 37-sample blocks; without the mapping the gain stays at
its default.

Not yet verified by a test: the ordering before notes at the same sample (the test
instruments read parameters every sample, so the order does not change their output), and
the `MODULATE` path (no first-party plugin sends it yet).

## Open questions

- Should the header be licensed permissively (MIT) so other hosts and plugins can adopt it?
  It is AGPL-3.0-or-later like the rest of the repo for now.
- Whether to propose it to the CLAP project as a draft extension, and how it should relate to
  `CLAP_EVENT_PARAM_MOD`'s per-note modulation and to host-side modulation routing.
- Lane metadata beyond a name (units, default mapping hints, bipolar or unipolar).
