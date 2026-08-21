#include "corrective_ir.hpp"
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

constexpr std::size_t kPhysicalSize = 0x2288;
constexpr std::size_t kHeaderSize = 0x88;
constexpr std::size_t kBlockACount = 128;
constexpr std::size_t kBlockBCount = 2048;
constexpr std::size_t kBlockBOffset = kHeaderSize + kBlockACount * sizeof(float); // 0x288
constexpr std::uint32_t kExpectedSampleRate = 44100;
constexpr std::uint16_t kWaveFormatPcm = 0x0001;
constexpr std::uint16_t kWaveFormatIeeeFloat = 0x0003;
constexpr std::uint16_t kWaveFormatExtensible = 0xFFFE;

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

bool readWaveFile(const fs::path& path, WavData& wav, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Cannot open Corrective IR WAV: " + pathToUtf8(path);
        return false;
    }

    std::array<std::uint8_t, 12> riff{};
    in.read(reinterpret_cast<char*>(riff.data()), static_cast<std::streamsize>(riff.size()));
    if (in.gcount() != static_cast<std::streamsize>(riff.size())
        || std::memcmp(riff.data(), "RIFF", 4) != 0
        || std::memcmp(riff.data() + 8, "WAVE", 4) != 0) {
        error = "Corrective IR is not a valid RIFF/WAVE file: " + pathToUtf8(path);
        return false;
    }

    bool haveFmt = false;
    bool haveData = false;
    while (in && !(haveFmt && haveData)) {
        std::array<std::uint8_t, 8> chunkHeader{};
        in.read(reinterpret_cast<char*>(chunkHeader.data()), static_cast<std::streamsize>(chunkHeader.size()));
        if (in.gcount() == 0) break;
        if (in.gcount() != static_cast<std::streamsize>(chunkHeader.size())) {
            error = "Truncated Corrective IR WAV chunk header.";
            return false;
        }

        const std::uint32_t chunkSize = readLe32(chunkHeader.data() + 4);
        const bool isFmt = std::memcmp(chunkHeader.data(), "fmt ", 4) == 0;
        const bool isData = std::memcmp(chunkHeader.data(), "data", 4) == 0;

        if (isFmt) {
            if (chunkSize < 16) {
                error = "Invalid fmt chunk in Corrective IR WAV.";
                return false;
            }
            std::vector<std::uint8_t> fmt(chunkSize);
            in.read(reinterpret_cast<char*>(fmt.data()), static_cast<std::streamsize>(fmt.size()));
            if (in.gcount() != static_cast<std::streamsize>(fmt.size())) {
                error = "Truncated fmt chunk in Corrective IR WAV.";
                return false;
            }

            wav.format = readLe16(fmt.data());
            wav.channels = readLe16(fmt.data() + 2);
            wav.sampleRate = readLe32(fmt.data() + 4);
            wav.blockAlign = readLe16(fmt.data() + 12);
            wav.bitsPerSample = readLe16(fmt.data() + 14);

            if (wav.format == kWaveFormatExtensible) {
                if (fmt.size() < 40 || readLe16(fmt.data() + 16) < 22) {
                    error = "Unsupported WAVE_FORMAT_EXTENSIBLE header in Corrective IR WAV.";
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
                    error = "Truncated data chunk in Corrective IR WAV.";
                    return false;
                }
            }
            haveData = true;
        } else {
            in.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
            if (!in) {
                error = "Invalid chunk size in Corrective IR WAV.";
                return false;
            }
        }

        if ((chunkSize & 1u) != 0u) in.seekg(1, std::ios::cur);
    }

    if (!haveFmt || !haveData) {
        error = "Corrective IR WAV is missing fmt or data chunk.";
        return false;
    }
    if (wav.channels == 0 || wav.channels > 2 || wav.sampleRate == 0 || wav.blockAlign == 0) {
        error = "Corrective IR WAV must be mono or stereo.";
        return false;
    }
    if (wav.sampleRate != kExpectedSampleRate) {
        error = "Corrective IR must be 44.1 kHz.";
        return false;
    }
    if (wav.data.empty() || (wav.data.size() % wav.blockAlign) != 0u) {
        error = "Corrective IR WAV contains no complete audio frames.";
        return false;
    }
    return true;
}

