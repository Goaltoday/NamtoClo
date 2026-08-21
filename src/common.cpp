#include "common.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <cstring>
#include <limits>
#include <iostream>
#include <sstream>
#include <system_error>

namespace ntc {

std::string toUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                            nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring fromUtf8(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed);
    return out;
}

std::string pathToUtf8(const fs::path& path) {
    return toUtf8(path.wstring());
}

std::wstring quoteWindowsArg(const std::wstring& arg) {
    if (arg.empty()) {
        return L"\"\"";
    }
    if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }

    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(ch);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

fs::path executablePath() {
    std::wstring buffer(32768, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0 || len >= buffer.size()) {
        return {};
    }
    buffer.resize(len);
    return fs::path(buffer);
}

std::string win32ErrorMessage(const std::uint32_t code) {
    LPWSTR raw = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD count = FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
    if (count == 0 || raw == nullptr) {
        return "Win32 error " + std::to_string(code);
    }
    std::wstring message(raw, count);
    LocalFree(raw);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    return toUtf8(message);
}

std::string hex32(const std::uint32_t value) {
    std::ostringstream os;
    os << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return os.str();
}

std::string hexBytes(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream os;
    os << std::uppercase << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) {
            os << ' ';
        }
        os << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return os.str();
}

CloInfo inspectClo(const fs::path& path, const std::size_t prefixBytes) {
    CloInfo info;
    std::error_code ec;
    info.exists = fs::exists(path, ec) && !ec;
    if (!info.exists) {
        return info;
    }
    info.size = fs::file_size(path, ec);
    if (ec) {
        info.size = 0;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return info;
    }
    info.prefix.resize(prefixBytes);
    stream.read(reinterpret_cast<char*>(info.prefix.data()), static_cast<std::streamsize>(info.prefix.size()));
    info.prefix.resize(static_cast<std::size_t>(stream.gcount()));

    const std::size_t magicLen = std::min<std::size_t>(4, info.prefix.size());
    info.magic.assign(reinterpret_cast<const char*>(info.prefix.data()), magicLen);
    for (char& ch : info.magic) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (uch < 0x20 || uch > 0x7E) {
            ch = '.';
        }
    }

    auto readLe32 = [](const std::vector<std::uint8_t>& data, std::size_t offset) -> std::uint32_t {
        if (offset + 4 > data.size()) return 0;
        return static_cast<std::uint32_t>(data[offset])
             | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
             | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
             | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
    };

    std::vector<std::uint8_t> header(0x88);
    stream.clear();
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    header.resize(static_cast<std::size_t>(stream.gcount()));
    info.declaredSize = readLe32(header, 0x04);
    info.payloadSize = readLe32(header, 0x14);
    info.modelField = readLe32(header, 0x84);

    stream.clear();
    stream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> all(static_cast<std::size_t>(info.size));
    if (!all.empty()) {
        stream.read(reinterpret_cast<char*>(all.data()), static_cast<std::streamsize>(all.size()));
        const auto count = static_cast<std::size_t>(stream.gcount());
        for (std::size_t i = count; i > 0; --i) {
            if (all[i - 1] != 0) {
                info.lastNonZero = static_cast<std::uint64_t>(i - 1);
                info.hasLastNonZero = true;
                break;
            }
        }
    }
    return info;
}

void printCloInfo(const fs::path& path, const CloInfo& info) {
    std::cout << "CLO: " << pathToUtf8(path) << "\n";
    std::cout << "  exists: " << (info.exists ? "yes" : "no") << "\n";
    if (!info.exists) {
        return;
    }
    std::cout << "  size:   " << info.size << " bytes (0x" << std::uppercase << std::hex << info.size
              << std::dec << ")\n";
    std::cout << "  magic:  " << info.magic << "\n";
    std::cout << "  prefix: " << hexBytes(info.prefix) << "\n";
    std::cout << "  expected-size: " << (info.size == kExpectedCloSize ? "yes" : "NO") << "\n";
    if (info.magic == "VTSI") {
        std::cout << "  declared-size @0x04: 0x" << std::uppercase << std::hex << info.declaredSize << std::dec << "\n";
        std::cout << "  payload-size  @0x14: 0x" << std::uppercase << std::hex << info.payloadSize << std::dec << "\n";
        std::cout << "  model-field   @0x84: 0x" << std::uppercase << std::hex << info.modelField << std::dec << "\n";
        if (info.hasLastNonZero) {
            std::cout << "  last-nonzero:        0x" << std::uppercase << std::hex << info.lastNonZero << std::dec << "\n";
        } else {
            std::cout << "  last-nonzero:        none\n";
        }
    }
}



