#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <limits>

namespace ntc {
namespace fs = std::filesystem;

enum class CloDestination { Gp200, Gp5 };

constexpr std::size_t destinationBTaps(CloDestination destination) {
    return destination == CloDestination::Gp5 ? 512u : 1024u;
}

struct CloRefineStats {
    double finalRmseDb = std::numeric_limits<double>::infinity();
    std::size_t analysisFrames = 0;
};

struct CloRefineConfig {
    CloDestination destination = CloDestination::Gp200;
    // Optional refinement test audio. When provided, its FIRST 20 seconds are
    // adapted to mono PCM16 44.1 kHz and inserted as the 20-second tail of a
    // second, otherwise-identical conversion stimulus. That exact stimulus is
    // rendered through both the verified NAM Full path and the destination-size CLO,
    // so Tone Match compares the same performance through both models.
    fs::path referenceWav;
};

using RefineStatusCallback = std::function<void(const std::wstring&)>;

// Always apply one direct Tone Match correction on final B1024/B512 (A stays 128).
// No confidence calculation/weighting, smoothing or candidate selection.
// The final CLO is serialized directly, with no later DSP or truncation.
bool refineCloBOnly(const fs::path& inputClo2048,
                    const fs::path& stimulusWav,
                    const fs::path& targetWav,
                    const fs::path& outputClo,
                    const CloRefineConfig& config,
                    std::string& error,
                    const RefineStatusCallback& status = {},
                    CloRefineStats* stats = nullptr);

} // namespace ntc
