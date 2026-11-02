/* SPDX-License-Identifier: MIT */
#pragma once
#include <clap/clap.h>

#ifdef __cplusplus
extern "C" {
#endif

/* oscairn.ui (draft): how a plugin's parameters are best shown and played, for hosts that
 * draw editors themselves on screens of any size (Oscairn's RFC 0019) rather than open the
 * plugin's window. A hint per parameter: the control it wants, and a priority, which decides
 * what a small screen shows first and the order physical knobs take parameters in.
 *
 * Parameters that belong together (an envelope's four stages) share a group: a host draws
 * them joined, edge to edge in their priority order, and keeps them together when the screen
 * is too narrow for all of them in a row.
 *
 * A parameter may also name its role (cutoff, attack, an effect's mix): what it does in terms
 * every instrument shares, so a host can put it on the same physical knob whatever the plugin
 * (Oscairn's role banks, RFC 0011). Oscairn guesses roles from the parameters' names for a plugin
 * that names none, and only then; where no role fills a bank, knobs go by priority.
 *
 * Hints only shape the editor; every parameter stays reachable whatever its hint. A host
 * that does not know this extension ignores it, and a parameter without a hint is drawn as
 * the host decides from its metadata (stepped values, ranges, module path).
 *
 * param_hint is main-thread; the host asks it when it reads parameter info (at every activation
 * and after a rescan of the parameters' info), so hints may change only with that info. Oscairn
 * ignores this extension when param_hint is null.
 */
static const char OSCAIRN_EXT_UI[] = "oscairn.ui/draft-3";

enum oscairn_widget
{
    OSCAIRN_WIDGET_AUTO = 0,    /* the host decides */
    OSCAIRN_WIDGET_SLIDER = 1,
    OSCAIRN_WIDGET_KNOB = 2,
    OSCAIRN_WIDGET_BUTTONS = 3, /* stepped: each value a button, side by side */
    OSCAIRN_WIDGET_SWITCH = 4,  /* stepped, two values: off and on */
    OSCAIRN_WIDGET_LIST = 5,    /* stepped: a list of the values' texts */
    OSCAIRN_WIDGET_RANGE = 6,   /* the low end of a range; range_high names its high end, and
                                   both are one two-sided control */
};

/* Roles, in the order of the role banks they fill (Filter, Amp EG, EQ, Levels). */
enum oscairn_role
{
    OSCAIRN_ROLE_NONE = 0,
    OSCAIRN_ROLE_CUTOFF = 1,
    OSCAIRN_ROLE_RESONANCE = 2,
    OSCAIRN_ROLE_FILTER_ENVELOPE = 3, /* the filter envelope's amount */
    OSCAIRN_ROLE_KEY_TRACKING = 4,    /* the filter's */
    OSCAIRN_ROLE_FILTER_ATTACK = 5,
    OSCAIRN_ROLE_FILTER_DECAY = 6,
    OSCAIRN_ROLE_FILTER_SUSTAIN = 7,
    OSCAIRN_ROLE_FILTER_RELEASE = 8,
    OSCAIRN_ROLE_ATTACK = 9,          /* the amplitude envelope's stages */
    OSCAIRN_ROLE_DECAY = 10,
    OSCAIRN_ROLE_SUSTAIN = 11,
    OSCAIRN_ROLE_RELEASE = 12,
    OSCAIRN_ROLE_HOLD = 13,
    OSCAIRN_ROLE_VELOCITY = 14,       /* velocity's effect on level */
    OSCAIRN_ROLE_AMP_KEY_TRACKING = 15,
    OSCAIRN_ROLE_LEVEL = 16,          /* the instrument's output level */
    OSCAIRN_ROLE_LOW_GAIN = 17,       /* EQ */
    OSCAIRN_ROLE_MID_GAIN = 18,
    OSCAIRN_ROLE_HIGH_GAIN = 19,
    OSCAIRN_ROLE_MID_FREQUENCY = 20,
    OSCAIRN_ROLE_LOW_FREQUENCY = 21,
    OSCAIRN_ROLE_HIGH_FREQUENCY = 22,
    OSCAIRN_ROLE_MID_Q = 23,
    OSCAIRN_ROLE_EQ_OUTPUT = 24,
    OSCAIRN_ROLE_LEVEL_1 = 25,        /* element, operator or oscillator levels, 1 to 8 */
    OSCAIRN_ROLE_LEVEL_8 = 32,
    OSCAIRN_ROLE_MIX = 33,            /* an effect's main control: its mix, or its amount */
    OSCAIRN_ROLE_PAN = 34,
};

typedef struct oscairn_param_hint
{
    uint32_t widget;    /* enum oscairn_widget; values a host does not know mean AUTO */
    uint32_t priority;  /* 1 is the most important; 0 is none, after every numbered one */
    clap_id range_high; /* OSCAIRN_WIDGET_RANGE: the range's high parameter; else CLAP_INVALID_ID */
    uint32_t group;     /* parameters with the same nonzero group, in one module, are drawn joined */
    uint32_t role;      /* enum oscairn_role; values a host does not know mean NONE */
} oscairn_param_hint_t;

typedef struct oscairn_plugin_ui
{
    /* [main-thread] Fill `hint` for the parameter `param_id`; false when it has none. */
    bool(CLAP_ABI* param_hint)(const clap_plugin_t* plugin, clap_id param_id, oscairn_param_hint_t* hint);
} oscairn_plugin_ui_t;

#ifdef __cplusplus
}
#endif
