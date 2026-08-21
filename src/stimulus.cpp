#include "stimulus.hpp"
#include "common.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

namespace ntc {
namespace {

constexpr std::uint32_t kExpectedSampleRate = 44100;
constexpr std::uint16_t kExpectedBitsPerSample = 16;
constexpr std::uint16_t kExpectedSourceChannels = 1;
constexpr std::uint64_t kBaseFrames = 50ull * kExpectedSampleRate;
constexpr std::uint64_t kTailFrames = 20ull * kExpectedSampleRate;
constexpr std::size_t kSoundClonePaddingFrames = 600;
constexpr std::uint16_t kWaveFormatPcm = 0x0001;
constexpr std::uint16_t kWaveFormatIeeeFloat = 0x0003;
constexpr std::uint16_t kWaveFormatExtensible = 0xFFFE;

struct Pcm16MonoWav {
    std::vector<std::int16_t> samples;
};

struct WavData {
    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t blockAlign = 0;
    std::uint16_t bitsPerSample = 0;
    std::vector<std::uint8_t> data;
};

std::uint16_t readLe16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0])
         | static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8);
}

std::uint32_t readLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0])
         | (static_cast<std::uint32_t>(p[1]) << 8)
         | (static_cast<std::uint32_t>(p[2]) << 16)
         | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t readLe64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(readLe32(p))
         | (static_cast<std::uint64_t>(readLe32(p + 4)) << 32);
}

void writeLe16(std::ostream& out, std::uint16_t value) {
    const std::array<char, 2> b = {
        static_cast<char>(value & 0xFFu),
        static_cast<char>((value >> 8) & 0xFFu)
    };
    out.write(b.data(), static_cast<std::streamsize>(b.size()));
}

void writeLe32(std::ostream& out, std::uint32_t value) {
    const std::array<char, 4> b = {
        static_cast<char>(value & 0xFFu),
        static_cast<char>((value >> 8) & 0xFFu),
        static_cast<char>((value >> 16) & 0xFFu),
        static_cast<char>((value >> 24) & 0xFFu)
    };
    out.write(b.data(), static_cast<std::streamsize>(b.size()));
}

bool readWaveFile(const fs::path& path, WavData& wav, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Cannot open WAV: " + pathToUtf8(path);
        return false;
    }

    std::array<std::uint8_t, 12> riff{};
    in.read(reinterpret_cast<char*>(riff.data()), static_cast<std::streamsize>(riff.size()));
    if (in.gcount() != static_cast<std::streamsize>(riff.size())
        || std::memcmp(riff.data(), "RIFF", 4) != 0
        || std::memcmp(riff.data() + 8, "WAVE", 4) != 0) {
        error = "Not a valid RIFF/WAVE file: " + pathToUtf8(path);
        return false;
    }

    bool haveFmt = false;
    bool haveData = false;

    while (in && !(haveFmt && haveData)) {
        std::array<std::uint8_t, 8> chunkHeader{};
        in.read(reinterpret_cast<char*>(chunkHeader.data()), static_cast<std::streamsize>(chunkHeader.size()));
        if (in.gcount() == 0) break;
        if (in.gcount() != static_cast<std::streamsize>(chunkHeader.size())) {
            error = "Truncated WAV chunk header: " + pathToUtf8(path);
            return false;
        }

        const std::uint32_t chunkSize = readLe32(chunkHeader.data() + 4);
        const bool isFmt = std::memcmp(chunkHeader.data(), "fmt ", 4) == 0;
        const bool isData = std::memcmp(chunkHeader.data(), "data", 4) == 0;

        if (isFmt) {
            if (chunkSize < 16) {
                error = "Invalid fmt chunk in WAV: " + pathToUtf8(path);
                return false;
            }
            std::vector<std::uint8_t> fmt(chunkSize);
            in.read(reinterpret_cast<char*>(fmt.data()), static_cast<std::streamsize>(fmt.size()));
            if (in.gcount() != static_cast<std::streamsize>(fmt.size())) {
                error = "Truncated fmt chunk in WAV: " + pathToUtf8(path);
                return false;
            }

            wav.format = readLe16(fmt.data());
            wav.channels = readLe16(fmt.data() + 2);
            wav.sampleRate = readLe32(fmt.data() + 4);
            wav.blockAlign = readLe16(fmt.data() + 12);
            wav.bitsPerSample = readLe16(fmt.data() + 14);

            // WAVE_FORMAT_EXTENSIBLE stores the real PCM/float format in the
            // SubFormat GUID. Its first WORD is the classic format tag.
            if (wav.format == kWaveFormatExtensible) {
                if (fmt.size() < 40 || readLe16(fmt.data() + 16) < 22) {
                    error = "Unsupported WAVE_FORMAT_EXTENSIBLE header: " + pathToUtf8(path);
                    return false;
                }
                wav.format = readLe16(fmt.data() + 24);
            }
            haveFmt = true;
        } else if (isData) {
            wav.data.resize(chunkSize);
            if (!wav.data.empty()) {
                in.read(reinterpret_cast<char*>(wav.data.data()), static_cast<std::streamsize>(wav.data.size()));
                if (in.gcount() != static_cast<std::streamsize>(wav.data.size())) {
                    error = "Truncated data chunk in WAV: " + pathToUtf8(path);
                    return false;
                }
            }
            haveData = true;
        } else {
            in.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
            if (!in) {
                error = "Invalid WAV chunk size: " + pathToUtf8(path);
                return false;
            }
        }

        if ((chunkSize & 1u) != 0u) in.seekg(1, std::ios::cur);
    }

    if (!haveFmt || !haveData) {
        error = "WAV is missing fmt or data chunk: " + pathToUtf8(path);
        return false;
    }
    if (wav.channels == 0 || wav.sampleRate == 0 || wav.blockAlign == 0) {
        error = "Invalid WAV format values: " + pathToUtf8(path);
        return false;
    }
    if ((wav.data.size() % wav.blockAlign) != 0u) {
        error = "WAV data is not aligned to complete audio frames: " + pathToUtf8(path);
        return false;
    }
    return true;
}

