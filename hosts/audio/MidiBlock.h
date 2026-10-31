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
    std::array<uint8_t,128> sources{}; // the MIDI input each came from (note_zone.h)
    uint32_t count=0,dropped=0;
    void reset() noexcept {count=dropped=0;}
    void append(const uint8_t* bytes,size_t size,uint8_t source=untaggedSource) noexcept
    {
        kick::clap::Event event;
        if(!midiToClap(bytes,size,0,event))return;
        if((bytes[0]&0xf0)==0x80 || (bytes[0]&0xf0)==0x90)
            event=kick::clap::Event::midi(bytes[0],bytes[1],bytes[2]);
        if(count==events.size()){++dropped;return;}
        sources[count]=source;events[count++]=event;
    }
};
}
