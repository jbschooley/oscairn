/* SPDX-License-Identifier: MIT */
#pragma once

/*
 * oscairn.param-target (draft): parameter changes sent by one plugin to the parameters of
 * other plugins, routed by the host within the same block.
 *
 * A plugin such as a step sequencer has numbered output "lanes". It sends
 * oscairn_event_param_target events on those lanes, in the event space the host returns
 * for OSCAIRN_EVENT_SPACE_PARAM_TARGET through CLAP's event-registry extension. The host
 * keeps a mapping from (plugin, lane) to (target plugin, parameter); the user or UI sets
 * it up, so the sending plugin never needs to know what it controls. The host turns each
 * event into a CLAP_EVENT_PARAM_VALUE (mode SET) or CLAP_EVENT_PARAM_MOD (mode MODULATE)
 * for the target, at the same sample time, ordered before notes at that time, so a
 * step's parameter lock applies to that step's note. The target is processed after the
 * source in the same block.
 *
 * Hosts that do not know this event space return false from event-registry's query, and
 * the plugin then sends nothing. Plugins that do not know it never see these events.
 *
 * See docs/rfcs/0002-param-target.md.
 */

#include <clap/clap.h>

#ifdef __cplusplus
extern "C" {
#endif

static const char OSCAIRN_EVENT_SPACE_PARAM_TARGET[] = "oscairn.param-target";

enum
{
    OSCAIRN_EVENT_PARAM_TARGET = 0,
};

enum oscairn_param_target_mode
{
    /* Set the target parameter: value is normalised, 0..1 over the target's range. */
    OSCAIRN_PARAM_TARGET_SET = 0,
    /* Modulate the target parameter: value is an amount, -1..1 of the target's range. */
    OSCAIRN_PARAM_TARGET_MODULATE = 1,
};

/* Plugin extension: lists the plugin's lanes so a host or UI can offer them for mapping. */
static const char OSCAIRN_EXT_PARAM_TARGET_LANES[] = "oscairn.param-target-lanes/draft";

typedef struct oscairn_param_target_lane_info
{
    uint32_t lane;
    char     name[CLAP_NAME_SIZE];
} oscairn_param_target_lane_info_t;

typedef struct oscairn_plugin_param_target_lanes
{
    /* [main-thread] */
    uint32_t( CLAP_ABI* count )( const clap_plugin_t* plugin );
    /* [main-thread] */
    bool( CLAP_ABI* get )( const clap_plugin_t* plugin, uint32_t index, oscairn_param_target_lane_info_t* info );
} oscairn_plugin_param_target_lanes_t;

typedef struct oscairn_event_param_target
{
    clap_event_header_t header; /* space_id from event-registry, type OSCAIRN_EVENT_PARAM_TARGET */

    uint32_t lane; /* the sending plugin's output lane */
    uint32_t mode; /* oscairn_param_target_mode */

    /* Which notes to affect, as in CLAP parameter events; -1 is a wildcard. */
    int32_t note_id;
    int16_t port_index;
    int16_t channel;
    int16_t key;

    double value;
} oscairn_event_param_target_t;

#ifdef __cplusplus
}
#endif