bool readPcm16Mono44100(const fs::path& path,
                        std::uint64_t expectedFrames,
                        Pcm16MonoWav& wav,
                        std::string& error) {
    WavData source;
    if (!readWaveFile(path, source, error)) return false;

    if (source.format != kWaveFormatPcm || source.channels != kExpectedSourceChannels
        || source.sampleRate != kExpectedSampleRate || source.bitsPerSample != kExpectedBitsPerSample
        || source.blockAlign != 2) {
        error = "Expected mono PCM16 44.1 kHz WAV: " + pathToUtf8(path);
        return false;
    }

    const std::uint64_t frames = source.data.size() / source.blockAlign;
    if (frames != expectedFrames) {
        error = "Unexpected duration for " + pathToUtf8(path)
              + ". Expected exactly " + std::to_string(expectedFrames)
              + " samples, got " + std::to_string(frames) + ".";
        return false;
    }

    wav.samples.resize(static_cast<std::size_t>(frames));
    for (std::size_t i = 0; i < wav.samples.size(); ++i) {
        wav.samples[i] = static_cast<std::int16_t>(readLe16(source.data.data() + i * 2));
    }
    return true;
}

double decodeSample(const std::uint8_t* p,
                    const std::uint16_t format,
                    const std::uint16_t bitsPerSample,
                    bool& ok) {
    ok = true;
    if (format == kWaveFormatPcm) {
        switch (bitsPerSample) {
        case 8:
            return (static_cast<int>(p[0]) - 128) / 128.0;
        case 16:
            return static_cast<std::int16_t>(readLe16(p)) / 32768.0;
        case 24: {
            std::int32_t value = static_cast<std::int32_t>(p[0])
                               | (static_cast<std::int32_t>(p[1]) << 8)
                               | (static_cast<std::int32_t>(p[2]) << 16);
            if ((value & 0x00800000) != 0) value |= static_cast<std::int32_t>(0xFF000000);
            return static_cast<double>(value) / 8388608.0;
        }
        case 32:
            return static_cast<std::int32_t>(readLe32(p)) / 2147483648.0;
        default:
            ok = false;
            return 0.0;
        }
    }

    if (format == kWaveFormatIeeeFloat) {
        if (bitsPerSample == 32) {
            const std::uint32_t raw = readLe32(p);
            float value = 0.0f;
            std::memcpy(&value, &raw, sizeof(value));
            if (!std::isfinite(value)) value = 0.0f;
            return static_cast<double>(value);
        }
        if (bitsPerSample == 64) {
            const std::uint64_t raw = readLe64(p);
            double value = 0.0;
            std::memcpy(&value, &raw, sizeof(value));
            if (!std::isfinite(value)) value = 0.0;
            return value;
        }
    }

    ok = false;
    return 0.0;
}

