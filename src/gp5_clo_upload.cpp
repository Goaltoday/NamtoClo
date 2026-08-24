#include "gp5_clo_upload.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <windows.h>

namespace ntc::gp5 {
namespace {
constexpr std::size_t headerBytes = 0x88;
constexpr std::size_t blockATaps = 128;
constexpr std::size_t gp5BlockBTaps = 512;
constexpr std::size_t gp5DeclaredBytes = 0x0A88;
constexpr std::uint32_t gp5PayloadBytes = 0x0A00;
constexpr std::size_t wrapperBytes = 74;
constexpr std::size_t chunkPayloadBytes = 19;

constexpr std::size_t declaredOffset = 0x04;
constexpr std::size_t crcOffset = 0x08;
constexpr std::size_t payloadOffset = 0x14;
constexpr std::size_t startAOffset = 0x78;
constexpr std::size_t countAOffset = 0x7C;
constexpr std::size_t startBOffset = 0x80;
constexpr std::size_t countBOffset = 0x84;
constexpr std::size_t crcDataOffset = 0x0C;

std::uint32_t readLe32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0])
         | (static_cast<std::uint32_t>(data[1]) << 8)
         | (static_cast<std::uint32_t>(data[2]) << 16)
         | (static_cast<std::uint32_t>(data[3]) << 24);
}

void writeLe32(std::uint8_t* data, std::uint32_t value) {
    data[0] = static_cast<std::uint8_t>(value & 0xFFu);
    data[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    data[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
    data[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

std::uint16_t crc16Modbus(const std::uint8_t* data, std::size_t size) {
    std::uint16_t crc = 0xFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint16_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) != 0u
                ? static_cast<std::uint16_t>((crc >> 1) ^ 0xA001u)
                : static_cast<std::uint16_t>(crc >> 1);
        }
    }
    return crc;
}

std::uint8_t crc8Poly07(const std::uint8_t* data, std::size_t size) {
    std::uint8_t crc = 0x00u;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80u) != 0u ? static_cast<std::uint8_t>((crc << 1) ^ 0x07u)
                                      : static_cast<std::uint8_t>(crc << 1);
    }
    return crc;
}

bool readFile(const std::filesystem::path& path,
              std::vector<std::uint8_t>& data,
              std::wstring& error) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        error = L"The CLO file cannot be opened.";
        return false;
    }
    const auto end = in.tellg();
    if (end < 0) {
        error = L"Cannot determine the CLO file size.";
        return false;
    }
    data.resize(static_cast<std::size_t>(end));
    in.seekg(0, std::ios::beg);
    if (!data.empty()) {
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (static_cast<std::size_t>(in.gcount()) != data.size()) {
            error = L"The CLO file could not be read completely.";
            return false;
        }
    }
    return true;
}

bool makeGp5CompactClo(const std::filesystem::path& source,
                       std::vector<std::uint8_t>& compact,
                       std::wstring& error) {
    std::vector<std::uint8_t> sourceData;
    if (!readFile(source, sourceData, error)) return false;
    if (sourceData.size() < gp5DeclaredBytes) {
        error = L"The CLO file is too small for an A128/B512 GP-5 model.";
        return false;
    }

    const bool isVtsi = sourceData[0] == 'V' && sourceData[1] == 'T'
                     && sourceData[2] == 'S' && sourceData[3] == 'I';
    const bool isHtsi = sourceData[0] == 'H' && sourceData[1] == 'T'
                     && sourceData[2] == 'S' && sourceData[3] == 'I';
    if (!isVtsi && !isHtsi) {
        error = L"Unsupported CLO format (expected VTSI or HTSI).";
        return false;
    }

    if (sourceData.size() < headerBytes) {
        error = L"The CLO header is incomplete.";
        return false;
    }
    const auto startA = readLe32(sourceData.data() + startAOffset);
    const auto countA = readLe32(sourceData.data() + countAOffset);
    const auto startB = readLe32(sourceData.data() + startBOffset);
    const auto countB = readLe32(sourceData.data() + countBOffset);
    if (startA != 0u || countA != blockATaps || startB != blockATaps || countB < gp5BlockBTaps) {
        error = L"Unsupported CLO structure. GP-5 upload requires A=128 and at least 512 B taps.";
        return false;
    }

    const std::size_t requiredCoeffBytes = headerBytes + (blockATaps + gp5BlockBTaps) * sizeof(float);
    if (sourceData.size() < requiredCoeffBytes) {
        error = L"The CLO does not contain the first 512 B coefficients required by GP-5.";
        return false;
    }

    compact.assign(sourceData.begin(), sourceData.begin() + static_cast<std::ptrdiff_t>(gp5DeclaredBytes));
    compact[0] = 'V'; compact[1] = 'T'; compact[2] = 'S'; compact[3] = 'I';
    writeLe32(compact.data() + declaredOffset, static_cast<std::uint32_t>(gp5DeclaredBytes));
    writeLe32(compact.data() + payloadOffset, gp5PayloadBytes);
    writeLe32(compact.data() + startAOffset, 0u);
    writeLe32(compact.data() + countAOffset, static_cast<std::uint32_t>(blockATaps));
    writeLe32(compact.data() + startBOffset, static_cast<std::uint32_t>(blockATaps));
    writeLe32(compact.data() + countBOffset, static_cast<std::uint32_t>(gp5BlockBTaps));

    // GP-5 captures store CRC16/MODBUS high byte first at 0x08/0x09,
    // calculated over [0x0C, declaredSize).
    const std::uint16_t crc = crc16Modbus(compact.data() + crcDataOffset,
                                         gp5DeclaredBytes - crcDataOffset);
    compact[crcOffset] = static_cast<std::uint8_t>((crc >> 8) & 0xFFu);
    compact[crcOffset + 1] = static_cast<std::uint8_t>(crc & 0xFFu);
    return true;
}

