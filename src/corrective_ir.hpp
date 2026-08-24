#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ntc {

namespace fs = std::filesystem;

struct CorrectiveIrConfig {
    bool enabled = false;
    fs::path wav;
};

struct CorrectiveIrStats {
    double originalRms = 0.0;
    double convolvedRms = 0.0;
    double rmsGain = 1.0;
    double rmsGainDb = 0.0;
    double postGainDb = -6.0;
    double totalGainDb = -6.0;
};


// Decode the 44.1 kHz mono corrective IR using the same WAV rules as the CLO
// correction path. The returned samples are not normalized or gain-scaled.
bool loadCorrectiveIrSamples(const fs::path& correctiveWav,
                             std::vector<float>& correctiveIr,
                             std::string& error);

bool applyCorrectiveIrToClo(const fs::path& sourceClo,
                            const std::vector<float>& correctiveIr,
                            const fs::path& destinationClo,
                            CorrectiveIrStats& stats,
                            std::string& error,
                            double postCorrectionDb = -6.0);

bool applyCorrectiveIrToClo(const fs::path& sourceClo,
                            const fs::path& correctiveWav,
                            const fs::path& destinationClo,
                            CorrectiveIrStats& stats,
                            std::string& error,
                            double postCorrectionDb = -6.0);

} // namespace ntc
