#include "gp200_clo_upload.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <windows.h>

namespace ntc::gp200 {
namespace {
constexpr int modelBytes = 8192;
constexpr int wrapperBytes = 28;
constexpr int blobBytes = wrapperBytes + modelBytes;
constexpr int chunkBytes = 183;

constexpr std::size_t physicalContainerBytes = 0x2288;
constexpr std::size_t gp200DeclaredBytes = 0x1288;
constexpr std::uint32_t largeDeclaredBytes = 0x2288;
constexpr std::uint32_t gp200PayloadBytes = 0x1200;
constexpr std::uint32_t largePayloadBytes = 0x2200;
constexpr std::uint32_t blockACount = 0x80;
constexpr std::uint32_t gp200BlockBCount = 0x400;
constexpr std::uint32_t largeBlockBCount = 0x800;

constexpr std::size_t declaredOffset = 0x04;
constexpr std::size_t crcOffset = 0x08;
constexpr std::size_t blockACountOffset = 0x80;
constexpr std::size_t blockBCountOffset = 0x84;
constexpr std::size_t payloadOffset = 0x14;
constexpr std::size_t crcDataOffset = 0x0c;

std::uint32_t readLe32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0])
         | (static_cast<std::uint32_t>(data[1]) << 8)
         | (static_cast<std::uint32_t>(data[2]) << 16)
         | (static_cast<std::uint32_t>(data[3]) << 24);
}

void writeLe32(std::uint8_t* data, std::uint32_t value) {
    data[0] = static_cast<std::uint8_t>(value & 0xff);
    data[1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    data[2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
    data[3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
}

std::uint16_t crc16Modbus(const std::uint8_t* data, std::size_t size) {
    std::uint16_t crc = 0xffff;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint16_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) != 0u
                ? static_cast<std::uint16_t>((crc >> 1) ^ 0xa001u)
                : static_cast<std::uint16_t>(crc >> 1);
        }
    }
    return crc;
}

void updateInternalCrc(std::array<std::uint8_t, physicalContainerBytes>& container) {
    const auto declared = readLe32(container.data() + declaredOffset);
    const auto crc = crc16Modbus(container.data() + crcDataOffset,
                                 static_cast<std::size_t>(declared) - crcDataOffset);
    container[crcOffset] = static_cast<std::uint8_t>(crc & 0xff);
    container[crcOffset + 1] = static_cast<std::uint8_t>((crc >> 8) & 0xff);
}

bool prepareSoundCloneModelForGP200(const std::filesystem::path& sourceFile,
                                    std::array<std::uint8_t, physicalContainerBytes>& prepared,
                                    std::wstring& error) {
    std::ifstream input(sourceFile, std::ios::binary);
    if (!input) {
        error = L"The Sound Clone file cannot be opened.";
        return false;
    }
    input.read(reinterpret_cast<char*>(prepared.data()), static_cast<std::streamsize>(prepared.size()));
    if (input.gcount() != static_cast<std::streamsize>(prepared.size())) {
        error = L"The Sound Clone file is too small. At least 0x2288 (8840) bytes are required.";
        return false;
    }

    const bool isVtsi = prepared[0] == 'V' && prepared[1] == 'T' && prepared[2] == 'S' && prepared[3] == 'I';
    const bool isHtsi = prepared[0] == 'H' && prepared[1] == 'T' && prepared[2] == 'S' && prepared[3] == 'I';
    if (!isVtsi && !isHtsi) {
        error = L"Unsupported Sound Clone format (expected VTSI or HTSI header).";
        return false;
    }

    const auto declared = readLe32(prepared.data() + declaredOffset);
    const auto blockA = readLe32(prepared.data() + blockACountOffset);
    const auto blockB = readLe32(prepared.data() + blockBCountOffset);
    const auto payload = readLe32(prepared.data() + payloadOffset);

    const bool isGp2001024 = declared == gp200DeclaredBytes && payload == gp200PayloadBytes
                          && blockA == blockACount && blockB == gp200BlockBCount;
    const bool isLarge2048 = declared == largeDeclaredBytes && payload == largePayloadBytes
                          && blockA == blockACount && blockB == largeBlockBCount;

    if (isVtsi && isGp2001024)
        return true;

    if (!isLarge2048) {
        error = L"Unsupported Sound Clone structure. Expected 128+1024 GP-200 VTSI or 128+2048 VTSI/HTSI model data.";
        return false;
    }

    prepared[0] = 'V'; prepared[1] = 'T'; prepared[2] = 'S'; prepared[3] = 'I';
    writeLe32(prepared.data() + declaredOffset, static_cast<std::uint32_t>(gp200DeclaredBytes));
    writeLe32(prepared.data() + payloadOffset, gp200PayloadBytes);
    writeLe32(prepared.data() + blockBCountOffset, gp200BlockBCount);
    std::fill(prepared.begin() + static_cast<std::ptrdiff_t>(gp200DeclaredBytes), prepared.end(), static_cast<std::uint8_t>(0));
    updateInternalCrc(prepared);
    return true;
}

