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

## Copied code

Code copied from other AGPL or compatible projects keeps its original copyright notices.
