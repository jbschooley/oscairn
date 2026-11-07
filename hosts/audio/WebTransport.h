// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace oscairn::host
{
    // Lets package repositories (RFC 0031) fetch over HTTP(S) through JUCE; the audio engine
    // calls it when it starts.
    void useWebTransport();
}