std::vector<std::uint8_t> nibbleEncode(const std::uint8_t* data, int size) {
    std::vector<std::uint8_t> encoded;
    encoded.reserve(static_cast<std::size_t>(size) * 2u);
    for (int i = 0; i < size; ++i) {
        encoded.push_back(static_cast<std::uint8_t>((data[i] >> 4) & 0x0f));
        encoded.push_back(static_cast<std::uint8_t>(data[i] & 0x0f));
    }
    return encoded;
}

std::vector<std::uint8_t> makePrepare(int globalSlot) {
    const auto category = static_cast<std::uint8_t>(globalSlot < 5 ? 0x03 : 0x02);
    return { 0xf0,0x21,0x25,0x7e,0x47,0x50,0x2d,0x32,0x12,0x14,
             0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,
             0x00,0x00,0x00,0x0d,0x0e,0x00,0x00,0x01,0x06,0x00,0x00,0x00,
             0x08,0x00,0x00,0x00,category,0x0f,0x07,0x0e,0x0f,0x00,0x02,0x00,
             static_cast<std::uint8_t>(globalSlot),0x00,0x00,0x00,0x00,0x00,0x0f,0xf7 };
}

std::string stemUtf8(const std::filesystem::path& path) {
    std::wstring w = path.stem().wstring();
    if (w.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string out(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), count, nullptr, nullptr);
    if (out.size() > 16) out.resize(16);
    return out;
}
} // namespace

bool buildCloUpload(const std::filesystem::path& cloFile,
                    int globalSlot,
                    CloUploadData& result,
                    std::wstring& error) {
    error.clear();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(cloFile, ec) || ec) {
        error = L"The selected Sound Clone file does not exist.";
        return false;
    }
    if (globalSlot < 0 || globalSlot >= 10) {
        error = L"The Sound Clone destination must be AMP 1-5 or DIST 1-5.";
        return false;
    }

    std::array<std::uint8_t, physicalContainerBytes> preparedContainer{};
    if (!prepareSoundCloneModelForGP200(cloFile, preparedContainer, error))
        return false;

    std::array<std::uint8_t, modelBytes> model{};
    std::copy_n(preparedContainer.begin(), modelBytes, model.begin());

    std::array<std::uint8_t, blobBytes> blob{};
    blob[0] = 0x13; blob[1] = 0x10; blob[2] = 0x18; blob[3] = 0x20;
    blob[4] = static_cast<std::uint8_t>(globalSlot);
    blob[10] = 0x08; blob[11] = 0x00;

    const auto displayName = stemUtf8(cloFile);
    for (std::size_t i = 0; i < displayName.size() && i < 16; ++i)
        blob[12 + i] = static_cast<std::uint8_t>(displayName[i]);

    std::uint16_t checksum = 0;
    for (const auto byte : model)
        checksum = static_cast<std::uint16_t>(checksum + byte);
    blob[8] = static_cast<std::uint8_t>(checksum & 0xff);
    blob[9] = static_cast<std::uint8_t>((checksum >> 8) & 0xff);
    std::copy(model.begin(), model.end(), blob.begin() + wrapperBytes);

    result = {};
    result.displayName = displayName;
    result.globalSlot = globalSlot;
    result.prepareMessage = makePrepare(globalSlot);

    for (int offset = 0; offset < blobBytes; offset += chunkBytes) {
        const auto amount = std::min(chunkBytes, blobBytes - offset);
        std::vector<std::uint8_t> full {
            0xf0,0x21,0x25,0x7e,0x47,0x50,0x2d,0x32,0x12,0x1c,0x40,
            static_cast<std::uint8_t>(offset & 0x7f),
            static_cast<std::uint8_t>((offset >> 7) & 0x7f)
        };
        auto encoded = nibbleEncode(blob.data() + offset, amount);
        full.insert(full.end(), encoded.begin(), encoded.end());
        full.push_back(0xf7);
        result.chunks.push_back(std::move(full));
    }
    return true;
}

} // namespace ntc::gp200
