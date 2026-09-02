#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ntc {

namespace fs = std::filesystem;

inline constexpr std::uint64_t kExpectedCloSize = 0x2288;
inline constexpr std::uint32_t kExpectedApiReturn = 0x2288;
inline constexpr wchar_t kVersion[] = L"2.9.13";

struct CloInfo {
    bool exists = false;
    std::uint64_t size = 0;
    std::vector<std::uint8_t> prefix;
    std::string magic;
    std::uint32_t declaredSize = 0;
    std::uint32_t payloadSize = 0;
    std::uint32_t modelField = 0;
    std::uint64_t lastNonZero = 0;
    bool hasLastNonZero = false;
};

struct BlockCompareStats {
    std::size_t count = 0;
    std::size_t exactFloatMatches = 0;
    double correlation = 0.0;
    double mae = 0.0;
    double rmse = 0.0;
    double maxAbsError = 0.0;
};

struct Gp200CompareResult {
    bool ok = false;
    std::string error;
    CloInfo a;
    CloInfo b;
    std::size_t byteMatches = 0;
    std::size_t byteDifferences = 0;
    std::size_t usefulByteDifferences = 0;
    std::size_t paddingByteDifferences = 0;
    bool crcAValid = false;
    bool crcBValid = false;
    std::uint16_t storedCrcA = 0;
    std::uint16_t storedCrcB = 0;
    std::uint16_t calculatedCrcA = 0;
    std::uint16_t calculatedCrcB = 0;
    BlockCompareStats blockA;
    BlockCompareStats blockB;
};

std::string toUtf8(const std::wstring& value);
std::wstring fromUtf8(const std::string& value);
std::string pathToUtf8(const fs::path& path);
std::wstring quoteWindowsArg(const std::wstring& arg);
fs::path executablePath();
std::string win32ErrorMessage(std::uint32_t code);
std::string hex32(std::uint32_t value);
std::string hexBytes(const std::vector<std::uint8_t>& bytes);
CloInfo inspectClo(const fs::path& path, std::size_t prefixBytes = 16);
void printCloInfo(const fs::path& path, const CloInfo& info);
bool readFileBytes(const fs::path& path, std::vector<std::uint8_t>& data, std::string& error);
bool writeFileBytes(const fs::path& path, const std::uint8_t* data, std::size_t size, std::string& error);
bool makeGp200CompactClo(const fs::path& source, const fs::path& destination, std::string& error);
Gp200CompareResult compareGp200Clo(const fs::path& a, const fs::path& b);
void printGp200Compare(const fs::path& a, const fs::path& b, const Gp200CompareResult& result);

} // namespace ntc
