#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ntc::gp5 {

struct CloUploadData {
    std::vector<std::vector<std::uint8_t>> chunks;
    std::string displayName;
    int slot = -1; // zero-based GP-5 SnapTone slot: 50..79 (visible 51..80)
    std::size_t compactCloBytes = 0;
};

// Build the GP-5 transfer reconstructed from Valeton Suite captures:
//   74-byte wrapper + compact VTSI A128/B512 (0x0A88 bytes)
// split into command-0x92 frames with 19-byte payloads.
bool buildCloUpload(const std::filesystem::path& cloFile,
                    int slot,
                    CloUploadData& result,
                    std::wstring& error);

} // namespace ntc::gp5
