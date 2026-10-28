// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <filesystem>
#include <vector>

namespace oscairn
{
    /// Writes interleaved samples as a 32-bit float WAV file. Throws std::runtime_error on failure.
    void writeWavFile( const std::filesystem::path& path, const std::vector<float>& interleaved, int channels, int sampleRate );
}
