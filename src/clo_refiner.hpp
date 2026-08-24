#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace ntc {
namespace fs = std::filesystem;

struct CloRefineConfig {
    bool enabled = false;
    int passes = 4;
    // Optional refinement test audio. When provided, its FIRST 20 seconds are
    // adapted to mono PCM16 44.1 kHz and inserted as the 20-second tail of a
    // second, otherwise-identical conversion stimulus. That exact stimulus is
    // rendered through both the verified NAM Full path and the original CLO,
    // so Tone Match compares the same performance through both models.
    fs::path referenceWav;
    // Optional persistent diagnostics for Tone Match. When set, the exact
    // 20-second NAM target, CLO source and generated minimum-phase IR used
    // by the comparison are written here for A/B verification.
    fs::path debugDirectory;
};

using RefineStatusCallback = std::function<void(const std::wstring&)>;

// CAB Tone Match refinement on the final 20 seconds.
// The 2048-sample minimum-phase IR stays in memory and is applied directly to Block B.
// The analysis/solver itself is unchanged by the diagnostic cleanup.
bool refineCloBOnly(const fs::path& inputClo2048,
                    const fs::path& stimulusWav,
                    const fs::path& targetWav,
                    const fs::path& outputClo2048,
                    const CloRefineConfig& config,
                    std::string& error,
                    const RefineStatusCallback& status = {});

} // namespace ntc
