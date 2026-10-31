# RFC 0001: Licence

**Status:** accepted

## Decision

The Oscairn core, hosts, first-party plugins and tools are licensed under the
**GNU Affero General Public License v3.0 or later** (AGPL-3.0-or-later).

## Reasons

- It matches the KICK firmware (MusicTribeOpenSource/0700-AAM), so code can move between the
  two in either direction.
- Anyone who ships a modified Oscairn, on hardware or as a network service, must publish their
  changes.
- JUCE (version 8 and later, including the 9.0.4 the desktop app uses) is available under
  AGPLv3, so it can be used without a commercial licence where it is useful, such as UI code.
  The core does not depend on it.

## The plugin boundary

Plugins loaded through a plugin API (CLAP, LV2, VST3) are separate works. They may use any
licence, including proprietary ones. Code linked into the core is covered by the AGPL.

The Oscairn CLAP extension headers (`core/clap-ext/include/oscairn-clap/`) are **MIT**
licensed, as is CLAP itself (decided by Jacob, October 30, 2026). A plugin includes them to
use Oscairn's extensions, so an AGPL licence there would have contradicted the boundary
above. The rest of the core stays AGPL. They are the start of the MIT plugin SDK, with
`sound-contexts-plugin.hpp`, the C++ helper that implements native sound contexts for an
instrument (relicensed from AGPL to MIT by Jacob, October 31, 2026).

## Copied code

Code copied from other AGPL or compatible projects keeps its original copyright notices.
