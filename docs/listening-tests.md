# Queued listening tests

Status: sections 1–3 listened to by Jacob on October 30, 2026; open items are marked below. Do not start
playback until Jacob is available. Automated rendering does not count as audible acceptance.
Before each grouped listening prompt, run `/Users/jacob/.codex/bin/prompt-ding` immediately
before the listening window. Report if the notification fails; command success does not
prove it was heard.

## 1. Two sounds, shared effects, Performance reopen

- [x] Replay detuned saws with short echo and square vibrato with long echo, including held
  notes while switching sounds. Listen for distinct sounds, overlap, uninterrupted tails,
  clicks, stuck notes and dropouts. **Jacob: all good, October 30, 2026** (Multi-Output
  Device, 48 kHz/128; 0 deadline misses, 0 event drops).
- [x] Save a Performance, stop/restart the test engine, open the folder, and replay it.
  Confirm both sounds, assignments and separate delay settings survive. **Jacob: same, all
  good, October 30, 2026** (saved and opened through `performance.save`/`performance.load`,
  not the desktop menu).

This carries forward the previous Performance milestone's unconfirmed listening check.
Earlier automated/live telemetry checks are not a substitute for this response.

## 2. Two independent tracks and A/B launches

- [x] Hear both tracks together: MIDI 1 saws / MIDI 2 square. Track A lengths 16 and 12
  should cycle independently. **Jacob: all good, October 30, 2026** (demo, Multi-Output
  Device; 0 deadline misses, 0 event drops).
- [x] Edit B while A plays; A must continue unchanged. Launch B mid-bar; both tracks
  change together at the next bar and restart their cycles (B lengths 8 and 5).
  **Jacob: "looked good", October 30, 2026** (hands-on in the KICK layout).
- [x] Mute/unmute track 2; track 1 continues. Release tails may continue after mute.
  **Jacob: all good, October 30, 2026** (demo).
- [ ] Assign different prepared sounds with **Use sound**, then play on-screen pads on
  each selected track. Changing track/channel while holding a pad must release the
  original note when that pad is released. Physical MIDI keeps its own channel.
  *October 30: per-track sounds through the pads sounded right (Jacob). Holding a pad while
  changing track needs two simultaneous inputs, impossible with one mouse (keyboard focus
  does not reach the track selector while a pad is held); still open for a touchscreen or
  the KICK hardware.*
- [x] Save/restart/open and repeat; patterns, lengths, channels, mutes and sounds survive.
  Opening is intentionally stopped and rewound; held notes are not restored. **Jacob: worked,
  October 30, 2026**, including a note edited after the first reopen and saved again
  (saved/opened through `performance.save`/`performance.load`).

## 3. Sampler import and playback (RFC 0008 step 2)

Also the first interactive run of the desktop import; automated tests cover the model code
behind it, not the JUCE menu, file chooser or button state.

- [x] Replace the synth with **Oscairn Sampler** in the plugin browser. **Prepare...** is
  enabled and offers **Import sample or SFZ...** only. *October 30: works from the default
  graph. After the shared-effects setup, replace refuses with "Incompatible audio buses;
  explicit routing is required" because the synth's routed sound buses have no
  counterpart on the Sampler.*
- [x] Import a mono or stereo WAV (at most 64 MiB decoded). It appears named after the
  file. **Use sound**, then play: middle C (60) is the recorded pitch, other keys are
  transposed, and a released note fades out quickly (default `ampeg_release` 1 ms).
  *October 30: plays (Jacob). Found: the picker stays on the previous sound after an
  import, so **Use sound** looked available but applied nothing until the new sound was
  chosen; and the 1 ms default release ticks audibly between notes.*
- [x] Import an `.sfz` beside a WAV containing `<region> sample=NAME.wav ampeg_release=0.5`.
  Hold a note on the first sound, switch to the second, play: the held note keeps its
  sound and both overlap; the new release takes half a second. **Jacob: worked, October 30,
  2026** (switched with **Use sound** while the pattern played; no ticks with the 0.5 s release).
- [x] Import an `.sfz` with an unsupported opcode such as `pan=10`: the picker shows
  `line N: unsupported opcode pan` and playback is not interrupted. **Jacob: worked,
  October 30, 2026.**
- [x] Save, restart, open and replay: both sounds return. Then rename the WAV and open
  again: opening fails with a message naming the sample, and the current sound keeps
  playing. **Jacob: worked, October 30, 2026.** The save contained no audio; the failed
  open reported `sample not found: tone.wav (in …/Oscairn sampler test)` and left the
  graph revision unchanged.

Level note: the Sampler matches liquidsfz's default level, 7.2 dB below the sample's own
level (default CC7 volume and centre pan law; see RFC 0008 step 2 status). Judge whether
that sounds right next to the Basic Synth.

## Repeatable setup

Use a **fresh disposable** `oscairn-engine` session and attach the UI in KICK layout.
Choose a working output explicitly when starting it; the default Multi-Output Device hung
in an earlier local run, while `--output-device 'MacBook Pro Speakers'` worked. Do not
reuse a musician's current session: these scripts change its graph and prepared sounds.
Use the current build and recreate test Performances after incompatible changes.

From the repository root, with `SOCKET` pointing to that private test socket:

```sh
# Configuration only; neither command starts playback.
python3 hosts/engine/examples/shared-effects.py "$SOCKET"
python3 hosts/engine/examples/tracks-and-patterns.py "$SOCKET"
```

Save the configured Performance with **Save Performance As...**. Use **Open Performance...**
after restarting to verify the roundtrip. The saved folder contains `performance.json`
and per-node state; retain the whole folder.

When Jacob is available, replay the tracks demo for roughly 26 seconds:

```sh
python3 hosts/engine/examples/tracks-and-patterns.py "$SOCKET" --play-only
```

It plays A, requests B, mutes/unmutes the square track, requests A, then stops and lets
its echoes decay. The script prints host status afterward. Check deadline/event-drop
counters as well as listening. Bar launches can wait up to one bar from the request.

For the separate held-note/effect-tail check, use a separate fresh setup from
`shared-effects.py`, save/reopen it, then:

```sh
python3 hosts/engine/examples/shared-effects.py "$SOCKET" --play-only
```

That demo intentionally changes all-channel sound selection; rerun the two-track
configuration before testing track assignments again. Mark each checkbox only after
Jacob confirms the corresponding result, and record any failures here.

## Automated and silent checks completed

The desktop build passes all 59 tests, including socket/helper lifecycle checks. Thirteen
focused tests pass under ThreadSanitizer and ASan/UBSan. Apple ASan reports stale globals
when instrumented CLAP images unload/reload on this Mac; the same fault reproduces with
the previous sequencer in a minimal `dlopen`/`dlclose` loop. The memory-check run retained
instrumented images for its process lifetime, without disabling sanitizer checks or
changing the production loader. Normal and ThreadSanitizer runs still unload plugins.

The configure-only scripts ran against a fresh stopped engine. A save/mutate/reopen check
restored all 203 writable sequencer parameters; status showed 48 kHz/128 frames and zero
deadline, graph-event or MIDI-event drops. KICK snapshots were inspected at lengths 16
and 8, including dimmed steps beyond the chosen length. Interactive pad-release and
listening acceptance remain in the checklist above.