double decodeSample(const std::uint8_t* p,
                    std::uint16_t format,
                    std::uint16_t bitsPerSample,
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

bool decodeCorrectiveIr(const fs::path& path, std::vector<double>& mono, std::string& error) {
    WavData wav;
    if (!readWaveFile(path, wav, error)) return false;

    const bool supportedPcm = wav.format == kWaveFormatPcm
        && (wav.bitsPerSample == 8 || wav.bitsPerSample == 16
            || wav.bitsPerSample == 24 || wav.bitsPerSample == 32);
    const bool supportedFloat = wav.format == kWaveFormatIeeeFloat
        && (wav.bitsPerSample == 32 || wav.bitsPerSample == 64);
    if (!supportedPcm && !supportedFloat) {
        error = "Unsupported Corrective IR WAV encoding. Use PCM 8/16/24/32-bit or float 32/64-bit.";
        return false;
    }

    const std::uint16_t bytesPerSample = static_cast<std::uint16_t>((wav.bitsPerSample + 7u) / 8u);
    const std::uint16_t minimumBlockAlign = static_cast<std::uint16_t>(bytesPerSample * wav.channels);
    if (bytesPerSample == 0 || wav.blockAlign < minimumBlockAlign) {
        error = "Invalid block alignment in Corrective IR WAV.";
        return false;
    }

    const std::size_t frames = wav.data.size() / wav.blockAlign;
    if (frames == 0) {
        error = "Corrective IR WAV is empty.";
        return false;
    }

    mono.resize(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::uint8_t* framePtr = wav.data.data() + frame * wav.blockAlign;
        double sum = 0.0;
        for (std::uint16_t channel = 0; channel < wav.channels; ++channel) {
            bool ok = false;
            const double sample = decodeSample(framePtr + static_cast<std::size_t>(channel) * bytesPerSample,
                                               wav.format, wav.bitsPerSample, ok);
            if (!ok) {
                error = "Unsupported sample format in Corrective IR WAV.";
                return false;
            }
            sum += sample;
        }
        mono[frame] = sum / static_cast<double>(wav.channels);
    }
    return true;
}

std::uint16_t crc16Modbus(const std::uint8_t* data, std::size_t size) {
    std::uint16_t crc = 0xFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint16_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? static_cast<std::uint16_t>((crc >> 1) ^ 0xA001u)
                             : static_cast<std::uint16_t>(crc >> 1);
        }
    }
    return crc;
}

double rms(const std::array<double, kBlockBCount>& values) {
    long double sum = 0.0L;
    for (const double value : values) sum += static_cast<long double>(value) * value;
    return std::sqrt(static_cast<double>(sum / static_cast<long double>(values.size())));
}

} // namespace

