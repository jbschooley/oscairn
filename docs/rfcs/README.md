# RFCs

Each design decision gets a short document: what was decided, why, and what it rules out.
Number them in order (`0002-...md`) and keep the status line up to date
(proposed, accepted, replaced).

| RFC | Title | Status |
| --- | --- | --- |
| [0001](0001-licence.md) | Licence | accepted |
| [0002](0002-param-target.md) | Parameter changes between plugins (oscairn.param-target) | draft, implemented |
| [0003](0003-control-api.md) | Control API | partially implemented; local JSON-RPC transport and explicit engine/UI separation |
| [0004](0004-hardware-services.md) | Hardware services, event timing and engine backends | proposed |
| [0005](0005-sound-contexts.md) | Native prepared sound contexts | instrument contexts and full snapshots implemented; SFZ patches via the Sampler; effects covered by 0007 |
| [0006](0006-packages-and-portability.md) | Packages and cross-device portability | proposed |
| [0007](0007-effect-contexts.md) | Independent effects on shared native engines | implemented Delay/bus foundation; automatic chain management planned |
| [0008](0008-sfz-native-samples-and-conversion.md) | SFZ-based native samples and Yamaha conversion | one-region Sampler implemented; multisample, extensions and converters planned |
