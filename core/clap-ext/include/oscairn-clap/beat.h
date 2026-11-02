/* SPDX-License-Identifier: MIT */
#pragma once

/*
 * oscairn.beat (draft): a beat position that runs whether or not the transport plays, the
 * same for every plugin in a block, so plugins keep time and start together while the
 * transport is stopped (an arpeggiator's Sync Quantize, a script's clock).
 *
 * The transport event keeps CLAP's meaning: while stopped, its position does not move. This
 * position never stops: it advances at the transport's tempo from the moment the host starts,
 * through starts, stops, jumps and Performance changes, and never goes back. Its origin means
 * nothing; plugins use it to agree, for example on the next whole beat.
 *
 * A plugin asks for these events by querying the event space OSCAIRN_EVENT_SPACE_BEAT through
 * CLAP's event-registry extension. The host then sends an oscairn_event_beat at sample 0 of
 * every block, before the block's other events; a plugin that does not ask never gets one.
 * When the host splits a block into several process calls, only the first carries it: the
 * position moves on at `tempo` from there.
 *
 * See docs/rfcs/0032-arpeggiator.md.
 */

#include <clap/clap.h>

#ifdef __cplusplus
extern "C" {
#endif

static const char OSCAIRN_EVENT_SPACE_BEAT[] = "oscairn.beat";

enum
{
    OSCAIRN_EVENT_BEAT = 0,
};

typedef struct oscairn_event_beat
{
    clap_event_header_t header; /* space_id from event-registry, type OSCAIRN_EVENT_BEAT, time 0 */

    clap_beattime beats; /* the free-running position at header.time, in CLAP_BEATTIME_FACTOR units */
    double        tempo; /* beats per minute it moves at through this block (the transport's) */
} oscairn_event_beat_t;

#ifdef __cplusplus
}
#endif
