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
 * Hints only shape the editor; every parameter stays reachable whatever its hint. A host
 * that does not know this extension ignores it, and a parameter without a hint is drawn as
 * the host decides from its metadata (stepped values, ranges, module path).
 *
 * param_hint is main-thread; the host asks it when it reads parameter info (at load and
 * after a rescan of the parameters' info), so hints may change only with that info.
 */
static const char OSCAIRN_EXT_UI[] = "oscairn.ui/draft-1";

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

typedef struct oscairn_param_hint
{
    uint32_t widget;    /* enum oscairn_widget; values a host does not know mean AUTO */
    uint32_t priority;  /* 1 is the most important; 0 is none, after every numbered one */
    clap_id range_high; /* OSCAIRN_WIDGET_RANGE: the range's high parameter; else CLAP_INVALID_ID */
} oscairn_param_hint_t;

typedef struct oscairn_plugin_ui
{
    /* [main-thread] Fill `hint` for the parameter `param_id`; false when it has none. */
    bool(CLAP_ABI* param_hint)(const clap_plugin_t* plugin, clap_id param_id, oscairn_param_hint_t* hint);
} oscairn_plugin_ui_t;

#ifdef __cplusplus
}
#endif
