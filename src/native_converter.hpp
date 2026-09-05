#pragma once

#include "stimulus.hpp"
#include "corrective_ir.hpp"
#include "clo_refiner.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ntc {

namespace fs = std::filesystem;

struct ConversionResult {
    bool ok = false;
    std::string error;
    fs::path inputNam;
    fs::path outputClo;
    CloDestination destination = CloDestination::Gp200;
    CloRefineStats toneMatch;
};

struct BatchConversionResult {
    bool ok = false;
    std::size_t total = 0;
    std::size_t succeeded = 0;
    std::size_t failed = 0;
    std::vector<ConversionResult> items;
};

using StatusCallback = std::function<void(const std::wstring&)>;

struct NativeConverterConfig {
    int blockSize = 1024;
};

ConversionResult convertNamToClo(const fs::path& inputNam,
                                 const fs::path& outputDirectory,
                                 StimulusConfig stimulus = {},
                                 CorrectiveIrConfig correction = {},
                                 CloRefineConfig refine = {},
                                 NativeConverterConfig converter = {},
                                 const StatusCallback& status = {});

BatchConversionResult convertNamFolderToClo(const fs::path& inputDirectory,
                                            const fs::path& outputDirectory,
                                            StimulusConfig stimulus = {},
                                            CorrectiveIrConfig correction = {},
                                            CloRefineConfig refine = {},
                                            NativeConverterConfig converter = {},
                                            const StatusCallback& status = {});

// Renders an arbitrary WAV through the selected NAM for the Tone3000 preview player.
// The input is downmixed to mono, resampled to the NAM expected sample rate and
// written as a mono PCM16 WAV suitable for Win32 PlaySound.
bool renderNamPreviewToWav(const fs::path& inputNam,
                           const fs::path& inputWav,
                           const fs::path& outputWav,
                           std::string& error);

} // namespace ntc
