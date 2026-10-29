// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/plugin_host.h"
namespace oscairn {
std::shared_ptr<PluginHost> preparedInstrument(std::shared_ptr<PluginHost>,double,uint32_t,ProcessingMode,Lv2BufferLimits);
}
