#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace ntc {

class NamPreviewPlayer {
public:
    NamPreviewPlayer();
    ~NamPreviewPlayer();

    NamPreviewPlayer(const NamPreviewPlayer&) = delete;
    NamPreviewPlayer& operator=(const NamPreviewPlayer&) = delete;

    // Loads the source WAV into RAM, adapts it to the NAM sample rate and loads
    // the NAM DSP. No processed preview WAV is generated.
    bool load(const std::filesystem::path& namPath,
              const std::filesystem::path& sourceWav,
              const std::filesystem::path& irWav,
              std::string& error);

    // Backward-compatible overload: preview without a cabinet IR.
    bool load(const std::filesystem::path& namPath,
              const std::filesystem::path& sourceWav,
              std::string& error) {
        return load(namPath, sourceWav, std::filesystem::path{}, error);
    }

    // Starts block-by-block realtime playback through the already loaded NAM.
    bool play(std::string& error);
    void stop();

    bool ready() const;
    bool playing() const;
    int sampleRate() const;
    bool irLoaded() const;
    int irOriginalSampleRate() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ntc
