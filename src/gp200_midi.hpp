#pragma once

#include "gp200_clo_upload.hpp"

#include <functional>
#include <string>

namespace ntc::gp200 {

struct MidiDetection {
    bool inputFound = false;
    bool outputFound = false;
    unsigned int inputId = 0;
    unsigned int outputId = 0;
    std::wstring inputName;
    std::wstring outputName;
};

MidiDetection detectGp200Midi();
std::wstring describeDetection(const MidiDetection& detection);

struct UploadResult {
    bool ok = false;
    std::wstring message;
};

using UploadProgress = std::function<void(int currentBlock, int totalBlocks, const std::wstring& status)>;

UploadResult uploadCloToGp200(const std::filesystem::path& cloFile,
                              int globalSlot,
                              UploadProgress progress = {});

} // namespace ntc::gp200
