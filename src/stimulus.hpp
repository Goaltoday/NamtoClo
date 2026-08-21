#pragma once

#include <filesystem>
#include <string>

namespace ntc {

namespace fs = std::filesystem;

enum class TailMode {
    PresetAudio = 0,
    RecordedAudio
};

struct StimulusConfig {
    TailMode tailMode = TailMode::PresetAudio;
    fs::path recordedAudio;
};

const wchar_t* tailModeDisplayName(TailMode mode);

// Builds the 70-second conversion stimulus from the official/original
// nam_input_wav.wav placed next to the application executable.
//
// Samples 0..50 s always come from nam_input_wav.wav. The final 20 s are
// either samples 50..70 s from that same file (Original Preset Audio) or a
// user-selected Recorded Audio WAV adapted to mono PCM16 / 44.1 kHz and
// exactly 20 seconds. The 600-sample guard used by the native converter is
// appended unchanged.
bool buildStimulus(const fs::path& originalStimulus,
                   const StimulusConfig& config,
                   const fs::path& destination,
                   std::string& error);

} // namespace ntc