static std::uint16_t crc16Modbus(const std::uint8_t* data, std::size_t size) {
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

bool makeGp200CompactClo(const fs::path& source, const fs::path& destination, std::string& error) {
    std::ifstream in(source, std::ios::binary);
    if (!in) {
        error = "Cannot open source CLO: " + pathToUtf8(source);
        return false;
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(kExpectedCloSize));
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (static_cast<std::size_t>(in.gcount()) != data.size()) {
        error = "Source CLO is not exactly 0x2288 bytes.";
        return false;
    }
    if (!(data[0] == 'V' && data[1] == 'T' && data[2] == 'S' && data[3] == 'I')) {
        error = "Source CLO magic is not VTSI.";
        return false;
    }
    auto readLe32 = [&](std::size_t off) -> std::uint32_t {
        return static_cast<std::uint32_t>(data[off])
             | (static_cast<std::uint32_t>(data[off+1]) << 8)
             | (static_cast<std::uint32_t>(data[off+2]) << 16)
             | (static_cast<std::uint32_t>(data[off+3]) << 24);
    };
    auto writeLe32 = [&](std::size_t off, std::uint32_t v) {
        data[off] = static_cast<std::uint8_t>(v & 0xFFu);
        data[off+1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
        data[off+2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
        data[off+3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
    };

    // The compact GP-200 shape inferred from same-NAM Valeton captures is:
    // 0x88-byte header + 128 float32 values + 1024 float32 values = 0x1288 useful bytes.
    // The Ampero normal VTSI uses the same offsets but serializes 2048 values in block B.
    if (readLe32(0x84) != 0x800u) {
        error = "Expected normal Ampero model-field 0x800 before compact serialization.";
        return false;
    }

    writeLe32(0x04, 0x1288u);
    writeLe32(0x14, 0x1200u);
    writeLe32(0x84, 0x0400u);
    std::fill(data.begin() + 0x1288, data.end(), std::uint8_t{0});

    // HTUSBTools stores CRC16/MODBUS with bytes swapped, over [0x0C, declaredSize).
    const std::uint16_t crc = crc16Modbus(data.data() + 0x0C, 0x1288u - 0x0Cu);
    data[0x08] = static_cast<std::uint8_t>((crc >> 8) & 0xFFu);
    data[0x09] = static_cast<std::uint8_t>(crc & 0xFFu);

    std::error_code ec;
    if (destination.has_parent_path()) {
        fs::create_directories(destination.parent_path(), ec);
        if (ec) {
            error = "Cannot create compact CLO output directory: " + ec.message();
            return false;
        }
    }
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Cannot create compact CLO: " + pathToUtf8(destination);
        return false;
    }
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out) {
        error = "Failed writing compact CLO: " + pathToUtf8(destination);
        return false;
    }
    return true;
}


static bool readWholeFile(const fs::path& path, std::vector<std::uint8_t>& data, std::string& error) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        error = "Cannot open file: " + pathToUtf8(path);
        return false;
    }
    const auto end = in.tellg();
    if (end < 0) {
        error = "Cannot determine file size: " + pathToUtf8(path);
        return false;
    }
    data.resize(static_cast<std::size_t>(end));
    in.seekg(0, std::ios::beg);
    if (!data.empty()) {
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (static_cast<std::size_t>(in.gcount()) != data.size()) {
            error = "Short read: " + pathToUtf8(path);
            return false;
        }
    }
    return true;
}

static std::uint32_t readLe32Raw(const std::vector<std::uint8_t>& data, std::size_t off) {
    if (off + 4 > data.size()) return 0;
    return static_cast<std::uint32_t>(data[off])
         | (static_cast<std::uint32_t>(data[off + 1]) << 8)
         | (static_cast<std::uint32_t>(data[off + 2]) << 16)
         | (static_cast<std::uint32_t>(data[off + 3]) << 24);
}

