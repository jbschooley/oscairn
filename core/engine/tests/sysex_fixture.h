// SPDX-License-Identifier: AGPL-3.0-or-later
// The SysEx fixture's messages, shared by the fixture and its test.
#pragma once
#include <cstdint>

namespace sysexFixture
{
    constexpr uint32_t limit = 64 * 1024; // the host's largest message (kick::clap::SysexPool::maxMessage)
    constexpr uint32_t big = 60 * 1024;
    // F0 7D, then 7-bit bytes that depend on the seed, then F7.
    inline void fill(uint8_t* bytes, uint32_t size, uint32_t seed)
    {
        bytes[0] = 0xF0; bytes[1] = 0x7D;
        for(uint32_t i = 2; i + 1 < size; ++i) bytes[i] = uint8_t((i * 31u + seed * 7u + (i >> 8)) & 0x7F);
        bytes[size - 1] = 0xF7;
    }
}