std::int16_t floatToPcm16(double value) {
    value = std::clamp(value, -1.0, 1.0);
    long sample = std::lround(value * 32768.0);
    sample = std::clamp(sample, -32768L, 32767L);
    return static_cast<std::int16_t>(sample);
}

bool readAdaptedAudio(const fs::path& path,
                      const std::uint64_t targetFrames,
                      const char* roleName,
                      Pcm16MonoWav& wav,
                      std::string& error) {
    WavData source;
    if (!readWaveFile(path, source, error)) return false;

    const bool formatSupported =
        (source.format == kWaveFormatPcm
            && (source.bitsPerSample == 8 || source.bitsPerSample == 16
                || source.bitsPerSample == 24 || source.bitsPerSample == 32))
        || (source.format == kWaveFormatIeeeFloat
            && (source.bitsPerSample == 32 || source.bitsPerSample == 64));
    if (!formatSupported) {
        error = std::string(roleName) + " WAV must use PCM 8/16/24/32-bit or IEEE float 32/64-bit audio.";
        return false;
    }

    const std::uint16_t bytesPerSample = static_cast<std::uint16_t>((source.bitsPerSample + 7u) / 8u);
    const std::uint32_t expectedBlockAlign = static_cast<std::uint32_t>(bytesPerSample) * source.channels;
    if (bytesPerSample == 0 || source.blockAlign < expectedBlockAlign) {
        error = "Unsupported WAV block alignment: " + pathToUtf8(path);
        return false;
    }

    const std::uint64_t sourceFrames = source.data.size() / source.blockAlign;
    if (sourceFrames == 0) {
        error = std::string(roleName) + " WAV contains no audio samples: " + pathToUtf8(path);
        return false;
    }

    std::vector<double> mono(static_cast<std::size_t>(sourceFrames), 0.0);
    for (std::size_t frame = 0; frame < mono.size(); ++frame) {
        const std::uint8_t* framePtr = source.data.data() + frame * source.blockAlign;
        double sum = 0.0;
        for (std::uint16_t channel = 0; channel < source.channels; ++channel) {
            bool ok = false;
            const double sample = decodeSample(framePtr + static_cast<std::size_t>(channel) * bytesPerSample,
                                               source.format, source.bitsPerSample, ok);
            if (!ok) {
                error = "Unsupported sample format in " + std::string(roleName) + " WAV: " + pathToUtf8(path);
                return false;
            }
            sum += sample;
        }
        mono[frame] = sum / source.channels;
    }

    // User-provided audio is always adapted to the exact requested duration
    // at 44.1 kHz. Longer files are trimmed; shorter files are zero-padded.
    wav.samples.assign(static_cast<std::size_t>(targetFrames), static_cast<std::int16_t>(0));

    if (source.sampleRate == kExpectedSampleRate) {
        const std::size_t framesToCopy = std::min(wav.samples.size(), mono.size());
        for (std::size_t i = 0; i < framesToCopy; ++i) {
            wav.samples[i] = floatToPcm16(mono[i]);
        }
        return true;
    }

    // Linear interpolation adapts user-provided audio to 44.1 kHz. Samples
    // beyond the end of a short file remain zero; samples beyond the target
    // duration of a long file are ignored.
    const double ratio = static_cast<double>(source.sampleRate) / kExpectedSampleRate;
    for (std::size_t i = 0; i < wav.samples.size(); ++i) {
        const double sourcePosition = static_cast<double>(i) * ratio;
        if (sourcePosition >= static_cast<double>(mono.size())) break;

        const std::size_t i0 = static_cast<std::size_t>(sourcePosition);
        const std::size_t i1 = std::min(i0 + 1, mono.size() - 1);
        const double fraction = sourcePosition - static_cast<double>(i0);
        const double sample = mono[i0] + (mono[i1] - mono[i0]) * fraction;
        wav.samples[i] = floatToPcm16(sample);
    }
    return true;
}

