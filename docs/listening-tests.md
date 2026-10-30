# Queued listening tests

Status: **pending Jacob's listening**, October 29, 2026. Jacob is away; do not start
playback until he is available. Automated rendering does not count as audible acceptance.
Before each grouped listening prompt, run `/Users/jacob/.codex/bin/prompt-ding` immediately
before the listening window. Report if the notification fails; command success does not
prove it was heard.

## 1. Two sounds, shared effects, Performance reopen

- [ ] Replay detuned saws with short echo and square vibrato with long echo, including held
  notes while switching sounds. Listen for distinct sounds, overlap, uninterrupted tails,
  clicks, stuck notes and dropouts.
- [ ] Save a Performance, stop/restart the test engine, open the folder, and replay it.
  Confirm both sounds, assignments and separate delay settings survive.

This carries forward the previous Performance milestone's unconfirmed listening check.
Earlier automated/live telemetry checks are not a substitute for this response.

## 2. Two independent tracks and A/B launches

- [ ] Hear both tracks together: MIDI 1 saws / MIDI 2 square. Track A lengths 16 and 12
  should cycle independently.
- [ ] Edit B while A plays; A must continue unchanged. Launch B mid-bar; both tracks
  change together at the next bar and restart their cycles (B lengths 8 and 5).
- [ ] Mute/unmute track 2; track 1 continues. Release tails may continue after mute.
- [ ] Assign different prepared sounds with **Use sound**, then play on-screen pads on
  each selected track. Changing track/channel while holding a pad must release the
  original note when that pad is released. Physical MIDI keeps its own channel.
- [ ] Save/restart/open and repeat; patterns, lengths, channels, mutes and sounds survive.
  Opening is intentionally stopped and rewound; held notes are not restored.

## Repeatable setup

Use a **fresh disposable** `oscairn-engine` session and attach the UI in KICK layout.
Choose a working output explicitly when starting it; the default Multi-Output Device hung
in an earlier local run, while `--output-device 'MacBook Pro Speakers'` worked. Do not
reuse a musician's current session: these scripts change its graph and prepared sounds.
Use the current build (sequencer 0.2.0); old 0.1.0 Performance folders are not migrated.

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
