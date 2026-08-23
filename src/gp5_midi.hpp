#pragma once

#include "gp5_clo_upload.hpp"

#include <filesystem>
#include <functional>
#include <string>

namespace ntc::gp5 {

struct MidiDetection {
    bool inputFound = false;
    bool outputFound = false;
    unsigned int inputId = 0;
    unsigned int outputId = 0;
    std::wstring inputName;
    std::wstring outputName;
};

MidiDetection detectGp5Midi();
std::wstring describeDetection(const MidiDetection& detection);

struct UploadResult {
    bool ok = false;
    std::wstring message;
};

using UploadProgress = std::function<void(int currentBlock, int totalBlocks, const std::wstring& status)>;

UploadResult uploadCloToGp5(const std::filesystem::path& cloFile,
                            int slot,
                            UploadProgress progress = {});

} // namespace ntc::gp5