bool writePcm16Wav(const fs::path& path,
                   const std::vector<std::int16_t>& monoSamples,
                   std::string& error) {
    constexpr std::uint16_t channels = 1;
    constexpr std::uint32_t bytesPerSample = kExpectedBitsPerSample / 8;
    const std::uint64_t dataBytes64 = static_cast<std::uint64_t>(monoSamples.size())
                                    * channels * bytesPerSample;
    if (dataBytes64 > std::numeric_limits<std::uint32_t>::max()) {
        error = "Generated stimulus WAV is too large.";
        return false;
    }
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(dataBytes64);
    const std::uint32_t riffSize = 36u + dataBytes;
    const std::uint32_t byteRate = kExpectedSampleRate * channels * bytesPerSample;
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * bytesPerSample);

    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            error = "Cannot create stimulus directory: " + ec.message();
            return false;
        }
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Cannot create stimulus WAV: " + pathToUtf8(path);
        return false;
    }

    out.write("RIFF", 4);
    writeLe32(out, riffSize);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeLe32(out, 16);
    writeLe16(out, 1);
    writeLe16(out, channels);
    writeLe32(out, kExpectedSampleRate);
    writeLe32(out, byteRate);
    writeLe16(out, blockAlign);
    writeLe16(out, kExpectedBitsPerSample);
    out.write("data", 4);
    writeLe32(out, dataBytes);

    for (const std::int16_t sample : monoSamples) {
        writeLe16(out, static_cast<std::uint16_t>(sample));
    }

    if (!out) {
        error = "Failed while writing stimulus WAV: " + pathToUtf8(path);
        return false;
    }
    return true;
}

bool existsFile(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::exists(p, ec) && !ec && fs::is_regular_file(p, ec) && !ec;
}

} // namespace

const wchar_t* tailModeDisplayName(const TailMode mode) {
    switch (mode) {
    case TailMode::PresetAudio:   return L"Original Preset Audio";
    case TailMode::RecordedAudio: return L"Recorded Audio";
    }
    return L"Unknown";
}

bool buildStimulus(const fs::path& originalStimulus,
                   const StimulusConfig& config,
                   const fs::path& destination,
                   std::string& error) {
    if (!existsFile(originalStimulus)) {
        error = "Missing nam_input_wav.wav next to the executable.";
        return false;
    }

    Pcm16MonoWav base;
    if (!readAdaptedAudio(originalStimulus, kBaseFrames, "Original stimulus", base, error)) return false;

    Pcm16MonoWav tail;
    if (config.tailMode == TailMode::PresetAudio) {
        Pcm16MonoWav full;
        if (!readAdaptedAudio(originalStimulus, kBaseFrames + kTailFrames,
                              "Original stimulus/tail", full, error)) return false;
        if (full.samples.size() < kBaseFrames + kTailFrames) {
            error = "nam_input_wav.wav is shorter than the required 70.000 seconds.";
            return false;
        }
        tail.samples.assign(full.samples.begin() + static_cast<std::ptrdiff_t>(kBaseFrames),
                            full.samples.begin() + static_cast<std::ptrdiff_t>(kBaseFrames + kTailFrames));
    } else {
        if (!existsFile(config.recordedAudio)) {
            error = "Select a valid Recorded Audio WAV file.";
            return false;
        }
        if (!readAdaptedAudio(config.recordedAudio, kTailFrames, "Recorded Audio", tail, error)) return false;
    }

    std::vector<std::int16_t> combined;
    combined.reserve(base.samples.size() + tail.samples.size() + kSoundClonePaddingFrames);
    combined.insert(combined.end(), base.samples.begin(), base.samples.end());
    combined.insert(combined.end(), tail.samples.begin(), tail.samples.end());
    combined.insert(combined.end(), kSoundClonePaddingFrames, static_cast<std::int16_t>(0));
    return writePcm16Wav(destination, combined, error);
}

} // namespace ntc
