#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ntc::gp200 {

struct CloUploadData {
    std::vector<std::uint8_t> prepareMessage;
    std::vector<std::vector<std::uint8_t>> chunks;
    std::string displayName;
    int globalSlot = -1;
};

// Global slots: AMP 1..5 = 0..4, DIST 1..5 = 5..9.
bool buildCloUpload(const std::filesystem::path& cloFile,
                    int globalSlot,
                    CloUploadData& result,
                    std::wstring& error);

} // namespace ntc::gp200