static float readLeFloatRaw(const std::vector<std::uint8_t>& data, std::size_t off) {
    const std::uint32_t bits = readLe32Raw(data, off);
    float value = 0.0f;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

static BlockCompareStats compareFloatBlock(const std::vector<std::uint8_t>& a,
                                           const std::vector<std::uint8_t>& b,
                                           std::size_t offset,
                                           std::size_t count) {
    BlockCompareStats out;
    if (offset + count * 4 > a.size() || offset + count * 4 > b.size()) {
        return out;
    }
    out.count = count;
    long double sumA = 0.0L, sumB = 0.0L;
    long double sumAA = 0.0L, sumBB = 0.0L, sumAB = 0.0L;
    long double sumAbs = 0.0L, sumSq = 0.0L;
    double maxAbs = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t off = offset + i * 4;
        const std::uint32_t bitsA = readLe32Raw(a, off);
        const std::uint32_t bitsB = readLe32Raw(b, off);
        if (bitsA == bitsB) ++out.exactFloatMatches;
        const double va = static_cast<double>(readLeFloatRaw(a, off));
        const double vb = static_cast<double>(readLeFloatRaw(b, off));
        const double d = va - vb;
        const double ad = std::abs(d);
        maxAbs = std::max(maxAbs, ad);
        sumA += va; sumB += vb;
        sumAA += va * va; sumBB += vb * vb; sumAB += va * vb;
        sumAbs += ad; sumSq += d * d;
    }
    const long double n = static_cast<long double>(count);
    const long double covN = n * sumAB - sumA * sumB;
    const long double varAN = n * sumAA - sumA * sumA;
    const long double varBN = n * sumBB - sumB * sumB;
    const long double denom = std::sqrt(std::max(0.0L, varAN * varBN));
    out.correlation = denom > std::numeric_limits<long double>::epsilon()
        ? static_cast<double>(covN / denom) : 0.0;
    out.mae = static_cast<double>(sumAbs / n);
    out.rmse = std::sqrt(static_cast<double>(sumSq / n));
    out.maxAbsError = maxAbs;
    return out;
}

static void crcInfo(const std::vector<std::uint8_t>& data, bool& valid,
                    std::uint16_t& stored, std::uint16_t& calculated) {
    valid = false; stored = 0; calculated = 0;
    if (data.size() < 0x0C) return;
    const std::uint32_t declared = readLe32Raw(data, 0x04);
    if (declared < 0x0C || declared > data.size()) return;
    stored = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0x08]) << 8) | data[0x09]);
    calculated = crc16Modbus(data.data() + 0x0C, declared - 0x0C);
    valid = stored == calculated;
}

Gp200CompareResult compareGp200Clo(const fs::path& aPath, const fs::path& bPath) {
    Gp200CompareResult result;
    result.a = inspectClo(aPath, 32);
    result.b = inspectClo(bPath, 32);
    std::vector<std::uint8_t> a, b;
    if (!readWholeFile(aPath, a, result.error)) return result;
    if (!readWholeFile(bPath, b, result.error)) return result;
    if (a.size() != kExpectedCloSize || b.size() != kExpectedCloSize) {
        result.error = "Both files must be exactly 0x2288 bytes.";
        return result;
    }
    if (a.size() < 4 || b.size() < 4 || std::memcmp(a.data(), "VTSI", 4) != 0 || std::memcmp(b.data(), "VTSI", 4) != 0) {
        result.error = "Both files must have VTSI magic.";
        return result;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] == b[i]) ++result.byteMatches;
        else ++result.byteDifferences;
    }
    constexpr std::size_t usefulEnd = 0x1288;
    for (std::size_t i = 0; i < usefulEnd; ++i) {
        if (a[i] != b[i]) ++result.usefulByteDifferences;
    }
    for (std::size_t i = usefulEnd; i < a.size(); ++i) {
        if (a[i] != b[i]) ++result.paddingByteDifferences;
    }
    crcInfo(a, result.crcAValid, result.storedCrcA, result.calculatedCrcA);
    crcInfo(b, result.crcBValid, result.storedCrcB, result.calculatedCrcB);
    // GP-200 structure inferred from same-NAM captures:
    // block A: 128 float32 at 0x88; block B: 1024 float32 at 0x288.
    result.blockA = compareFloatBlock(a, b, 0x88, 128);
    result.blockB = compareFloatBlock(a, b, 0x288, 1024);
    result.ok = true;
    return result;
}

