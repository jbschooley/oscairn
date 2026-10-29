// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/midi_input.h"
#include <array>
namespace oscairn::host
{
// The graph assigns note IDs to raw MIDI presses, preserving FIFO releases.
struct MidiBlock
{
    std::array<kick::clap::Event,128> events{};
    uint32_t count=0,dropped=0;
    void reset() noexcept {count=dropped=0;}
    void append(const uint8_t* bytes,size_t size) noexcept
    {
        kick::clap::Event event;
        if(!midiToClap(bytes,size,0,event))return;
        if((bytes[0]&0xf0)==0x80 || (bytes[0]&0xf0)==0x90)
            event=kick::clap::Event::midi(bytes[0],bytes[1],bytes[2]);
        if(count==events.size()){++dropped;return;}
        events[count++]=event;
    }
};
}
