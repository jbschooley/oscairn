/* SPDX-License-Identifier: MIT */
#pragma once

/*
 * oscairn.harmony (draft): the key and scale of the part a plugin is in, and the chord held
 * on that part, for plugins that follow the harmony (a harmonizer, a chord memory, a script).
 *
 * This is not tuning: the scale says which notes belong to the key, not how they are tuned.
 *
 * The key and scale are the Performance's, or the part's own where it overrides them. The
 * chord is the host's reading of the keys held at the part's input, after the part's zones
 * (so a transposing zone transposes the chord), not of the notes a plugin before this one
 * sends: an arpeggiator's output does not change it. Sustained keys that are no longer held
 * do not count.
 *
 * Two ways to read it:
 *  - Events. A plugin that queries the event space OSCAIRN_EVENT_SPACE_HARMONY through CLAP's
 *    event-registry extension gets an oscairn_event_harmony in the first block it processes
 *    and in each block where anything in oscairn_harmony_t changes, at the sample of the
 *    change: the sample of the note that changed the chord, before that note. A plugin that
 *    does not ask never gets one.
 *  - The host extension OSCAIRN_EXT_HARMONY, on the main thread: the harmony as of the last
 *    block the plugin processed.
 *
 * See docs/rfcs/0033-script-block.md.
 */

#include <clap/clap.h>

#ifdef __cplusplus
extern "C" {
#endif

static const char OSCAIRN_EVENT_SPACE_HARMONY[] = "oscairn.harmony";
static const char OSCAIRN_EXT_HARMONY[] = "oscairn.harmony/draft";

enum
{
    OSCAIRN_EVENT_HARMONY = 0,
};

/* Chord types: the intervals above the root, in semitones. */
enum oscairn_chord_type
{
    OSCAIRN_CHORD_NONE = 0,       /* no chord: fewer than two pitch classes, or none of these */
    OSCAIRN_CHORD_MAJOR,          /* 0 4 7 */
    OSCAIRN_CHORD_MINOR,          /* 0 3 7 */
    OSCAIRN_CHORD_DIMINISHED,     /* 0 3 6 */
    OSCAIRN_CHORD_AUGMENTED,      /* 0 4 8 */
    OSCAIRN_CHORD_SUS2,           /* 0 2 7 */
    OSCAIRN_CHORD_SUS4,           /* 0 5 7 */
    OSCAIRN_CHORD_POWER,          /* 0 7 */
    OSCAIRN_CHORD_MAJOR6,         /* 0 4 7 9 */
    OSCAIRN_CHORD_MINOR6,         /* 0 3 7 9 */
    OSCAIRN_CHORD_DOMINANT7,      /* 0 4 7 10 */
    OSCAIRN_CHORD_MAJOR7,         /* 0 4 7 11 */
    OSCAIRN_CHORD_MINOR7,         /* 0 3 7 10 */
    OSCAIRN_CHORD_MINOR_MAJOR7,   /* 0 3 7 11 */
    OSCAIRN_CHORD_HALF_DIMINISHED7, /* 0 3 6 10 (m7b5) */
    OSCAIRN_CHORD_DIMINISHED7,    /* 0 3 6 9 */
    OSCAIRN_CHORD_AUGMENTED7,     /* 0 4 8 10 */
    OSCAIRN_CHORD_DOMINANT7_SUS4, /* 0 5 7 10 */
    OSCAIRN_CHORD_ADD9,           /* 0 2 4 7 */
    OSCAIRN_CHORD_MINOR_ADD9,     /* 0 2 3 7 */
    OSCAIRN_CHORD_DOMINANT9,      /* 0 2 4 7 10 */
    OSCAIRN_CHORD_MAJOR9,         /* 0 2 4 7 11 */
    OSCAIRN_CHORD_MINOR9,         /* 0 2 3 7 10 */
};

typedef struct oscairn_harmony
{
    uint8_t  key_root;   /* the key's root: 0 = C .. 11 = B */
    uint8_t  chord_root; /* the chord's root, 0..11; 0 with no chord */
    uint8_t  chord_type; /* enum oscairn_chord_type */
    uint8_t  chord_bass; /* the lowest held key's pitch class, 0..11 (an inversion's bass); 0 with no keys */
    uint16_t scale;      /* bit n set: the pitch class n semitones above key_root is in the scale (bit 0, the root, always) */
    uint16_t reserved;   /* 0 */
    uint64_t held[2];    /* the keys held at the part's input: bit k of held[k / 64] for MIDI key k */
} oscairn_harmony_t;

/* A chord read from the held keys follows a fixed table of the types above. Where the same
 * pitch classes read as more than one chord (C6 and Am7, the symmetric augmented and
 * diminished seventh), the reading rooted on the bass wins; otherwise the first in the
 * table's order. Doubled and octave-spread keys read as their pitch classes. */

typedef struct oscairn_event_harmony
{
    clap_event_header_t header; /* space_id from event-registry, type OSCAIRN_EVENT_HARMONY */
    oscairn_harmony_t   harmony;
} oscairn_event_harmony_t;

typedef struct oscairn_host_harmony
{
    /* [main-thread] The harmony as of the last block the plugin processed; false before the
     * first, or for a plugin in no part. */
    bool( CLAP_ABI* get )( const clap_host_t* host, oscairn_harmony_t* harmony );
} oscairn_host_harmony_t;

#ifdef __cplusplus
}
#endif
