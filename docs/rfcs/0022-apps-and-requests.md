# RFC 0022: Apps that open each other

Status: in progress, 2026-10-31. Step 1 is built in the desktop app: the registry
(`hosts/standalone/Apps.h`), the Library answering `oscairn.pick.performance` and the
Setlist taking `oscairn.send.performance`, each flow checked by `desktop-shell`. One app
per action for now (the first to offer it); manifests, the control API transport and the
chooser are not built. Builds on the separation of apps from the sound
engine ([architecture](../architecture.md#two-kinds-of-thing-plugins-and-the-ui)), the
control API ([RFC 0003](0003-control-api.md)), the library
([RFC 0021](0021-sounds-performances-and-library.md)) and setlists
([RFC 0012](0012-sync-external-midi-and-multi-device.md)).

## Problem

Apps are separate components: the Groovebox, a Setlist app, the Library, the sound editor.
Musicians move between them all the time:

- from the Library, playing straight from the list (like the MODX's Category Search), add a
  Performance to the set for Friday;
- in the Setlist app, fill a slot by choosing a Performance from the Library;
- in the Groovebox, choose a part's Sound from the Library;
- from a part in the sound editor, open the Groovebox's pattern for it.

If each app links to the others, none can be replaced, and a third party's setlist or library
app is a second-class citizen. If none do, the musician copies names by hand.

## Outcome

Apps offer **actions** to each other by name, and ask for them without knowing which app
answers:

- **Pick** something and get it back: "pick a Performance", "pick a Sound for this part".
- **Send** something to whoever takes it: "add this Performance to a setlist".
- **Open** something: "open this Performance in the sound editor", "open this part's pattern".

An action that no installed app offers is not shown. When two apps offer it, the musician
chooses once, and may change the default in settings. Oscairn's own apps use the same route
as anyone else's.

## Actions

An action has a name, what it takes and what it gives back. Names are reverse-domain strings,
like CLAP's extensions, so third parties can add their own without collisions.

| Action | Takes | Gives back | Offered by |
| --- | --- | --- | --- |
| `oscairn.pick.performance` | optional tags to start with | a library ID | Library |
| `oscairn.pick.sound` | optional part, tags | a library ID | Library |
| `oscairn.send.performance` | a library ID | done or cancelled | Setlist |
| `oscairn.open.performance` | a library ID | | Sound editor |
| `oscairn.open.part` | a part | | Sound editor |
| `oscairn.open.pattern` | a part or sequencer node | | Groovebox |

What is passed is always a reference the engine already knows (a library ID, a part, a node),
never the content: apps share the one Performance through the control API, so an action only
has to say *which*.

## How it works

- **An app's manifest** lists the actions it offers, with a name and an icon for menus. It is
  part of the app's package ([RFC 0006](0006-packages-and-portability.md)), read at install,
  so the host knows the offers without starting the app.
- **The host** (the shell on the desktop, the system UI on a device) keeps the list, decides
  who answers, starts or brings forward that app, and passes the request and the answer back.
  Apps never call each other directly.
- **A pick opens over the asking app:** a sheet on the device, a dialog or a pane on the
  desktop, in the answering app's own view at the asking app's size class (RFC 0019). The asker
  waits for an answer or a cancel, and is never blocked: a pick has no time limit, and the
  asker may be closed meanwhile, in which case the answer is dropped.
- **A send** is fire and forget with an acknowledgement, so "Add to setlist" can say "Added to
  Friday".
- **Transport:** requests go through the engine's control API as messages addressed to an app
  (`apps.request`, `apps.answer` and an `apps` notification topic), because every app already
  holds a control connection and the engine outlives any one app. The engine relays; it does
  not interpret actions.

## Third-party apps

- A third-party setlist or library app offers the same actions and is listed beside Oscairn's.
  The musician picks a default per action; nothing gives Oscairn's own apps priority.
- **A third-party library** may keep its own content (a sample browser, a cloud library). Its
  picks still give back a library ID, so what it hands over must be in the Oscairn library:
  it imports or registers the item first (RFC 0021's index, RFC 0009's registered folders), so
  saving and portability keep working without that app.
- **Providers without a whole app:** a package may add *content* to the picker (a pack's
  Sounds and tags) without offering actions; that is RFC 0021's package content, not this RFC.
- **Trust:** an app answers only actions it declared; an answer is checked against what the
  action gives back (a library ID that exists) before the asker sees it.

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1. In-process actions | The registry and requests inside the desktop app; Library pick and Setlist send between the built-in apps | From the Setlist app, fill a slot through the Library's pick; from the Library, send a Performance to the setlist |
| 2. Over the control API | `apps.request`, `apps.answer`, the `apps` topic; manifests | The same two flows with the Setlist app in a separate process |
| 3. Choice and defaults | Two apps offering one action; the chooser and its setting | A second setlist app answers once chosen, and the choice is kept |

## Open questions

- Whether an answer can return several items (pick three Sounds for a split).
- How a device without a touch screen chooses between two apps.
- Whether actions belong in the control API's capability list for clients that are not apps.

## Ruled out

- **Apps linking to each other directly:** none could be replaced.
- **Passing content between apps:** the Performance is shared through the engine; actions pass
  references.
- **A preferred vendor:** Oscairn's apps answer only by being the default the musician keeps.