std::string stemUtf8(const std::filesystem::path& path) {
    const std::wstring w = path.stem().wstring();
    if (w.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string out(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), count, nullptr, nullptr);
    if (out.size() > 64) out.resize(64);
    return out;
}

std::vector<std::uint8_t> makeTransferFrame(std::uint8_t sequence,
                                            const std::uint8_t* payload,
                                            std::size_t payloadSize) {
    // Decoded body observed in Valeton Suite:
    // [CRC8][0x92][sequence][payload length][payload].
    std::vector<std::uint8_t> body;
    body.reserve(4 + payloadSize);
    body.push_back(0); // CRC placeholder
    body.push_back(0x92);
    body.push_back(sequence);
    body.push_back(static_cast<std::uint8_t>(payloadSize));
    body.insert(body.end(), payload, payload + payloadSize);
    body[0] = crc8Poly07(body.data() + 1, body.size() - 1);

    // Every decoded byte is transmitted as high nibble then low nibble,
    // wrapped directly in F0/F7 SysEx.
    std::vector<std::uint8_t> sysex;
    sysex.reserve(2 + body.size() * 2);
    sysex.push_back(0xF0);
    for (const auto byte : body) {
        sysex.push_back(static_cast<std::uint8_t>((byte >> 4) & 0x0Fu));
        sysex.push_back(static_cast<std::uint8_t>(byte & 0x0Fu));
    }
    sysex.push_back(0xF7);
    return sysex;
}
} // namespace

bool buildCloUpload(const std::filesystem::path& cloFile,
                    int slot,
                    CloUploadData& result,
                    std::wstring& error) {
    error.clear();
    result = {};

    std::error_code ec;
    if (!std::filesystem::is_regular_file(cloFile, ec) || ec) {
        error = L"The selected CLO file does not exist.";
        return false;
    }
    // Only the experimentally validated user SnapTone range is exposed:
    // visible SnapTone 51..80 == zero-based protocol slots 50..79.
    if (slot < 50 || slot >= 80) {
        error = L"The GP-5 destination must be SnapTone 51-80.";
        return false;
    }

    std::vector<std::uint8_t> compact;
    if (!makeGp5CompactClo(cloFile, compact, error)) return false;

    std::array<std::uint8_t, wrapperBytes> wrapper{};
    wrapper[0] = 0x11;
    wrapper[1] = 0x25;
    wrapper[6] = static_cast<std::uint8_t>(slot); // visible slot - 1
    wrapper[9] = 0x0F;

    const auto displayName = stemUtf8(cloFile);
    for (std::size_t i = 0; i < displayName.size() && i < 64; ++i)
        wrapper[10 + i] = static_cast<std::uint8_t>(displayName[i]);

    std::vector<std::uint8_t> stream;
    stream.reserve(wrapper.size() + compact.size());
    stream.insert(stream.end(), wrapper.begin(), wrapper.end());
    stream.insert(stream.end(), compact.begin(), compact.end());

    // Captures: 2770 bytes = 145*19 + 15 => sequences 0x00..0x91.
    std::uint16_t sequence = 0;
    for (std::size_t offset = 0; offset < stream.size(); offset += chunkPayloadBytes, ++sequence) {
        const std::size_t amount = std::min(chunkPayloadBytes, stream.size() - offset);
        if (sequence > 0xFFu) {
            error = L"GP-5 transfer generated too many chunks.";
            return false;
        }
        result.chunks.push_back(makeTransferFrame(static_cast<std::uint8_t>(sequence),
                                                  stream.data() + offset, amount));
    }

    if (result.chunks.size() != 146u) {
        error = L"Internal GP-5 transfer size mismatch (expected 146 chunks).";
        return false;
    }

    result.displayName = displayName;
    result.slot = slot;
    result.compactCloBytes = compact.size();
    return true;
}

} // namespace ntc::gp5