bool applyCorrectiveIrToClo(const fs::path& sourceClo,
                            const std::vector<float>& correctiveIr,
                            const fs::path& destinationClo,
                            CorrectiveIrStats& stats,
                            std::string& error,
                            double postCorrectionDb) {
    stats = {};

    std::vector<std::uint8_t> data;
    if (!readFileBytes(sourceClo, data, error)) return false;
    if (data.size() != kPhysicalSize) {
        error = "Source Ampero CLO is not exactly 0x2288 bytes.";
        return false;
    }
    if (std::memcmp(data.data(), "VTSI", 4) != 0) {
        error = "Source Ampero CLO magic is not VTSI.";
        return false;
    }
    if (readLe32(data.data() + 0x04) != 0x2288u
        || readLe32(data.data() + 0x14) != 0x2200u
        || readLe32(data.data() + 0x80) != 0x80u
        || readLe32(data.data() + 0x84) != 0x800u) {
        error = "Source CLO is not the expected 128 + 2048 Ampero VTSI structure.";
        return false;
    }

    if (correctiveIr.empty()) {
        error = "Corrective IR has no samples.";
        return false;
    }
    std::vector<double> ir;
    ir.reserve(correctiveIr.size());
    for (const float sample : correctiveIr) {
        if (!std::isfinite(sample)) {
            error = "Corrective IR contains a non-finite sample.";
            return false;
        }
        ir.push_back(static_cast<double>(sample));
    }

    std::array<double, kBlockBCount> original{};
    for (std::size_t i = 0; i < kBlockBCount; ++i) {
        float value = 0.0f;
        std::memcpy(&value, data.data() + kBlockBOffset + i * sizeof(float), sizeof(value));
        if (!std::isfinite(value)) {
            error = "Source CLO contains a non-finite Block B value.";
            return false;
        }
        original[i] = static_cast<double>(value);
    }

    std::array<double, kBlockBCount> corrected{};
    for (std::size_t n = 0; n < kBlockBCount; ++n) {
        const std::size_t maxK = std::min<std::size_t>(n, ir.size() - 1u);
        long double sum = 0.0L;
        for (std::size_t k = 0; k <= maxK; ++k) {
            sum += static_cast<long double>(original[n - k]) * ir[k];
        }
        corrected[n] = static_cast<double>(sum);
    }

    stats.originalRms = rms(original);
    stats.convolvedRms = rms(corrected);
    if (!std::isfinite(stats.originalRms) || stats.originalRms <= 1e-20) {
        error = "Source CLO Block B is silent or invalid.";
        return false;
    }
    if (!std::isfinite(stats.convolvedRms) || stats.convolvedRms <= 1e-20) {
        error = "Corrective IR produced a silent Block B.";
        return false;
    }

    stats.rmsGain = stats.originalRms / stats.convolvedRms;
    stats.rmsGainDb = 20.0 * std::log10(stats.rmsGain);
    const double postGain = std::pow(10.0, postCorrectionDb / 20.0);
    const double finalGain = stats.rmsGain * postGain;
    stats.postGainDb = postCorrectionDb;
    stats.totalGainDb = stats.rmsGainDb + postCorrectionDb;

    if (!std::isfinite(finalGain)) {
        error = "Corrective IR normalization produced an invalid gain.";
        return false;
    }

    for (std::size_t i = 0; i < kBlockBCount; ++i) {
        const double scaled = corrected[i] * finalGain;
        if (!std::isfinite(scaled)
            || scaled > static_cast<double>(std::numeric_limits<float>::max())
            || scaled < -static_cast<double>(std::numeric_limits<float>::max())) {
            error = "Corrective IR produced an out-of-range Block B value.";
            return false;
        }
        const float value = static_cast<float>(scaled);
        std::memcpy(data.data() + kBlockBOffset + i * sizeof(float), &value, sizeof(value));
    }

    // Match the existing converter's confirmed HTUSBTools CRC representation:
    // CRC16/MODBUS over [0x0C, declaredSize), stored high byte at 0x08.
    const std::uint16_t crc = crc16Modbus(data.data() + 0x0C, 0x2288u - 0x0Cu);
    data[0x08] = static_cast<std::uint8_t>((crc >> 8) & 0xFFu);
    data[0x09] = static_cast<std::uint8_t>(crc & 0xFFu);

    return writeFileBytes(destinationClo, data.data(), data.size(), error);
}

bool applyCorrectiveIrToClo(const fs::path& sourceClo,
                            const fs::path& correctiveWav,
                            const fs::path& destinationClo,
                            CorrectiveIrStats& stats,
                            std::string& error,
                            double postCorrectionDb) {
    std::vector<double> decoded;
    if (!decodeCorrectiveIr(correctiveWav, decoded, error)) return false;
    std::vector<float> ir;
    ir.reserve(decoded.size());
    for (const double sample : decoded) {
        if (!std::isfinite(sample)
            || sample > static_cast<double>(std::numeric_limits<float>::max())
            || sample < -static_cast<double>(std::numeric_limits<float>::max())) {
            error = "Corrective IR WAV contains an invalid sample.";
            return false;
        }
        ir.push_back(static_cast<float>(sample));
    }
    return applyCorrectiveIrToClo(sourceClo, ir, destinationClo, stats, error, postCorrectionDb);
}

} // namespace ntc