static void printBlockStats(const char* name, const BlockCompareStats& s) {
    std::cout << "  " << name << ":\n";
    std::cout << "    floats:        " << s.count << "\n";
    std::cout << "    exact matches: " << s.exactFloatMatches << "/" << s.count << "\n";
    std::cout << std::fixed << std::setprecision(9);
    std::cout << "    correlation:   " << s.correlation << "\n";
    std::cout << "    MAE:           " << s.mae << "\n";
    std::cout << "    RMSE:          " << s.rmse << "\n";
    std::cout << "    max abs error: " << s.maxAbsError << "\n";
    std::cout.unsetf(std::ios::floatfield);
}

void printGp200Compare(const fs::path& aPath, const fs::path& bPath, const Gp200CompareResult& r) {
    std::cout << "GP-200 CLO comparison\n";
    std::cout << "  A: " << pathToUtf8(aPath) << "\n";
    std::cout << "  B: " << pathToUtf8(bPath) << "\n";
    if (!r.ok) {
        std::cout << "  ERROR: " << r.error << "\n";
        return;
    }
    std::cout << "\nStructure\n";
    std::cout << "  A declared/payload/model: 0x" << std::hex << std::uppercase << r.a.declaredSize
              << " / 0x" << r.a.payloadSize << " / 0x" << r.a.modelField << std::dec << "\n";
    std::cout << "  B declared/payload/model: 0x" << std::hex << std::uppercase << r.b.declaredSize
              << " / 0x" << r.b.payloadSize << " / 0x" << r.b.modelField << std::dec << "\n";
    std::cout << "  A GP200 shape: " << ((r.a.declaredSize == 0x1288 && r.a.payloadSize == 0x1200 && r.a.modelField == 0x400) ? "yes" : "NO") << "\n";
    std::cout << "  B GP200 shape: " << ((r.b.declaredSize == 0x1288 && r.b.payloadSize == 0x1200 && r.b.modelField == 0x400) ? "yes" : "NO") << "\n";

    std::cout << "\nCRC16/MODBUS\n";
    std::cout << "  A stored/calculated: 0x" << std::hex << std::uppercase << r.storedCrcA
              << " / 0x" << r.calculatedCrcA << std::dec << " -> " << (r.crcAValid ? "valid" : "INVALID") << "\n";
    std::cout << "  B stored/calculated: 0x" << std::hex << std::uppercase << r.storedCrcB
              << " / 0x" << r.calculatedCrcB << std::dec << " -> " << (r.crcBValid ? "valid" : "INVALID") << "\n";

    std::cout << "\nByte comparison\n";
    std::cout << "  equal:              " << r.byteMatches << "/" << kExpectedCloSize << "\n";
    std::cout << "  different:          " << r.byteDifferences << "/" << kExpectedCloSize << "\n";
    std::cout << "  different <0x1288:  " << r.usefulByteDifferences << "\n";
    std::cout << "  different padding:  " << r.paddingByteDifferences << "\n";

    std::cout << "\nFloat blocks\n";
    printBlockStats("Block A @0x88, 128 float32", r.blockA);
    printBlockStats("Block B @0x288, 1024 float32", r.blockB);
}


bool readFileBytes(const fs::path& path, std::vector<std::uint8_t>& data, std::string& error) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { error = "Cannot open file: " + pathToUtf8(path); return false; }
    const auto end = in.tellg();
    if (end < 0) { error = "Cannot determine file size: " + pathToUtf8(path); return false; }
    data.resize(static_cast<std::size_t>(end));
    in.seekg(0, std::ios::beg);
    if (!data.empty()) {
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (static_cast<std::size_t>(in.gcount()) != data.size()) {
            error = "Short read: " + pathToUtf8(path); return false;
        }
    }
    return true;
}

bool writeFileBytes(const fs::path& path, const std::uint8_t* data, std::size_t size, std::string& error) {
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) { error = "Cannot create output directory: " + ec.message(); return false; }
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { error = "Cannot create file: " + pathToUtf8(path); return false; }
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!out) { error = "Failed writing file: " + pathToUtf8(path); return false; }
    return true;
}

} // namespace ntc
