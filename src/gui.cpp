#include "native_converter.hpp"
#include "common.hpp"
#include "gp200_midi.hpp"
#include "gp5_midi.hpp"
#include "tone3000_client.hpp"
#include "nam_preview_player.hpp"
#include "resource.h"

#include <windows.h>
#include <mmsystem.h>
#include <dpapi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>
#include <cwctype>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace {
constexpr wchar_t kClassName[] = L"NamToCloMainWindow";
constexpr UINT WM_APP_STATUS = WM_APP + 1;
constexpr UINT WM_APP_DONE_SINGLE = WM_APP + 2;
constexpr UINT WM_APP_DONE_BATCH = WM_APP + 3;
constexpr UINT WM_APP_UPLOAD_PROGRESS = WM_APP + 4;
constexpr UINT WM_APP_UPLOAD_DONE = WM_APP + 5;
constexpr UINT WM_APP_GP5_UPLOAD_PROGRESS = WM_APP + 6;
constexpr UINT WM_APP_GP5_UPLOAD_DONE = WM_APP + 7;
constexpr UINT WM_APP_T3K_AUTH_DONE = WM_APP + 8;
constexpr UINT WM_APP_T3K_SEARCH_DONE = WM_APP + 9;
constexpr UINT WM_APP_T3K_MODELS_DONE = WM_APP + 10;
constexpr UINT WM_APP_T3K_DOWNLOAD_DONE = WM_APP + 11;
constexpr UINT WM_APP_T3K_AUTOLOGIN_DONE = WM_APP + 12;
constexpr UINT WM_APP_T3K_PREVIEW_DONE = WM_APP + 13;
constexpr int IDC_INPUT_PATH = 101;
constexpr int IDC_LOAD_FILE = 102;
constexpr int IDC_LOAD_FOLDER = 103;
constexpr int IDC_OUTPUT_PATH = 104;
constexpr int IDC_BROWSE_OUTPUT = 105;
constexpr int IDC_CONVERT = 106;
constexpr int IDC_OPEN_OUTPUT = 107;
constexpr int IDC_STATUS = 108;
constexpr int IDC_TAIL_MODE = 110;
constexpr int IDC_RECORDED_PATH = 111;
constexpr int IDC_BROWSE_RECORDED = 112;
constexpr int IDC_VERSION = 113;
constexpr int IDC_SUBTITLE = 114;
constexpr int IDC_INFO = 115;
constexpr int IDC_APPLY_CORRECTIVE_IR = 118;
constexpr int IDC_CORRECTIVE_IR_PATH = 119;
constexpr int IDC_BROWSE_CORRECTIVE_IR = 120;
constexpr int IDC_REFINE_CLO = 121;
constexpr int IDC_REFINE_TARGET_PATH = 122;
constexpr int IDC_BROWSE_REFINE_TARGET = 123;
constexpr int IDC_REFINE_SOURCE = 155;
constexpr int IDC_BACKEND_TABS = 124;
constexpr int IDC_UPLOADER_CLO_PATH = 125;
constexpr int IDC_UPLOADER_BROWSE = 126;
constexpr int IDC_UPLOADER_SLOT = 127;
constexpr int IDC_UPLOADER_RESCAN = 128;
constexpr int IDC_UPLOADER_UPLOAD = 129;
constexpr int IDC_UPLOADER_DEVICE = 130;
constexpr int IDC_UPLOADER_PROGRESS = 131;
constexpr int IDC_GP5_CLO_PATH = 132;
constexpr int IDC_GP5_BROWSE = 133;
constexpr int IDC_GP5_SLOT = 134;
constexpr int IDC_GP5_RESCAN = 135;
constexpr int IDC_GP5_UPLOAD = 136;
constexpr int IDC_GP5_DEVICE = 137;
constexpr int IDC_GP5_PROGRESS = 138;
constexpr int IDC_T3K_KEY = 139;
constexpr int IDC_T3K_CONNECT = 140;
constexpr int IDC_T3K_SEARCH = 141;
constexpr int IDC_T3K_SEARCH_BUTTON = 142;
constexpr int IDC_T3K_RESULTS = 143;
constexpr int IDC_T3K_MODELS = 144;
constexpr int IDC_T3K_USE = 145;
constexpr int IDC_T3K_STATE = 146;
constexpr int IDC_T3K_PREVIOUS = 147;
constexpr int IDC_T3K_PAGE = 148;
constexpr int IDC_T3K_NEXT = 149;
constexpr int IDC_T3K_SORT = 150;
constexpr int IDC_T3K_PREVIEW_PLAY = 151;
constexpr int IDC_T3K_PREVIEW_STOP = 152;
constexpr int IDC_T3K_PREVIEW_WAV = 153;
constexpr int IDC_T3K_PREVIEW_BROWSE = 154;
constexpr int IDC_T3K_IR_WAV = 155;
constexpr int IDC_T3K_IR_BROWSE = 156;
constexpr int IDC_T3K_IR_CLEAR = 157;

constexpr COLORREF kColorWindow = RGB(246, 248, 252);
constexpr COLORREF kColorCard = RGB(255, 255, 255);
constexpr COLORREF kColorBorder = RGB(220, 226, 235);
constexpr COLORREF kColorAccent = RGB(46, 115, 233);
constexpr COLORREF kColorAccentDark = RGB(33, 95, 204);
constexpr COLORREF kColorText = RGB(26, 31, 41);
constexpr COLORREF kColorSubtleText = RGB(88, 97, 112);
constexpr COLORREF kColorFooter = RGB(239, 243, 249);
constexpr COLORREF kColorInfo = RGB(244, 248, 255);
constexpr COLORREF kColorStatusOk = RGB(73, 193, 89);
constexpr COLORREF kColorDisabled = RGB(203, 210, 220);

enum class InputMode { None, SingleNam, Folder };

struct UiMetrics {
    RECT header{};
    RECT sectionInput{};
    RECT sectionOutput{};
    RECT sectionTail{};
    RECT sectionRecorded{};
    RECT sectionCorrective{};
    RECT sectionRefine{};
    RECT buttonArea{};
    RECT footer{};
    RECT infoBox{};
    RECT uploaderCard{};
};

HWND gBackendTabs = nullptr;
HWND gUploaderCloEdit = nullptr;
HWND gUploaderBrowseButton = nullptr;
HWND gUploaderSlotCombo = nullptr;
HWND gUploaderRescanButton = nullptr;
HWND gUploaderUploadButton = nullptr;
HWND gUploaderDevice = nullptr;
HWND gUploaderProgress = nullptr;
HWND gGp5CloEdit = nullptr;
HWND gGp5BrowseButton = nullptr;
HWND gGp5SlotCombo = nullptr;
HWND gGp5RescanButton = nullptr;
HWND gGp5UploadButton = nullptr;
HWND gGp5Device = nullptr;
HWND gGp5Progress = nullptr;
HWND gT3kKey = nullptr;
HWND gT3kConnect = nullptr;
HWND gT3kSearch = nullptr;
HWND gT3kSearchButton = nullptr;
HWND gT3kResults = nullptr;
HWND gT3kModels = nullptr;
HWND gT3kUse = nullptr;
HWND gT3kState = nullptr;
HWND gT3kPrevious = nullptr;
HWND gT3kPageLabel = nullptr;
HWND gT3kNext = nullptr;
HWND gT3kSort = nullptr;
HWND gT3kPreviewPlay = nullptr;
HWND gT3kPreviewStop = nullptr;
HWND gT3kPreviewWav = nullptr;
HWND gT3kPreviewBrowse = nullptr;
HWND gT3kIrWav = nullptr;
HWND gT3kIrBrowse = nullptr;
HWND gT3kIrClear = nullptr;
fs::path gT3kPreviewNam;
fs::path gT3kPreviewWavPath;
fs::path gT3kIrWavPath;
bool gT3kPreviewBusy = false;
ntc::NamPreviewPlayer gT3kPreviewPlayer;
ntc::tone3000::Client gT3kClient;
std::vector<ntc::tone3000::Tone> gT3kTones;
std::vector<ntc::tone3000::Model> gT3kModelItems;
bool gT3kBusy = false;
int gT3kPage = 1;
int gT3kTotalPages = 1;
int gT3kTotalResults = 0;
std::string gT3kLastQuery;
HWND gInputEdit = nullptr;
HWND gOutEdit = nullptr;
HWND gLoadFileButton = nullptr;
HWND gLoadFolderButton = nullptr;
HWND gBrowseButton = nullptr;
HWND gConvertButton = nullptr;
HWND gOpenButton = nullptr;
HWND gStatus = nullptr;
HWND gTailCombo = nullptr;
HWND gRecordedEdit = nullptr;
HWND gBrowseRecordedButton = nullptr;
HWND gCorrectiveCheck = nullptr;
HWND gCorrectiveEdit = nullptr;
HWND gBrowseCorrectiveButton = nullptr;
HWND gRefineSourceCombo = nullptr;
HWND gRefineTargetEdit = nullptr;
HWND gBrowseRefineTargetButton = nullptr;
HWND gVersion = nullptr;
HWND gInfo = nullptr;
HWND gSubtitle = nullptr;
HFONT gFont = nullptr;
HFONT gTitleFont = nullptr;
HFONT gSubtitleFont = nullptr;
HFONT gSectionFont = nullptr;
HBRUSH gWindowBrush = nullptr;
HBRUSH gCardBrush = nullptr;
HBRUSH gFooterBrush = nullptr;
HBRUSH gInfoBrush = nullptr;
HBRUSH gStatusBrush = nullptr;
HBITMAP gLogoBitmap = nullptr;
HBITMAP gSectionIcons[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
UiMetrics gUi{};
bool gBusy = false;
InputMode gInputMode = InputMode::None;
bool gUploadBusy = false;
bool gGp5UploadBusy = false;

struct UploadProgressMessage {
    int current = 0;
    int total = 0;
    std::wstring status;
};

std::wstring getText(HWND h) {
    const int len = GetWindowTextLengthW(h);
    std::wstring s(static_cast<std::size_t>(len) + 1, L'\0');
    if (len) GetWindowTextW(h, s.data(), len + 1);
    s.resize(static_cast<std::size_t>(len));
    return s;
}

void setText(HWND h, const std::wstring& s) {
    SetWindowTextW(h, s.c_str());
    if (h == gStatus || h == gVersion) {
        InvalidateRect(h, nullptr, TRUE);
        UpdateWindow(h);
    }
}

HMENU controlId(int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); }

void safeDeleteObject(HGDIOBJ obj) {
    if (obj) DeleteObject(obj);
}

void createResources() {
    gFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    gTitleFont = CreateFontW(-40, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    gSubtitleFont = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    gSectionFont = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    gWindowBrush = CreateSolidBrush(kColorWindow);
    gCardBrush = CreateSolidBrush(kColorCard);
    gFooterBrush = CreateSolidBrush(kColorFooter);
    gInfoBrush = CreateSolidBrush(kColorInfo);
    gStatusBrush = CreateSolidBrush(kColorStatusOk);

    HINSTANCE instance = GetModuleHandleW(nullptr);
    gLogoBitmap = LoadBitmapW(instance, MAKEINTRESOURCEW(IDB_LOGO));
    gSectionIcons[0] = LoadBitmapW(instance, MAKEINTRESOURCEW(IDB_ICON_INPUT));
    gSectionIcons[1] = LoadBitmapW(instance, MAKEINTRESOURCEW(IDB_ICON_OUTPUT));
    gSectionIcons[2] = LoadBitmapW(instance, MAKEINTRESOURCEW(IDB_ICON_STIMULUS));
    gSectionIcons[3] = LoadBitmapW(instance, MAKEINTRESOURCEW(IDB_ICON_REAMP));
    gSectionIcons[4] = LoadBitmapW(instance, MAKEINTRESOURCEW(IDB_ICON_RECORDED));
}

void destroyResources() {
    safeDeleteObject(gFont);
    safeDeleteObject(gTitleFont);
    safeDeleteObject(gSubtitleFont);
    safeDeleteObject(gSectionFont);
    safeDeleteObject(gWindowBrush);
    safeDeleteObject(gCardBrush);
    safeDeleteObject(gFooterBrush);
    safeDeleteObject(gInfoBrush);
    safeDeleteObject(gStatusBrush);
    safeDeleteObject(gLogoBitmap);
    gLogoBitmap = nullptr;
    for (auto& icon : gSectionIcons) {
        safeDeleteObject(icon);
        icon = nullptr;
    }
    gFont = gTitleFont = gSubtitleFont = gSectionFont = nullptr;
    gWindowBrush = gCardBrush = gFooterBrush = gInfoBrush = gStatusBrush = nullptr;
}

void applyFont(HWND h, HFONT font) {
    if (h && font) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void applyFont(HWND h) { applyFont(h, gFont); }

bool gp200UploaderTabSelected() {
    return gBackendTabs && TabCtrl_GetCurSel(gBackendTabs) == 2;
}

bool gp5UploaderTabSelected() {
    return gBackendTabs && TabCtrl_GetCurSel(gBackendTabs) == 3;
}

void showControl(HWND h, bool show) {
    if (h) ShowWindow(h, show ? SW_SHOW : SW_HIDE);
}

void showConversionUi(HWND hwnd, bool show) {
    const HWND controls[] = {
        gInputEdit, gOutEdit, gLoadFileButton, gLoadFolderButton, gBrowseButton,
        gConvertButton, gOpenButton, gTailCombo, gRecordedEdit, gBrowseRecordedButton,
        gCorrectiveCheck, gCorrectiveEdit, gBrowseCorrectiveButton, gRefineSourceCombo,
        gRefineTargetEdit, gBrowseRefineTargetButton, gInfo
    };
    for (HWND h : controls) showControl(h, show);
    for (int id : {1002,1003,1005,1006,1008,1009,1010})
        showControl(GetDlgItem(hwnd, id), show);
}

void showUploaderUi(HWND hwnd, bool show) {
    const HWND controls[] = {
        gUploaderCloEdit, gUploaderBrowseButton, gUploaderSlotCombo,
        gUploaderRescanButton, gUploaderUploadButton, gUploaderDevice, gUploaderProgress
    };
    for (HWND h : controls) showControl(h, show);
    for (int id : {1011,1012,1013,1014})
        showControl(GetDlgItem(hwnd, id), show);
}

void showGp5UploaderUi(HWND hwnd, bool show) {
    const HWND controls[] = {
        gGp5CloEdit, gGp5BrowseButton, gGp5SlotCombo,
        gGp5RescanButton, gGp5UploadButton, gGp5Device, gGp5Progress
    };
    for (HWND h : controls) showControl(h, show);
    for (int id : {1015,1016,1017,1018})
        showControl(GetDlgItem(hwnd, id), show);
}

bool tone3000TabSelected() {
    return gBackendTabs && TabCtrl_GetCurSel(gBackendTabs) == 1;
}

void showTone3000Ui(HWND hwnd, bool show) {
    const HWND controls[] = { gT3kKey, gT3kConnect, gT3kSearch, gT3kSearchButton,
        gT3kResults, gT3kModels, gT3kUse, gT3kState, gT3kPrevious, gT3kPageLabel, gT3kNext, gT3kSort,
        gT3kIrWav, gT3kIrBrowse, gT3kIrClear, gT3kPreviewWav, gT3kPreviewBrowse, gT3kPreviewPlay, gT3kPreviewStop };
    for (HWND h : controls) showControl(h, show);
    for (int id : {1019,1020,1021,1022,1023,1024,1025}) showControl(GetDlgItem(hwnd, id), show);
}

struct T3kResultMessage { bool ok=false; std::string error; };
struct T3kSearchMessage {
    bool ok=false;
    std::string error;
    std::vector<ntc::tone3000::Tone> tones;
    int page=1;
    int totalPages=1;
    int totalResults=0;
};
struct T3kModelsMessage { bool ok=false; std::string error; std::vector<ntc::tone3000::Model> models; };
struct T3kDownloadMessage { bool ok=false; std::string error; fs::path path; };
struct T3kPreviewMessage { bool ok=false; bool playing=false; bool irLoaded=false; std::string error; int sampleRate=0; int irOriginalRate=0; };

std::string utf8FromWide(const std::wstring& s) {
    if (s.empty()) return {};
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);
    std::string out(static_cast<size_t>(n),'\0');
    WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),out.data(),n,nullptr,nullptr);
    return out;
}
std::wstring wideFromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0);
    std::wstring out(static_cast<size_t>(n),L'\0');
    MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),out.data(),n);
    return out;
}

std::wstring loadSavedT3kKey() {
    wchar_t buffer[512]{};
    DWORD bytes = sizeof(buffer);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\NamToClo", L"Tone3000PublishableKey",
                                        RRF_RT_REG_SZ, nullptr, buffer, &bytes);
    if (status != ERROR_SUCCESS) return {};
    return buffer;
}

void saveT3kKey(const std::wstring& key) {
    if (key.empty()) return;
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\NamToClo", 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &hKey, nullptr) != ERROR_SUCCESS) return;
    const DWORD bytes = static_cast<DWORD>((key.size() + 1) * sizeof(wchar_t));
    RegSetValueExW(hKey, L"Tone3000PublishableKey", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(key.c_str()), bytes);
    RegCloseKey(hKey);
}


std::string loadSavedT3kRefreshToken() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\NamToClo", 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) return {};
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(hKey, L"Tone3000RefreshToken", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_BINARY || bytes == 0) {
        RegCloseKey(hKey);
        return {};
    }
    std::vector<BYTE> encrypted(bytes);
    if (RegQueryValueExW(hKey, L"Tone3000RefreshToken", nullptr, nullptr, encrypted.data(), &bytes) != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return {};
    }
    RegCloseKey(hKey);

    DATA_BLOB in{bytes, encrypted.data()};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string token(reinterpret_cast<const char*>(out.pbData), reinterpret_cast<const char*>(out.pbData) + out.cbData);
    LocalFree(out.pbData);
    return token;
}

void saveT3kRefreshToken(const std::string& token) {
    if (token.empty()) return;
    DATA_BLOB in{static_cast<DWORD>(token.size()), reinterpret_cast<BYTE*>(const_cast<char*>(token.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"NamToClo Tone3000 OAuth", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) return;
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\NamToClo", 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"Tone3000RefreshToken", 0, REG_BINARY, out.pbData, out.cbData);
        RegCloseKey(hKey);
    }
    LocalFree(out.pbData);
}

void clearSavedT3kRefreshToken() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\NamToClo", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, L"Tone3000RefreshToken");
        RegCloseKey(hKey);
    }
}


fs::path tone3000ModelsDirectory() {
    wchar_t local[MAX_PATH]{};
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local) != S_OK) return {};
    return fs::path(local) / L"NamToClo" / L"Tone3000" / L"models";
}

std::wstring normalizedT3kNamPath(const fs::path& path) {
    std::error_code ec;
    fs::path p = fs::absolute(path, ec);
    if (ec) p = path;
    std::wstring value = p.lexically_normal().wstring();
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}

std::set<std::wstring> loadPendingT3kNams() {
    std::set<std::wstring> result;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\NamToClo", 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) return result;
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(hKey, L"Tone3000PendingNams", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes < sizeof(wchar_t)) {
        RegCloseKey(hKey);
        return result;
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(hKey, L"Tone3000PendingNams", nullptr, nullptr,
                         reinterpret_cast<BYTE*>(buffer.data()), &bytes) != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return result;
    }
    RegCloseKey(hKey);
    std::wistringstream input(buffer.data());
    std::wstring line;
    while (std::getline(input, line)) {
        if (!line.empty()) result.insert(line);
    }
    return result;
}

void savePendingT3kNams(const std::set<std::wstring>& paths) {
    std::wstring value;
    for (const auto& path : paths) {
        if (!value.empty()) value += L'\n';
        value += path;
    }
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\NamToClo", 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &hKey, nullptr) != ERROR_SUCCESS) return;
    if (value.empty()) {
        RegDeleteValueW(hKey, L"Tone3000PendingNams");
    } else {
        const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
        RegSetValueExW(hKey, L"Tone3000PendingNams", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    }
    RegCloseKey(hKey);
}

bool isTone3000DownloadedNam(const fs::path& path) {
    if (path.empty()) return false;
    std::wstring ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    if (ext != L".nam") return false;
    const fs::path dir = tone3000ModelsDirectory();
    if (dir.empty()) return false;
    return normalizedT3kNamPath(path.parent_path()) == normalizedT3kNamPath(dir);
}

void registerTemporaryT3kNam(const fs::path& namPath) {
    if (!isTone3000DownloadedNam(namPath)) return;
    auto pending = loadPendingT3kNams();
    pending.insert(normalizedT3kNamPath(namPath));
    savePendingT3kNams(pending);
}

void preserveConvertedT3kNam(const fs::path& namPath) {
    if (!isTone3000DownloadedNam(namPath)) return;
    auto pending = loadPendingT3kNams();
    pending.erase(normalizedT3kNamPath(namPath));
    savePendingT3kNams(pending);
}

void cleanupUnconvertedT3kNams() {
    auto pending = loadPendingT3kNams();
    for (const auto& value : pending) {
        const fs::path p(value);
        if (!isTone3000DownloadedNam(p)) continue;
        std::error_code ec;
        fs::remove(p, ec);
    }
    savePendingT3kNams({});
}

void syncT3kConnectButton() {
    if (gT3kConnect) setText(gT3kConnect, gT3kClient.connected() ? L"Disconnect" : L"Connect");
}

void updateT3kPagingControls() {
    if (gT3kPageLabel) {
        setText(gT3kPageLabel, L"Page " + std::to_wstring(gT3kPage) + L" of " + std::to_wstring(gT3kTotalPages));
    }
    if (gT3kPrevious) EnableWindow(gT3kPrevious, !gT3kBusy && gT3kClient.connected() && gT3kPage > 1);
    if (gT3kNext) EnableWindow(gT3kNext, !gT3kBusy && gT3kClient.connected() && gT3kPage < gT3kTotalPages);
}

void setT3kBusy(bool busy) {
    gT3kBusy=busy;
    EnableWindow(gT3kKey,!busy && !gT3kClient.connected());
    EnableWindow(gT3kConnect,!busy);
    syncT3kConnectButton();
    EnableWindow(gT3kSearch,!busy && gT3kClient.connected());
    EnableWindow(gT3kSearchButton,!busy && gT3kClient.connected());
    EnableWindow(gT3kSort,!busy && gT3kClient.connected());
    EnableWindow(gT3kResults,!busy && gT3kClient.connected());
    EnableWindow(gT3kModels,!busy && !gT3kModelItems.empty());
    EnableWindow(gT3kUse,!busy && !gT3kModelItems.empty());
    updateT3kPagingControls();
}

void startT3kAuth(HWND hwnd) {
    if (gT3kBusy) return;
    if (gT3kClient.connected()) {
        gT3kClient.disconnect();
        clearSavedT3kRefreshToken();
        gT3kTones.clear(); gT3kModelItems.clear(); gT3kPage=1; gT3kTotalPages=1; gT3kTotalResults=0;
        SendMessageW(gT3kResults,LB_RESETCONTENT,0,0); SendMessageW(gT3kModels,CB_RESETCONTENT,0,0);
        setText(gT3kState,L"Disconnected. Your API key is still saved.");
        setText(gStatus,L"Tone3000 disconnected.");
        setT3kBusy(false);
        return;
    }
    const auto keyText=getText(gT3kKey);
    const auto key=utf8FromWide(keyText);
    gT3kClient.setPublishableKey(key);
    saveT3kKey(keyText);
    setT3kBusy(true); setText(gT3kState,L"Opening Tone3000 authorization in your browser...");
    std::thread([hwnd]{ auto* m=new T3kResultMessage; m->ok=gT3kClient.authenticateInteractive(m->error); PostMessageW(hwnd,WM_APP_T3K_AUTH_DONE,0,reinterpret_cast<LPARAM>(m)); }).detach();
}

void startT3kAutoLogin(HWND hwnd) {
    if (gT3kBusy || gT3kClient.connected()) return;
    const auto keyText = getText(gT3kKey);
    const auto refreshToken = loadSavedT3kRefreshToken();
    if (keyText.empty() || refreshToken.empty()) return;
    gT3kClient.setPublishableKey(utf8FromWide(keyText));
    setT3kBusy(true);
    setText(gT3kState,L"Restoring previous Tone3000 session...");
    std::thread([hwnd, refreshToken]{
        auto* m = new T3kResultMessage;
        m->ok = gT3kClient.restoreSession(refreshToken, m->error);
        PostMessageW(hwnd, WM_APP_T3K_AUTOLOGIN_DONE, 0, reinterpret_cast<LPARAM>(m));
    }).detach();
}

std::string currentT3kSort() {
    if (!gT3kSort) return "best-match";
    const int sel = static_cast<int>(SendMessageW(gT3kSort, CB_GETCURSEL, 0, 0));
    switch (sel) {
    case 1: return "newest";
    case 2: return "oldest";
    case 3: return "trending";
    case 4: return "downloads-all-time";
    default: return "best-match";
    }
}

void startT3kSearchPage(HWND hwnd, int page, bool newQuery) {
    if (gT3kBusy || !gT3kClient.connected()) return;
    if (newQuery) {
        gT3kLastQuery=utf8FromWide(getText(gT3kSearch));
        gT3kPage=1;
        page=1;
    }
    if (gT3kLastQuery.empty()) gT3kLastQuery=utf8FromWide(getText(gT3kSearch));
    if (page < 1) page=1;
    if (gT3kTotalPages > 0 && !newQuery && page > gT3kTotalPages) return;
    const auto q=gT3kLastQuery;
    const auto sort=currentT3kSort();
    setT3kBusy(true);
    setText(gT3kState,L"Searching NAM A2 captures on Tone3000...");
    std::thread([hwnd,q,page,sort]{
        auto* m=new T3kSearchMessage;
        m->page=page;
        m->ok=gT3kClient.searchNamTones(q,page,sort,m->tones,m->totalPages,m->totalResults,m->error);
        PostMessageW(hwnd,WM_APP_T3K_SEARCH_DONE,0,reinterpret_cast<LPARAM>(m));
    }).detach();
}

void startT3kSearch(HWND hwnd) { startT3kSearchPage(hwnd,1,true); }
void startT3kPrevious(HWND hwnd) { if (gT3kPage>1) startT3kSearchPage(hwnd,gT3kPage-1,false); }
void startT3kNext(HWND hwnd) { if (gT3kPage<gT3kTotalPages) startT3kSearchPage(hwnd,gT3kPage+1,false); }

void startT3kModels(HWND hwnd) {
    if (gT3kBusy) return;
    int sel=static_cast<int>(SendMessageW(gT3kResults,LB_GETCURSEL,0,0));
    if(sel<0 || sel>=static_cast<int>(gT3kTones.size())) return;
    auto id=gT3kTones[static_cast<size_t>(sel)].id; setT3kBusy(true); setText(gT3kState,L"Loading models for selected tone...");
    std::thread([hwnd,id]{ auto* m=new T3kModelsMessage; m->ok=gT3kClient.listModels(id,m->models,m->error); PostMessageW(hwnd,WM_APP_T3K_MODELS_DONE,0,reinterpret_cast<LPARAM>(m)); }).detach();
}

std::wstring safeTone3000FileStem(const std::string& modelName) {
    std::wstring name = wideFromUtf8(modelName);
    if (name.empty()) name = L"Tone3000 NAM";

    // Windows file names cannot contain these characters. Preserve the model
    // name as closely as possible so the downloaded NAM and generated CLO
    // share the same human-readable base name.
    constexpr wchar_t invalid[] = L"<>:\\|?*\"";
    for (auto& c : name) {
        if (c < 32 || std::wcschr(invalid, c) != nullptr) c = L'_';
    }
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
    if (name.empty()) name = L"Tone3000 NAM";

    // Avoid Windows reserved device names while keeping the visible model name.
    std::wstring upper = name;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towupper(c));
    });
    static const std::array<const wchar_t*, 22> reserved = {
        L"CON", L"PRN", L"AUX", L"NUL",
        L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"
    };
    for (const auto* r : reserved) {
        if (upper == r) { name += L"_"; break; }
    }
    return name;
}

void startT3kDownload(HWND hwnd) {
    if(gT3kBusy) return;
    int sel=static_cast<int>(SendMessageW(gT3kModels,CB_GETCURSEL,0,0)); if(sel<0||sel>=static_cast<int>(gT3kModelItems.size())) return;
    const auto model=gT3kModelItems[static_cast<size_t>(sel)];
    fs::path dir=tone3000ModelsDirectory();
    fs::path dest=dir/(safeTone3000FileStem(model.name)+L".nam");
    setT3kBusy(true); setText(gT3kState,L"Downloading selected NAM...");
    std::thread([hwnd,model,dest]{ auto* m=new T3kDownloadMessage; m->path=dest; m->ok=gT3kClient.downloadModel(model,dest,m->error); PostMessageW(hwnd,WM_APP_T3K_DOWNLOAD_DONE,0,reinterpret_cast<LPARAM>(m)); }).detach();
}

void stopT3kPreview();
void startT3kPreview(HWND hwnd, bool forceLoad);

void chooseT3kPreviewWav(HWND owner) {
    wchar_t file[32768]{};
    if (!gT3kPreviewWavPath.empty()) {
        const auto current = gT3kPreviewWavPath.wstring();
        wcsncpy_s(file, std::size(file), current.c_str(), _TRUNCATE);
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Mono WAV audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = L"wav";
    if (!GetOpenFileNameW(&ofn)) return;

    stopT3kPreview();
    gT3kPreviewWavPath = fs::path(file);
    setText(gT3kPreviewWav, gT3kPreviewWavPath.wstring());
    // Force a reload because the source audio changed, while keeping the
    // currently selected/downloaded NAM.
    if (!gT3kPreviewNam.empty()) startT3kPreview(owner, true);
}

void chooseT3kIrWav(HWND owner) {
    wchar_t file[32768]{};
    if (!gT3kIrWavPath.empty()) {
        const auto current = gT3kIrWavPath.wstring();
        wcsncpy_s(file, std::size(file), current.c_str(), _TRUNCATE);
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Cabinet IR WAV (*.wav)\0*.wav\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = L"wav";
    if (!GetOpenFileNameW(&ofn)) return;

    stopT3kPreview();
    gT3kIrWavPath = fs::path(file);
    setText(gT3kIrWav, gT3kIrWavPath.wstring());
    EnableWindow(gT3kIrClear, TRUE);
    setText(gT3kState, L"Cabinet IR selected. Non-48 kHz IRs are resampled internally to 48 kHz.");
    if (!gT3kPreviewNam.empty() && !gT3kPreviewWavPath.empty()) startT3kPreview(owner, true);
}

void clearT3kIr(HWND owner) {
    stopT3kPreview();
    gT3kIrWavPath.clear();
    setText(gT3kIrWav, L"");
    EnableWindow(gT3kIrClear, FALSE);
    setText(gT3kState, L"Cabinet IR cleared. Preview will play the NAM without an IR.");
    if (!gT3kPreviewNam.empty() && !gT3kPreviewWavPath.empty()) startT3kPreview(owner, true);
}

void stopT3kPreview(){
    gT3kPreviewPlayer.stop();
    if(gT3kPreviewStop) EnableWindow(gT3kPreviewStop,FALSE);
}
void startT3kPreview(HWND hwnd,bool forceLoad){
    if(gT3kPreviewBusy||gT3kPreviewNam.empty()) return;
    const fs::path input=gT3kPreviewWavPath;
    std::error_code ec;
    if(input.empty()){
        setText(gT3kState,L"NAM loaded. Choose a mono WAV for the real-time preview.");
        EnableWindow(gT3kPreviewPlay,FALSE);
        return;
    }
    if(!fs::exists(input,ec)||ec){
        setText(gT3kState,L"Preview WAV not found. Choose another WAV file.");
        EnableWindow(gT3kPreviewPlay,FALSE);
        return;
    }
    if(!forceLoad && gT3kPreviewPlayer.ready()){
        std::string error;
        if(gT3kPreviewPlayer.play(error)){
            EnableWindow(gT3kPreviewStop,TRUE);
            setText(gT3kState,L"Real-time NAM preview playing.");
        }else{
            setText(gT3kState,L"Preview failed: "+wideFromUtf8(error));
        }
        return;
    }
    gT3kPreviewBusy=true;
    EnableWindow(gT3kPreviewPlay,FALSE);
    EnableWindow(gT3kPreviewStop,FALSE);
    setText(gT3kState, gT3kIrWavPath.empty()
        ? L"Loading NAM and selected WAV for real-time preview..."
        : L"Loading NAM + cabinet IR for real-time preview...");
    const fs::path nam=gT3kPreviewNam;
    const fs::path ir=gT3kIrWavPath;
    std::thread([hwnd,nam,input,ir]{
        auto* m=new T3kPreviewMessage;
        m->ok=gT3kPreviewPlayer.load(nam,input,ir,m->error);
        if(m->ok){
            m->sampleRate=gT3kPreviewPlayer.sampleRate();
            m->irLoaded=gT3kPreviewPlayer.irLoaded();
            m->irOriginalRate=gT3kPreviewPlayer.irOriginalSampleRate();
            m->playing=gT3kPreviewPlayer.play(m->error);
            if(!m->playing) m->ok=false;
        }
        PostMessageW(hwnd,WM_APP_T3K_PREVIEW_DONE,0,reinterpret_cast<LPARAM>(m));
    }).detach();
}

void refreshUploaderDetection() {
    const auto d = ntc::gp200::detectGp200Midi();
    setText(gUploaderDevice, ntc::gp200::describeDetection(d));
    if (!gUploadBusy)
        EnableWindow(gUploaderUploadButton, d.inputFound && d.outputFound ? TRUE : FALSE);
}

void refreshGp5Detection() {
    const auto d = ntc::gp5::detectGp5Midi();
    setText(gGp5Device, ntc::gp5::describeDetection(d));
    if (!gGp5UploadBusy)
        EnableWindow(gGp5UploadButton, d.inputFound && d.outputFound ? TRUE : FALSE);
}

void updateBackendUi() {
    HWND hwnd = gBackendTabs ? GetParent(gBackendTabs) : nullptr;
    const int selected = gBackendTabs ? TabCtrl_GetCurSel(gBackendTabs) : 0;
    const bool t3k = selected == 1;
    const bool gp200 = selected == 2;
    const bool gp5 = selected == 3;
    showConversionUi(hwnd, selected == 0);
    showTone3000Ui(hwnd, t3k);
    showUploaderUi(hwnd, gp200);
    showGp5UploaderUi(hwnd, gp5);
    if (t3k) {
        setText(gSubtitle, L"Browse NAM captures on Tone3000 and load a model directly into the converter.");
        setText(gStatus, gT3kClient.connected() ? L"Tone3000 connected." : L"Enter your Tone3000 publishable API key and connect.");
        setT3kBusy(gT3kBusy);
    } else if (gp200) {
        setText(gSubtitle, L"Upload CLO files directly to a GP-200 SnapTone slot via USB MIDI.");
        refreshUploaderDetection();
        if (!gUploadBusy) setText(gStatus, L"GP-200 Uploader ready.");
    } else if (gp5) {
        setText(gSubtitle, L"Adapt a CLO to the GP-5/GP-50 A128/B512 transfer format and upload it to SnapTone 51-80.");
        refreshGp5Detection();
        if (!gGp5UploadBusy) setText(gStatus, L"GP-5/GP-50 Uploader ready.");
    } else {
        setText(gSubtitle, L"Convert one NAM or batch-convert every NAM in a selected folder.");
        setText(gInfo,
            L"Place nam_input_wav.wav next to NamToClo.exe. The original stimulus is always used.\r\n"
            L"Tail / Reamp and Corrective IR are optional. Tone Match is applied by default.");
        if (!gBusy) setText(gStatus, L"Ready to convert.");
    }
    if (hwnd) InvalidateRect(hwnd, nullptr, TRUE);
}

void enableControls(bool enable) {
    gBusy = !enable;
    EnableWindow(gBackendTabs, enable);
    EnableWindow(gLoadFileButton, enable);
    EnableWindow(gLoadFolderButton, enable);
    EnableWindow(gBrowseButton, enable);
    EnableWindow(gConvertButton, enable);
    EnableWindow(gOpenButton, enable);
    EnableWindow(gTailCombo, enable);
    EnableWindow(gCorrectiveCheck, enable);
    EnableWindow(gRefineSourceCombo, enable);
    if (!enable) {
        EnableWindow(gRefineTargetEdit, FALSE);
        EnableWindow(gBrowseRefineTargetButton, FALSE);
    }
    if (!enable) {
        EnableWindow(gRecordedEdit, FALSE);
        EnableWindow(gBrowseRecordedButton, FALSE);
        EnableWindow(gCorrectiveEdit, FALSE);
        EnableWindow(gBrowseCorrectiveButton, FALSE);
    }
    InvalidateRect(GetParent(gConvertButton), nullptr, FALSE);
}

bool isNamFile(const fs::path& p) {
    std::wstring ext = p.extension().wstring();
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext == L".nam";
}

void setSingleNam(const fs::path& p) {
    if (p.empty()) return;
    if (!isNamFile(p)) {
        MessageBoxW(nullptr, L"Please select a .nam file.", L"NAM to CLO", MB_ICONWARNING | MB_OK);
        return;
    }
    gInputMode = InputMode::SingleNam;
    setText(gInputEdit, p.wstring());
    if (getText(gOutEdit).empty()) setText(gOutEdit, p.parent_path().wstring());
    setText(gStatus, L"Single-file mode. Ready to convert.");
}

void setNamFolder(const fs::path& p) {
    if (p.empty()) return;
    std::error_code ec;
    if (!fs::is_directory(p, ec) || ec) {
        MessageBoxW(nullptr, L"Please select a valid folder.", L"NAM to CLO", MB_ICONWARNING | MB_OK);
        return;
    }

    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(p, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec) && !ec && isNamFile(entry.path())) ++count;
        ec.clear();
    }
    if (count == 0) {
        MessageBoxW(nullptr, L"The selected folder contains no .nam files.", L"NAM to CLO", MB_ICONINFORMATION | MB_OK);
        return;
    }

    gInputMode = InputMode::Folder;
    setText(gInputEdit, p.wstring());
    if (getText(gOutEdit).empty()) setText(gOutEdit, p.wstring());
    setText(gStatus, L"Batch mode: " + std::to_wstring(count) + L" NAM file(s) found.");
}

void chooseNam(HWND owner) {
    wchar_t file[32768]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Neural Amp Model (*.nam)\0*.nam\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = L"nam";
    if (GetOpenFileNameW(&ofn)) setSingleNam(fs::path(file));
}

int CALLBACK browseCallback(HWND hwnd, UINT msg, LPARAM, LPARAM data) {
    if (msg == BFFM_INITIALIZED && data) SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

bool chooseFolder(HWND owner, const wchar_t* title, const std::wstring& current, fs::path& selected) {
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = browseCallback;
    bi.lParam = reinterpret_cast<LPARAM>(current.c_str());
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    wchar_t path[MAX_PATH]{};
    const bool ok = SHGetPathFromIDListW(pidl, path) != FALSE;
    if (ok) selected = fs::path(path);
    CoTaskMemFree(pidl);
    return ok;
}

void chooseNamFolder(HWND owner) {
    fs::path selected;
    const std::wstring current = getText(gInputEdit);
    if (chooseFolder(owner, L"Select folder containing NAM files", current, selected)) setNamFolder(selected);
}

void chooseOutput(HWND owner) {
    fs::path selected;
    const std::wstring current = getText(gOutEdit);
    if (chooseFolder(owner, L"Select output folder", current, selected)) setText(gOutEdit, selected.wstring());
}





void chooseRecordedAudio(HWND owner) {
    wchar_t file[32768]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"WAV audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = L"wav";
    if (GetOpenFileNameW(&ofn)) setText(gRecordedEdit, fs::path(file).wstring());
}

void chooseCorrectiveIr(HWND owner) {
    wchar_t file[32768]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"WAV impulse response (*.wav)\0*.wav\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = L"wav";
    if (GetOpenFileNameW(&ofn)) setText(gCorrectiveEdit, fs::path(file).wstring());
}

void chooseRefineTarget(HWND owner) {
    wchar_t file[32768]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Refinement test WAV (*.wav)\0*.wav\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = L"wav";
    if (GetOpenFileNameW(&ofn)) setText(gRefineTargetEdit, fs::path(file).wstring());
}

ntc::TailMode selectedTailMode() {
    return SendMessageW(gTailCombo, CB_GETCURSEL, 0, 0) == 1
        ? ntc::TailMode::RecordedAudio
        : ntc::TailMode::PresetAudio;
}

void updateTailControls() {
    if (gp200UploaderTabSelected() || gp5UploaderTabSelected()) return;
    // Release UI always uses the official/original 50 s stimulus. Tail/Reamp
    // remains selectable between the original tail and a recorded WAV.
    EnableWindow(gTailCombo, TRUE);
    const bool recorded = selectedTailMode() == ntc::TailMode::RecordedAudio;
    EnableWindow(gRecordedEdit, recorded ? TRUE : FALSE);
    EnableWindow(gBrowseRecordedButton, recorded ? TRUE : FALSE);

    EnableWindow(gCorrectiveCheck, TRUE);
    const bool correctiveEnabled = SendMessageW(gCorrectiveCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(gCorrectiveEdit, correctiveEnabled ? TRUE : FALSE);
    EnableWindow(gBrowseCorrectiveButton, correctiveEnabled ? TRUE : FALSE);

    EnableWindow(gRefineSourceCombo, TRUE);
    const bool customToneMatch = SendMessageW(gRefineSourceCombo, CB_GETCURSEL, 0, 0) == 1;
    EnableWindow(gRefineTargetEdit, customToneMatch ? TRUE : FALSE);
    EnableWindow(gBrowseRefineTargetButton, customToneMatch ? TRUE : FALSE);
}

void postStatus(HWND hwnd, const std::wstring& s) {
    auto* copy = new std::wstring(s);
    PostMessageW(hwnd, WM_APP_STATUS, 0, reinterpret_cast<LPARAM>(copy));
}

void startConversion(HWND hwnd) {
    if (gBusy) return;
    const fs::path input = getText(gInputEdit);
    const fs::path out = getText(gOutEdit);
    if (input.empty() || gInputMode == InputMode::None) {
        MessageBoxW(hwnd, L"Select a NAM file or a folder containing NAM files first.", L"NAM to CLO", MB_ICONINFORMATION | MB_OK);
        return;
    }
    if (out.empty()) {
        MessageBoxW(hwnd, L"Select an output folder.", L"NAM to CLO", MB_ICONINFORMATION | MB_OK);
        return;
    }

    ntc::StimulusConfig stimulus;
    stimulus.tailMode = selectedTailMode();
    stimulus.recordedAudio = fs::path(getText(gRecordedEdit));
    if (stimulus.tailMode == ntc::TailMode::RecordedAudio
        && stimulus.recordedAudio.empty()) {
        MessageBoxW(hwnd, L"Select a Recorded Audio WAV file.", L"NAM to CLO", MB_ICONINFORMATION | MB_OK);
        return;
    }

    ntc::CorrectiveIrConfig correction;
    correction.enabled = SendMessageW(gCorrectiveCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    correction.wav = fs::path(getText(gCorrectiveEdit));
    if (correction.enabled && correction.wav.empty()) {
        MessageBoxW(hwnd, L"Select a Corrective IR WAV file.", L"NAM to CLO", MB_ICONINFORMATION | MB_OK);
        return;
    }

    ntc::CloRefineConfig refine;
    refine.enabled = true;
    refine.passes = 4;
    const bool customToneMatch = SendMessageW(gRefineSourceCombo, CB_GETCURSEL, 0, 0) == 1;
    refine.referenceWav = customToneMatch ? fs::path(getText(gRefineTargetEdit)) : fs::path{};
    if (customToneMatch && refine.referenceWav.empty()) {
        MessageBoxW(hwnd, L"Select a custom Tone Match reference WAV file.", L"NAM to CLO", MB_ICONINFORMATION | MB_OK);
        return;
    }

    enableControls(false);
    ntc::NativeConverterConfig nativeConfig;
    if (gInputMode == InputMode::SingleNam) {
        setText(gStatus, L"Starting conversion...");
        std::thread([hwnd, input, out, stimulus, correction, refine, nativeConfig] {
            auto result = std::make_unique<ntc::ConversionResult>(
                ntc::convertNamToClo(input, out, stimulus, correction, refine, nativeConfig,
                    [hwnd](const std::wstring& text) { postStatus(hwnd, text); }));
            PostMessageW(hwnd, WM_APP_DONE_SINGLE, 0, reinterpret_cast<LPARAM>(result.release()));
        }).detach();
    } else {
        setText(gStatus, L"Starting batch conversion...");
        std::thread([hwnd, input, out, stimulus, correction, refine, nativeConfig] {
            auto result = std::make_unique<ntc::BatchConversionResult>(
                ntc::convertNamFolderToClo(input, out, stimulus, correction, refine, nativeConfig,
                    [hwnd](const std::wstring& text) { postStatus(hwnd, text); }));
            PostMessageW(hwnd, WM_APP_DONE_BATCH, 0, reinterpret_cast<LPARAM>(result.release()));
        }).detach();
    }

}

void openOutputFolder(HWND hwnd) {
    const std::wstring out = getText(gOutEdit);
    if (out.empty()) return;
    ShellExecuteW(hwnd, L"open", out.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void chooseUploaderClo(HWND hwnd) {
    wchar_t file[32768]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.lpstrFilter = L"Sound Clone files (*.clo)\0*.clo\0All files (*.*)\0*.*\0\0";
    ofn.lpstrDefExt = L"clo";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) {
        setText(gUploaderCloEdit, file);
        setText(gStatus, L"CLO selected. Choose a destination slot and press Upload to GP-200.");
    }
}

void startUploader(HWND hwnd) {
    if (gUploadBusy) return;
    const std::wstring clo = getText(gUploaderCloEdit);
    if (clo.empty()) {
        MessageBoxW(hwnd, L"Select a .clo file first.", L"GP-200 Uploader", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const int slot = static_cast<int>(SendMessageW(gUploaderSlotCombo, CB_GETCURSEL, 0, 0));
    if (slot < 0 || slot >= 10) {
        MessageBoxW(hwnd, L"Select a destination SnapTone slot.", L"GP-200 Uploader", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const auto d = ntc::gp200::detectGp200Midi();
    if (!d.inputFound || !d.outputFound) {
        const auto msg = ntc::gp200::describeDetection(d);
        setText(gUploaderDevice, msg);
        MessageBoxW(hwnd, msg.c_str(), L"GP-200 Uploader", MB_OK | MB_ICONWARNING);
        return;
    }

    gUploadBusy = true;
    EnableWindow(gBackendTabs, FALSE);
    EnableWindow(gUploaderBrowseButton, FALSE);
    EnableWindow(gUploaderSlotCombo, FALSE);
    EnableWindow(gUploaderRescanButton, FALSE);
    EnableWindow(gUploaderUploadButton, FALSE);
    SendMessageW(gUploaderProgress, PBM_SETRANGE32, 0, 45);
    SendMessageW(gUploaderProgress, PBM_SETPOS, 0, 0);
    setText(gStatus, L"Starting Sound Clone upload...");

    std::thread([hwnd, clo, slot] {
        auto result = ntc::gp200::uploadCloToGp200(fs::path(clo), slot,
            [hwnd](int current, int total, const std::wstring& status) {
                auto* m = new UploadProgressMessage{};
                m->current = current;
                m->total = total;
                m->status = status;
                PostMessageW(hwnd, WM_APP_UPLOAD_PROGRESS, 0, reinterpret_cast<LPARAM>(m));
            });
        auto* posted = new ntc::gp200::UploadResult(std::move(result));
        PostMessageW(hwnd, WM_APP_UPLOAD_DONE, 0, reinterpret_cast<LPARAM>(posted));
    }).detach();
}

void chooseGp5Clo(HWND hwnd) {
    wchar_t file[32768]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.lpstrFilter = L"CLO files (*.clo)\0*.clo\0All files (*.*)\0*.*\0\0";
    ofn.lpstrDefExt = L"clo";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) {
        setText(gGp5CloEdit, file);
        setText(gStatus, L"CLO selected. Choose SnapTone 51-80 and press Upload to GP-5/GP-50.");
    }
}

void startGp5Uploader(HWND hwnd) {
    if (gGp5UploadBusy) return;
    const std::wstring clo = getText(gGp5CloEdit);
    if (clo.empty()) {
        MessageBoxW(hwnd, L"Select a .clo file first.", L"GP-5/GP-50 Uploader", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const int selection = static_cast<int>(SendMessageW(gGp5SlotCombo, CB_GETCURSEL, 0, 0));
    if (selection < 0 || selection >= 30) {
        MessageBoxW(hwnd, L"Select a destination SnapTone slot (51-80).", L"GP-5/GP-50 Uploader", MB_OK | MB_ICONINFORMATION);
        return;
    }
    // The combo contains visible SnapTone 51..80, while the GP-5 protocol
    // uses a zero-based slot byte. Therefore selection 0 -> slot 50 (SnapTone 51).
    const int slot = selection + 50;

    const auto d = ntc::gp5::detectGp5Midi();
    if (!d.inputFound || !d.outputFound) {
        const auto msg = ntc::gp5::describeDetection(d);
        setText(gGp5Device, msg);
        MessageBoxW(hwnd, msg.c_str(), L"GP-5/GP-50 Uploader", MB_OK | MB_ICONWARNING);
        return;
    }

    gGp5UploadBusy = true;
    EnableWindow(gBackendTabs, FALSE);
    EnableWindow(gGp5BrowseButton, FALSE);
    EnableWindow(gGp5SlotCombo, FALSE);
    EnableWindow(gGp5RescanButton, FALSE);
    EnableWindow(gGp5UploadButton, FALSE);
    SendMessageW(gGp5Progress, PBM_SETRANGE32, 0, 146);
    SendMessageW(gGp5Progress, PBM_SETPOS, 0, 0);
    setText(gStatus, L"Preparing GP-5/GP-50 A128/B512 transfer...");

    std::thread([hwnd, clo, slot] {
        auto result = ntc::gp5::uploadCloToGp5(fs::path(clo), slot,
            [hwnd](int current, int total, const std::wstring& status) {
                auto* m = new UploadProgressMessage{};
                m->current = current;
                m->total = total;
                m->status = status;
                PostMessageW(hwnd, WM_APP_GP5_UPLOAD_PROGRESS, 0, reinterpret_cast<LPARAM>(m));
            });
        auto* posted = new ntc::gp5::UploadResult(std::move(result));
        PostMessageW(hwnd, WM_APP_GP5_UPLOAD_DONE, 0, reinterpret_cast<LPARAM>(posted));
    }).detach();
}

void moveCtrl(HWND h, int x, int y, int w, int hgt) {
    if (h) MoveWindow(h, x, y, w, hgt, TRUE);
}

void computeLayout(int clientW, int clientH) {
    const int margin = 24;
    const int gap = 7;
    const int footerH = 38;

    gUi.header = RECT{ margin, 12, clientW - margin, 124 };

    int y = 128;
    gUi.sectionInput = RECT{ margin, y, clientW - margin, y + 76 }; y += 76 + gap;
    gUi.sectionOutput = RECT{ margin, y, clientW - margin, y + 70 }; y += 70 + gap;
    gUi.sectionTail = RECT{ margin, y, clientW - margin, y + 66 }; y += 66 + gap;
    gUi.sectionRecorded = RECT{ margin, y, clientW - margin, y + 105 }; y += 105 + gap;
    gUi.sectionCorrective = RECT{ margin, y, clientW - margin, y + 86 }; y += 86 + gap;
    gUi.sectionRefine = RECT{ margin, y, clientW - margin, y + 108 }; y += 108 + gap;
    gUi.buttonArea = RECT{ margin, y, clientW - margin, y + 38 };
    gUi.footer = RECT{ 0, clientH - footerH, clientW, clientH };
    gUi.infoBox = RECT{ gUi.sectionRecorded.left + 108, gUi.sectionRecorded.top + 62,
                        gUi.sectionRecorded.right - 16, gUi.sectionRecorded.top + 96 };
    gUi.uploaderCard = RECT{ margin, 145, clientW - margin, 675 };
}

void layoutControls(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    computeLayout(rc.right - rc.left, rc.bottom - rc.top);

    const int contentX = gUi.sectionInput.left + 108;
    const int sectionRightInset = 16;

    HWND title = GetDlgItem(hwnd, 1001);
    if (title) moveCtrl(title, 118, 18, 340, 42);
    if (gSubtitle) moveCtrl(gSubtitle, 120, 62, rc.right - 150, 22);
    moveCtrl(gBackendTabs, 120, 88, rc.right - 150, 32);

    // Keep a visible gap between each label and its field/control.
    moveCtrl(GetDlgItem(hwnd, 1002), contentX, gUi.sectionInput.top + 8, 230, 20);
    const int inputButtonX = gUi.sectionInput.right - sectionRightInset - 244;
    const int inputEditW = inputButtonX - 12 - contentX;
    moveCtrl(gInputEdit, contentX, gUi.sectionInput.top + 35, inputEditW, 28);
    moveCtrl(gLoadFileButton, inputButtonX, gUi.sectionInput.top + 31, 116, 32);
    moveCtrl(gLoadFolderButton, gUi.sectionInput.right - sectionRightInset - 120, gUi.sectionInput.top + 31, 116, 32);

    moveCtrl(GetDlgItem(hwnd, 1003), contentX, gUi.sectionOutput.top + 7, 170, 20);
    const int outputEditW = (gUi.sectionOutput.right - sectionRightInset) - (contentX + 142);
    moveCtrl(gOutEdit, contentX, gUi.sectionOutput.top + 33, outputEditW, 28);
    moveCtrl(gBrowseButton, gUi.sectionOutput.right - sectionRightInset - 112, gUi.sectionOutput.top + 29, 112, 32);

    moveCtrl(GetDlgItem(hwnd, 1005), contentX, gUi.sectionTail.top + 6, 210, 20);
    moveCtrl(gTailCombo, contentX, gUi.sectionTail.top + 32, 490, 180);

    moveCtrl(GetDlgItem(hwnd, 1006), contentX, gUi.sectionRecorded.top + 6, 430, 20);
    const int recordedEditW = (gUi.sectionRecorded.right - sectionRightInset) - (contentX + 150);
    moveCtrl(gRecordedEdit, contentX, gUi.sectionRecorded.top + 32, recordedEditW, 28);
    moveCtrl(gBrowseRecordedButton, gUi.sectionRecorded.right - sectionRightInset - 124, gUi.sectionRecorded.top + 28, 124, 32);
    moveCtrl(gInfo, gUi.infoBox.left + 36, gUi.infoBox.top + 5,
             (gUi.infoBox.right - gUi.infoBox.left) - 44, 24);

    moveCtrl(GetDlgItem(hwnd, 1008), contentX, gUi.sectionCorrective.top + 8, 180, 22);
    moveCtrl(gCorrectiveCheck, contentX, gUi.sectionCorrective.top + 31, 170, 24);
    const int correctiveEditX = contentX + 180;
    const int correctiveEditW = (gUi.sectionCorrective.right - sectionRightInset - 124 - 8) - correctiveEditX;
    moveCtrl(gCorrectiveEdit, correctiveEditX, gUi.sectionCorrective.top + 29, correctiveEditW, 28);
    moveCtrl(gBrowseCorrectiveButton, gUi.sectionCorrective.right - sectionRightInset - 124, gUi.sectionCorrective.top + 27, 124, 32);

    moveCtrl(GetDlgItem(hwnd, 1009), contentX, gUi.sectionRefine.top + 7, 360, 22);
    moveCtrl(gRefineSourceCombo, contentX, gUi.sectionRefine.top + 32, 360, 120);
    moveCtrl(GetDlgItem(hwnd, 1010), contentX, gUi.sectionRefine.top + 58, 430, 20);
    const int refineTargetEditW = (gUi.sectionRefine.right - sectionRightInset - 124 - 8) - contentX;
    moveCtrl(gRefineTargetEdit, contentX, gUi.sectionRefine.top + 78, refineTargetEditW, 28);
    moveCtrl(gBrowseRefineTargetButton, gUi.sectionRefine.right - sectionRightInset - 124, gUi.sectionRefine.top + 76, 124, 32);

    const int center = rc.right / 2;
    moveCtrl(gConvertButton, center - 222, gUi.buttonArea.top, 200, 36);
    moveCtrl(gOpenButton, center - 10, gUi.buttonArea.top, 200, 36);

    const int ux = gUi.uploaderCard.left + 34;
    const int ur = gUi.uploaderCard.right - 34;
    moveCtrl(GetDlgItem(hwnd, 1011), ux, gUi.uploaderCard.top + 28, 220, 22);
    moveCtrl(GetDlgItem(hwnd,1019), ux, gUi.uploaderCard.top + 18, 220, 24);
    moveCtrl(gT3kKey, ux, gUi.uploaderCard.top + 46, ur - ux - 136, 30);
    moveCtrl(gT3kConnect, ur - 124, gUi.uploaderCard.top + 42, 124, 34);
    moveCtrl(GetDlgItem(hwnd,1020), ux, gUi.uploaderCard.top + 88, 220, 24);
    moveCtrl(gT3kSearch, ux, gUi.uploaderCard.top + 116, ur - ux - 136, 30);
    moveCtrl(gT3kSearchButton, ur - 124, gUi.uploaderCard.top + 112, 124, 34);
    moveCtrl(GetDlgItem(hwnd,1021), ux, gUi.uploaderCard.top + 158, 72, 24);
    moveCtrl(GetDlgItem(hwnd,1023), ux + 82, gUi.uploaderCard.top + 158, 52, 24);
    moveCtrl(gT3kSort, ux + 136, gUi.uploaderCard.top + 154, 190, 180);
    moveCtrl(gT3kPrevious, ur - 330, gUi.uploaderCard.top + 154, 96, 28);
    moveCtrl(gT3kPageLabel, ur - 226, gUi.uploaderCard.top + 158, 122, 22);
    moveCtrl(gT3kNext, ur - 96, gUi.uploaderCard.top + 154, 96, 28);
    moveCtrl(gT3kResults, ux, gUi.uploaderCard.top + 184, ur - ux, 142);
    moveCtrl(GetDlgItem(hwnd,1022), ux, gUi.uploaderCard.top + 338, 220, 24);
    moveCtrl(gT3kModels, ux, gUi.uploaderCard.top + 364, ur - ux - 220, 220);
    moveCtrl(gT3kUse, ur - 208, gUi.uploaderCard.top + 360, 208, 36);
    moveCtrl(gT3kState, ux, gUi.uploaderCard.top + 408, ur - ux, 24);

    // Cabinet IR row. Keep it above Preview so the signal chain reads NAM -> IR -> output.
    moveCtrl(GetDlgItem(hwnd,1025), ux, gUi.uploaderCard.top + 440, 92, 22);
    moveCtrl(gT3kIrWav, ux + 94, gUi.uploaderCard.top + 436, ur - ux - 94 - 300, 30);
    moveCtrl(gT3kIrBrowse, ur - 290, gUi.uploaderCard.top + 436, 116, 32);
    moveCtrl(gT3kIrClear, ur - 166, gUi.uploaderCard.top + 436, 74, 32);

    // Preview moved down to make room for the cabinet IR loader.
    moveCtrl(GetDlgItem(hwnd,1024), ux, gUi.uploaderCard.top + 486, 92, 22);
    moveCtrl(gT3kPreviewWav, ux + 94, gUi.uploaderCard.top + 482, ur - ux - 94 - 386, 30);
    moveCtrl(gT3kPreviewBrowse, ur - 376, gUi.uploaderCard.top + 482, 116, 32);
    moveCtrl(gT3kPreviewPlay, ur - 252, gUi.uploaderCard.top + 482, 152, 32);
    moveCtrl(gT3kPreviewStop, ur - 92, gUi.uploaderCard.top + 482, 92, 32);

    moveCtrl(gUploaderCloEdit, ux, gUi.uploaderCard.top + 56, ur - ux - 136, 30);
    moveCtrl(gUploaderBrowseButton, ur - 124, gUi.uploaderCard.top + 52, 124, 34);
    moveCtrl(GetDlgItem(hwnd, 1012), ux, gUi.uploaderCard.top + 112, 220, 22);
    moveCtrl(gUploaderSlotCombo, ux, gUi.uploaderCard.top + 140, 310, 260);
    moveCtrl(GetDlgItem(hwnd, 1013), ux, gUi.uploaderCard.top + 196, 220, 22);
    moveCtrl(gUploaderDevice, ux, gUi.uploaderCard.top + 224, ur - ux - 136, 28);
    moveCtrl(gUploaderRescanButton, ur - 124, gUi.uploaderCard.top + 220, 124, 34);
    moveCtrl(GetDlgItem(hwnd, 1014), ux, gUi.uploaderCard.top + 276, 220, 22);
    moveCtrl(gUploaderProgress, ux, gUi.uploaderCard.top + 306, ur - ux, 22);
    moveCtrl(gUploaderUploadButton, center - 120, gUi.uploaderCard.top + 340, 240, 38);

    moveCtrl(GetDlgItem(hwnd, 1015), ux, gUi.uploaderCard.top + 28, 330, 22);
    moveCtrl(gGp5CloEdit, ux, gUi.uploaderCard.top + 56, ur - ux - 136, 30);
    moveCtrl(gGp5BrowseButton, ur - 124, gUi.uploaderCard.top + 52, 124, 34);
    moveCtrl(GetDlgItem(hwnd, 1016), ux, gUi.uploaderCard.top + 112, 260, 22);
    moveCtrl(gGp5SlotCombo, ux, gUi.uploaderCard.top + 140, 310, 260);
    moveCtrl(GetDlgItem(hwnd, 1017), ux, gUi.uploaderCard.top + 196, 220, 22);
    moveCtrl(gGp5Device, ux, gUi.uploaderCard.top + 224, ur - ux - 136, 28);
    moveCtrl(gGp5RescanButton, ur - 124, gUi.uploaderCard.top + 220, 124, 34);
    moveCtrl(GetDlgItem(hwnd, 1018), ux, gUi.uploaderCard.top + 276, 220, 22);
    moveCtrl(gGp5Progress, ux, gUi.uploaderCard.top + 306, ur - ux, 22);
    moveCtrl(gGp5UploadButton, center - 120, gUi.uploaderCard.top + 340, 240, 38);

    moveCtrl(gStatus, 44, gUi.footer.top + 8, rc.right - 220, 22);
    moveCtrl(gVersion, rc.right - 140, gUi.footer.top + 8, 110, 22);
}

void createSectionLabel(HWND hwnd, int id, const wchar_t* text) {
    HWND h = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE,
                           0, 0, 100, 24, hwnd, controlId(id), nullptr, nullptr);
    applyFont(h, gSectionFont);
}

void createUi(HWND hwnd) {
    createResources();

    const std::wstring appHeader = L"NAM to CLO";
    HWND title = CreateWindowW(L"STATIC", appHeader.c_str(), WS_CHILD | WS_VISIBLE,
                               0, 0, 100, 30, hwnd, controlId(1001), nullptr, nullptr);
    applyFont(title, gTitleFont);

    gSubtitle = CreateWindowW(L"STATIC", L"Convert one NAM or batch-convert every NAM in a selected folder.",
                              WS_CHILD | WS_VISIBLE, 0, 0, 100, 20, hwnd,
                              controlId(IDC_SUBTITLE), nullptr, nullptr);
    applyFont(gSubtitle, gSubtitleFont);

    gBackendTabs = CreateWindowExW(0, WC_TABCONTROLW, L"",
                                    WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FIXEDWIDTH,
                                    0, 0, 100, 32, hwnd, controlId(IDC_BACKEND_TABS), nullptr, nullptr);
    applyFont(gBackendTabs);
    SendMessageW(gBackendTabs, TCM_SETITEMSIZE, 0, MAKELPARAM(190, 25));
    TCITEMW tab{};
    tab.mask = TCIF_TEXT;
    tab.pszText = const_cast<LPWSTR>(L"Convert to CLO");
    TabCtrl_InsertItem(gBackendTabs, 0, &tab);
    tab.pszText = const_cast<LPWSTR>(L"Tone3000");
    TabCtrl_InsertItem(gBackendTabs, 1, &tab);
    tab.pszText = const_cast<LPWSTR>(L"GP-200 Uploader");
    TabCtrl_InsertItem(gBackendTabs, 2, &tab);
    tab.pszText = const_cast<LPWSTR>(L"GP-5/GP-50 Uploader");
    TabCtrl_InsertItem(gBackendTabs, 3, &tab);
    TabCtrl_SetCurSel(gBackendTabs, 0);

    createSectionLabel(hwnd, 1002, L"Input NAM or folder");
    createSectionLabel(hwnd, 1003, L"Output folder");
    createSectionLabel(hwnd, 1005, L"Tail / Reamp source");
    createSectionLabel(hwnd, 1006, L"Recorded WAV (adapted automatically to 20.000 s)");
    createSectionLabel(hwnd, 1008, L"Corrective IR");
    createSectionLabel(hwnd, 1009, L"Tone Match");
    createSectionLabel(hwnd, 1010, L"Custom Tone Match reference WAV (first 20 s used)");

    gInputEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                                 0, 0, 100, 30, hwnd, controlId(IDC_INPUT_PATH), nullptr, nullptr);
    applyFont(gInputEdit);
    gLoadFileButton = CreateWindowW(L"BUTTON", L"Load NAM...", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                    0, 0, 110, 34, hwnd, controlId(IDC_LOAD_FILE), nullptr, nullptr);
    gLoadFolderButton = CreateWindowW(L"BUTTON", L"Load Folder...", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                      0, 0, 120, 34, hwnd, controlId(IDC_LOAD_FOLDER), nullptr, nullptr);

    gOutEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                               0, 0, 100, 30, hwnd, controlId(IDC_OUTPUT_PATH), nullptr, nullptr);
    applyFont(gOutEdit);
    gBrowseButton = CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                  0, 0, 120, 34, hwnd, controlId(IDC_BROWSE_OUTPUT), nullptr, nullptr);

    gTailCombo = CreateWindowW(L"COMBOBOX", L"",
                               WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                               0, 0, 100, 120, hwnd, controlId(IDC_TAIL_MODE), nullptr, nullptr);
    applyFont(gTailCombo);
    for (const auto mode : { ntc::TailMode::PresetAudio, ntc::TailMode::RecordedAudio }) {
        SendMessageW(gTailCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(ntc::tailModeDisplayName(mode)));
    }
    SendMessageW(gTailCombo, CB_SETCURSEL, 0, 0);

    gRecordedEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                                    0, 0, 100, 30, hwnd, controlId(IDC_RECORDED_PATH), nullptr, nullptr);
    applyFont(gRecordedEdit);
    gBrowseRecordedButton = CreateWindowW(L"BUTTON", L"Browse WAV...", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                          0, 0, 120, 34, hwnd, controlId(IDC_BROWSE_RECORDED), nullptr, nullptr);

    gCorrectiveCheck = CreateWindowW(L"BUTTON", L"Apply corrective IR",
                                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                     0, 0, 170, 24, hwnd, controlId(IDC_APPLY_CORRECTIVE_IR), nullptr, nullptr);
    applyFont(gCorrectiveCheck);
    gCorrectiveEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                                      0, 0, 100, 30, hwnd, controlId(IDC_CORRECTIVE_IR_PATH), nullptr, nullptr);
    applyFont(gCorrectiveEdit);
    gBrowseCorrectiveButton = CreateWindowW(L"BUTTON", L"Browse WAV...",
                                             WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                             0, 0, 120, 34, hwnd, controlId(IDC_BROWSE_CORRECTIVE_IR), nullptr, nullptr);

    gRefineSourceCombo = CreateWindowW(L"COMBOBOX", L"",
                                        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                        0, 0, 520, 120, hwnd, controlId(IDC_REFINE_SOURCE), nullptr, nullptr);
    applyFont(gRefineSourceCombo);
    SendMessageW(gRefineSourceCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Reference audio (default)"));
    SendMessageW(gRefineSourceCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Custom WAV..."));
    SendMessageW(gRefineSourceCombo, CB_SETCURSEL, 0, 0);
    gRefineTargetEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                                        0, 0, 100, 30, hwnd, controlId(IDC_REFINE_TARGET_PATH), nullptr, nullptr);
    applyFont(gRefineTargetEdit);
    gBrowseRefineTargetButton = CreateWindowW(L"BUTTON", L"Browse WAV...",
                                               WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                               0, 0, 124, 32, hwnd, controlId(IDC_BROWSE_REFINE_TARGET), nullptr, nullptr);

    createSectionLabel(hwnd, 1011, L"Sound Clone file (.clo)");
    createSectionLabel(hwnd, 1012, L"Destination SnapTone slot");
    createSectionLabel(hwnd, 1013, L"USB MIDI device");
    createSectionLabel(hwnd, 1014, L"Transfer progress");

    createSectionLabel(hwnd, 1019, L"Publishable API key");
    gT3kKey = CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|ES_AUTOHSCROLL,0,0,0,0,hwnd,controlId(IDC_T3K_KEY),nullptr,nullptr); applyFont(gT3kKey);
    gT3kConnect = CreateWindowW(L"BUTTON",L"Connect",WS_CHILD|BS_OWNERDRAW,0,0,0,0,hwnd,controlId(IDC_T3K_CONNECT),nullptr,nullptr); applyFont(gT3kConnect);
    createSectionLabel(hwnd, 1020, L"Search NAM captures (A2)");
    gT3kSearch = CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|ES_AUTOHSCROLL,0,0,0,0,hwnd,controlId(IDC_T3K_SEARCH),nullptr,nullptr); applyFont(gT3kSearch);
    gT3kSearchButton = CreateWindowW(L"BUTTON",L"Search",WS_CHILD|BS_OWNERDRAW,0,0,0,0,hwnd,controlId(IDC_T3K_SEARCH_BUTTON),nullptr,nullptr); applyFont(gT3kSearchButton);
    createSectionLabel(hwnd, 1021, L"Results");
    createSectionLabel(hwnd, 1023, L"Sort by");
    gT3kSort = CreateWindowW(L"COMBOBOX",L"",WS_CHILD|CBS_DROPDOWNLIST|WS_VSCROLL,0,0,0,0,hwnd,controlId(IDC_T3K_SORT),nullptr,nullptr); applyFont(gT3kSort);
    for (const wchar_t* option : {L"Best match", L"Newest", L"Oldest", L"Trending", L"Most downloaded"})
        SendMessageW(gT3kSort, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option));
    SendMessageW(gT3kSort, CB_SETCURSEL, 0, 0);
    gT3kPrevious = CreateWindowW(L"BUTTON",L"Previous",WS_CHILD|BS_OWNERDRAW,0,0,0,0,hwnd,controlId(IDC_T3K_PREVIOUS),nullptr,nullptr); applyFont(gT3kPrevious);
    gT3kPageLabel = CreateWindowW(L"STATIC",L"Page 1 of 1",WS_CHILD|SS_CENTER,0,0,0,0,hwnd,controlId(IDC_T3K_PAGE),nullptr,nullptr); applyFont(gT3kPageLabel);
    gT3kNext = CreateWindowW(L"BUTTON",L"Next",WS_CHILD|BS_OWNERDRAW,0,0,0,0,hwnd,controlId(IDC_T3K_NEXT),nullptr,nullptr); applyFont(gT3kNext);
    gT3kResults = CreateWindowExW(WS_EX_CLIENTEDGE,L"LISTBOX",L"",WS_CHILD|LBS_NOTIFY|WS_VSCROLL,0,0,0,0,hwnd,controlId(IDC_T3K_RESULTS),nullptr,nullptr); applyFont(gT3kResults);
    createSectionLabel(hwnd, 1022, L"NAM model");
    gT3kModels = CreateWindowW(L"COMBOBOX",L"",WS_CHILD|CBS_DROPDOWNLIST|WS_VSCROLL,0,0,0,0,hwnd,controlId(IDC_T3K_MODELS),nullptr,nullptr); applyFont(gT3kModels);
    gT3kUse = CreateWindowW(L"BUTTON",L"Load selected NAM",WS_CHILD|BS_OWNERDRAW,0,0,0,0,hwnd,controlId(IDC_T3K_USE),nullptr,nullptr); applyFont(gT3kUse);
    gT3kState = CreateWindowW(L"STATIC",L"Not connected.",WS_CHILD|SS_LEFTNOWORDWRAP,0,0,0,0,hwnd,controlId(IDC_T3K_STATE),nullptr,nullptr); applyFont(gT3kState);
    createSectionLabel(hwnd, 1025, L"Cabinet IR");
    gT3kIrWav = CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|ES_AUTOHSCROLL|ES_READONLY,0,0,0,0,hwnd,controlId(IDC_T3K_IR_WAV),nullptr,nullptr); applyFont(gT3kIrWav);
    gT3kIrBrowse = CreateWindowW(L"BUTTON",L"Browse IR...",WS_CHILD|BS_OWNERDRAW,0,0,0,0,hwnd,controlId(IDC_T3K_IR_BROWSE),nullptr,nullptr); applyFont(gT3kIrBrowse);
    gT3kIrClear = CreateWindowW(L"BUTTON",L"Clear",WS_CHILD|BS_OWNERDRAW,0,0,0,0,hwnd,controlId(IDC_T3K_IR_CLEAR),nullptr,nullptr); applyFont(gT3kIrClear); EnableWindow(gT3kIrClear,FALSE);

    createSectionLabel(hwnd, 1024, L"Preview WAV");
    gT3kPreviewWav = CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|ES_AUTOHSCROLL|ES_READONLY,0,0,0,0,hwnd,controlId(IDC_T3K_PREVIEW_WAV),nullptr,nullptr); applyFont(gT3kPreviewWav);
    gT3kPreviewBrowse = CreateWindowW(L"BUTTON",L"Browse WAV...",WS_CHILD|BS_OWNERDRAW,0,0,0,0,hwnd,controlId(IDC_T3K_PREVIEW_BROWSE),nullptr,nullptr); applyFont(gT3kPreviewBrowse);
    gT3kPreviewPlay = CreateWindowW(L"BUTTON",L"Replay preview",WS_CHILD|BS_OWNERDRAW,0,0,0,0,hwnd,controlId(IDC_T3K_PREVIEW_PLAY),nullptr,nullptr); applyFont(gT3kPreviewPlay); EnableWindow(gT3kPreviewPlay,FALSE);
    gT3kPreviewStop = CreateWindowW(L"BUTTON",L"Stop",WS_CHILD|BS_OWNERDRAW,0,0,0,0,hwnd,controlId(IDC_T3K_PREVIEW_STOP),nullptr,nullptr); applyFont(gT3kPreviewStop); EnableWindow(gT3kPreviewStop,FALSE);
    const auto savedT3kKey = loadSavedT3kKey();
    if (!savedT3kKey.empty()) setText(gT3kKey, savedT3kKey);
    updateT3kPagingControls();
    if (!savedT3kKey.empty() && !loadSavedT3kRefreshToken().empty()) startT3kAutoLogin(hwnd);

    gUploaderCloEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                       WS_CHILD | ES_AUTOHSCROLL | ES_READONLY,
                                       0, 0, 100, 30, hwnd, controlId(IDC_UPLOADER_CLO_PATH), nullptr, nullptr);
    applyFont(gUploaderCloEdit);
    gUploaderBrowseButton = CreateWindowW(L"BUTTON", L"Browse CLO...", WS_CHILD | BS_OWNERDRAW,
                                          0, 0, 124, 34, hwnd, controlId(IDC_UPLOADER_BROWSE), nullptr, nullptr);
    applyFont(gUploaderBrowseButton);
    gUploaderSlotCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
                                       0, 0, 310, 240, hwnd, controlId(IDC_UPLOADER_SLOT), nullptr, nullptr);
    applyFont(gUploaderSlotCombo);
    for (const wchar_t* name : { L"SnapTone 1 (AMP 1)", L"SnapTone 2 (AMP 2)", L"SnapTone 3 (AMP 3)",
                                 L"SnapTone 4 (AMP 4)", L"SnapTone 5 (AMP 5)", L"SnapTone 6 (DIST 1)",
                                 L"SnapTone 7 (DIST 2)", L"SnapTone 8 (DIST 3)", L"SnapTone 9 (DIST 4)",
                                 L"SnapTone 10 (DIST 5)" })
        SendMessageW(gUploaderSlotCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
    SendMessageW(gUploaderSlotCombo, CB_SETCURSEL, 0, 0);
    gUploaderDevice = CreateWindowW(L"STATIC", L"GP-200 MIDI not scanned yet.", WS_CHILD,
                                    0, 0, 100, 24, hwnd, controlId(IDC_UPLOADER_DEVICE), nullptr, nullptr);
    applyFont(gUploaderDevice);
    gUploaderRescanButton = CreateWindowW(L"BUTTON", L"Rescan", WS_CHILD | BS_OWNERDRAW,
                                          0, 0, 124, 34, hwnd, controlId(IDC_UPLOADER_RESCAN), nullptr, nullptr);
    applyFont(gUploaderRescanButton);
    gUploaderProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | PBS_SMOOTH,
                                        0, 0, 100, 22, hwnd, controlId(IDC_UPLOADER_PROGRESS), nullptr, nullptr);
    SendMessageW(gUploaderProgress, PBM_SETRANGE32, 0, 45);
    SendMessageW(gUploaderProgress, PBM_SETPOS, 0, 0);
    gUploaderUploadButton = CreateWindowW(L"BUTTON", L"Upload to GP-200", WS_CHILD | BS_OWNERDRAW,
                                          0, 0, 240, 38, hwnd, controlId(IDC_UPLOADER_UPLOAD), nullptr, nullptr);
    applyFont(gUploaderUploadButton);

    createSectionLabel(hwnd, 1015, L"CLO file (adapted automatically to GP-5 B512)");
    createSectionLabel(hwnd, 1016, L"Destination SnapTone slot (51-80)");
    createSectionLabel(hwnd, 1017, L"USB MIDI device");
    createSectionLabel(hwnd, 1018, L"Transfer progress");

    gGp5CloEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | ES_AUTOHSCROLL | ES_READONLY,
                                  0, 0, 100, 30, hwnd, controlId(IDC_GP5_CLO_PATH), nullptr, nullptr);
    applyFont(gGp5CloEdit);
    gGp5BrowseButton = CreateWindowW(L"BUTTON", L"Browse CLO...", WS_CHILD | BS_OWNERDRAW,
                                     0, 0, 124, 34, hwnd, controlId(IDC_GP5_BROWSE), nullptr, nullptr);
    applyFont(gGp5BrowseButton);
    gGp5SlotCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
                                  0, 0, 310, 260, hwnd, controlId(IDC_GP5_SLOT), nullptr, nullptr);
    applyFont(gGp5SlotCombo);
    for (int i = 51; i <= 80; ++i) {
        const std::wstring name = L"SnapTone " + std::to_wstring(i);
        SendMessageW(gGp5SlotCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    }
    SendMessageW(gGp5SlotCombo, CB_SETCURSEL, 0, 0);
    gGp5Device = CreateWindowW(L"STATIC", L"GP-5/GP-50 MIDI not scanned yet.", WS_CHILD,
                               0, 0, 100, 24, hwnd, controlId(IDC_GP5_DEVICE), nullptr, nullptr);
    applyFont(gGp5Device);
    gGp5RescanButton = CreateWindowW(L"BUTTON", L"Rescan", WS_CHILD | BS_OWNERDRAW,
                                     0, 0, 124, 34, hwnd, controlId(IDC_GP5_RESCAN), nullptr, nullptr);
    applyFont(gGp5RescanButton);
    gGp5Progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | PBS_SMOOTH,
                                   0, 0, 100, 22, hwnd, controlId(IDC_GP5_PROGRESS), nullptr, nullptr);
    SendMessageW(gGp5Progress, PBM_SETRANGE32, 0, 146);
    SendMessageW(gGp5Progress, PBM_SETPOS, 0, 0);
    gGp5UploadButton = CreateWindowW(L"BUTTON", L"Upload to GP-5/GP-50", WS_CHILD | BS_OWNERDRAW,
                                     0, 0, 240, 38, hwnd, controlId(IDC_GP5_UPLOAD), nullptr, nullptr);
    applyFont(gGp5UploadButton);

    gInfo = CreateWindowW(L"STATIC",
                          L"CLO files will be created as Mono, PCM16, 44.1 kHz.\r\n"
                          L"Audio will be trimmed or padded to exactly 20.000 seconds.",
                          WS_CHILD | WS_VISIBLE,
                          0, 0, 100, 40, hwnd, controlId(IDC_INFO), nullptr, nullptr);
    applyFont(gInfo);

    gConvertButton = CreateWindowW(L"BUTTON", L"Convert", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                   0, 0, 150, 42, hwnd, controlId(IDC_CONVERT), nullptr, nullptr);
    applyFont(gConvertButton);
    gOpenButton = CreateWindowW(L"BUTTON", L"Open output folder", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                0, 0, 180, 42, hwnd, controlId(IDC_OPEN_OUTPUT), nullptr, nullptr);
    applyFont(gOpenButton);

    gStatus = CreateWindowW(L"STATIC", L"Ready to convert.", WS_CHILD | WS_VISIBLE,
                            0, 0, 100, 22, hwnd, controlId(IDC_STATUS), nullptr, nullptr);
    applyFont(gStatus);

    const std::wstring versionLabel = std::wstring(L"Version ") + ntc::kVersion;
    gVersion = CreateWindowW(L"STATIC", versionLabel.c_str(), WS_CHILD | WS_VISIBLE | SS_RIGHT,
                             0, 0, 110, 22, hwnd, controlId(IDC_VERSION), nullptr, nullptr);
    applyFont(gVersion);

    layoutControls(hwnd);
    updateBackendUi();
    updateTailControls();
    DragAcceptFiles(hwnd, TRUE);
}

void drawRoundedRect(HDC hdc, const RECT& rc, COLORREF fill, COLORREF border, int radius = 18) {
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HBRUSH brush = CreateSolidBrush(fill);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void fillRect(HDC hdc, const RECT& rc, COLORREF fill) {
    HBRUSH brush = CreateSolidBrush(fill);
    FillRect(hdc, &rc, brush);
    DeleteObject(brush);
}

void drawBitmap(HDC hdc, HBITMAP bitmap, int x, int y) {
    if (!bitmap) return;
    BITMAP bm{};
    GetObjectW(bitmap, sizeof(bm), &bm);
    HDC mem = CreateCompatibleDC(hdc);
    HGDIOBJ old = SelectObject(mem, bitmap);
    BitBlt(hdc, x, y, bm.bmWidth, bm.bmHeight, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
}

void drawSectionIcon(HDC hdc, const RECT& rc, int kind) {
    if (kind < 0 || kind >= 5 || !gSectionIcons[kind]) return;
    BITMAP bm{};
    GetObjectW(gSectionIcons[kind], sizeof(bm), &bm);
    const int x = rc.left + ((rc.right - rc.left) - bm.bmWidth) / 2;
    const int y = rc.top + ((rc.bottom - rc.top) - bm.bmHeight) / 2;
    drawBitmap(hdc, gSectionIcons[kind], x, y);
}

void drawSectionCard(HDC hdc, const RECT& rc, int iconKind) {
    drawRoundedRect(hdc, rc, kColorCard, kColorBorder, 18);
    RECT iconRect{ rc.left + 14, rc.top + 9, rc.left + 66, rc.top + 61 };
    drawSectionIcon(hdc, iconRect, iconKind);
}

void drawInfoBox(HDC hdc) {
    drawRoundedRect(hdc, gUi.infoBox, kColorInfo, RGB(210, 223, 247), 12);
    RECT iconRc{ gUi.infoBox.left + 14, gUi.infoBox.top + 12, gUi.infoBox.left + 34, gUi.infoBox.top + 32 };
    HPEN pen = CreatePen(PS_SOLID, 2, kColorAccent);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Ellipse(hdc, iconRc.left, iconRc.top, iconRc.right, iconRc.bottom);
    MoveToEx(hdc, (iconRc.left + iconRc.right) / 2, iconRc.top + 8, nullptr);
    LineTo(hdc, (iconRc.left + iconRc.right) / 2, iconRc.bottom - 7);
    MoveToEx(hdc, (iconRc.left + iconRc.right) / 2, iconRc.top + 4, nullptr);
    LineTo(hdc, (iconRc.left + iconRc.right) / 2 + 1, iconRc.top + 4);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void paintBackground(HWND hwnd, HDC hdc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    fillRect(hdc, rc, kColorWindow);

    drawBitmap(hdc, gLogoBitmap, 28, 18);

    if (tone3000TabSelected() || gp200UploaderTabSelected() || gp5UploaderTabSelected()) {
        drawRoundedRect(hdc, gUi.uploaderCard, kColorCard, kColorBorder, 18);
    } else {
        drawSectionCard(hdc, gUi.sectionInput, 0);
        drawSectionCard(hdc, gUi.sectionOutput, 1);
        drawSectionCard(hdc, gUi.sectionTail, 3);
        drawSectionCard(hdc, gUi.sectionRecorded, 4);
        drawSectionCard(hdc, gUi.sectionCorrective, 4);
        drawSectionCard(hdc, gUi.sectionRefine, 2);
        drawInfoBox(hdc);
    }
    fillRect(hdc, gUi.footer, kColorFooter);

    RECT statusDot{ 18, gUi.footer.top + 9, 32, gUi.footer.top + 23 };
    HBRUSH dotBrush = CreateSolidBrush(kColorStatusOk);
    HGDIOBJ oldBrush = SelectObject(hdc, dotBrush);
    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    Ellipse(hdc, statusDot.left, statusDot.top, statusDot.right, statusDot.bottom);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(dotBrush);
}

void drawButton(DRAWITEMSTRUCT* dis) {
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool selected = (dis->itemState & ODS_SELECTED) != 0;
    const int id = static_cast<int>(dis->CtlID);
    const bool primary = id == IDC_CONVERT || id == IDC_UPLOADER_UPLOAD || id == IDC_GP5_UPLOAD;

    COLORREF fill = primary ? (selected ? kColorAccentDark : kColorAccent) : kColorCard;
    COLORREF border = primary ? (selected ? kColorAccentDark : kColorAccentDark) : kColorAccent;
    COLORREF text = primary ? RGB(255, 255, 255) : kColorAccentDark;
    if (disabled) {
        fill = primary ? kColorDisabled : RGB(247, 248, 250);
        border = RGB(208, 214, 224);
        text = RGB(145, 152, 164);
    }

    RECT rc = dis->rcItem;
    drawRoundedRect(dis->hDC, rc, fill, border, 16);

    std::wstring label = getText(dis->hwndItem);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, text);
    SelectObject(dis->hDC, gSectionFont ? gSectionFont : gFont);

    if (primary) {
        POINT pts[3] = {
            { rc.left + 34, rc.top + 14 },
            { rc.left + 34, rc.bottom - 14 },
            { rc.left + 50, (rc.top + rc.bottom) / 2 }
        };
        HBRUSH triBrush = CreateSolidBrush(text);
        HGDIOBJ oldBrush = SelectObject(dis->hDC, triBrush);
        HGDIOBJ oldPen = SelectObject(dis->hDC, GetStockObject(NULL_PEN));
        Polygon(dis->hDC, pts, 3);
        SelectObject(dis->hDC, oldPen);
        SelectObject(dis->hDC, oldBrush);
        DeleteObject(triBrush);
        rc.left += 60;
    }

    DrawTextW(dis->hDC, label.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if ((dis->itemState & ODS_FOCUS) != 0) {
        RECT focus = dis->rcItem;
        InflateRect(&focus, -5, -5);
        DrawFocusRect(dis->hDC, &focus);
    }
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        createUi(hwnd);
        setText(gStatus, L"Ready to convert.");
        return 0;
    }
    case WM_SIZE:
        layoutControls(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        HWND ctrl = reinterpret_cast<HWND>(lParam);
        if (ctrl == gStatus || ctrl == gVersion) {
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, kColorFooter);
            SetTextColor(hdc, kColorSubtleText);
            return reinterpret_cast<LRESULT>(gFooterBrush);
        }
        if (ctrl == gInfo) {
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, kColorInfo);
            SetTextColor(hdc, kColorSubtleText);
            return reinterpret_cast<LRESULT>(gInfoBrush);
        }
        if (ctrl == gT3kState) {
            // This status text changes frequently. Give it an opaque card
            // background so old transparent text is fully erased on update.
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, kColorCard);
            SetTextColor(hdc, kColorText);
            return reinterpret_cast<LRESULT>(gCardBrush);
        }
        if (ctrl == gSubtitle || ctrl == GetDlgItem(hwnd, 1001)
            || ctrl == GetDlgItem(hwnd, 1002) || ctrl == GetDlgItem(hwnd, 1003) || ctrl == GetDlgItem(hwnd, 1004)
            || ctrl == GetDlgItem(hwnd, 1005) || ctrl == GetDlgItem(hwnd, 1006) || ctrl == GetDlgItem(hwnd, 1007)
            || ctrl == GetDlgItem(hwnd, 1008) || ctrl == GetDlgItem(hwnd, 1009) || ctrl == GetDlgItem(hwnd, 1010)
            || ctrl == GetDlgItem(hwnd, 1011) || ctrl == GetDlgItem(hwnd, 1012) || ctrl == GetDlgItem(hwnd, 1013)
            || ctrl == GetDlgItem(hwnd, 1014) || ctrl == gUploaderDevice
            || ctrl == GetDlgItem(hwnd, 1015) || ctrl == GetDlgItem(hwnd, 1016)
            || ctrl == GetDlgItem(hwnd, 1017) || ctrl == GetDlgItem(hwnd, 1018) || ctrl == gGp5Device
            || ctrl == GetDlgItem(hwnd,1019) || ctrl == GetDlgItem(hwnd,1020) || ctrl == GetDlgItem(hwnd,1021) || ctrl == GetDlgItem(hwnd,1022) || ctrl == GetDlgItem(hwnd,1023) || ctrl == GetDlgItem(hwnd,1024)) {
            SetTextColor(hdc, ctrl == gSubtitle ? kColorSubtleText : kColorText);
            return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
        }
        break;
    }
    case WM_DRAWITEM:
        drawButton(reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        paintBackground(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NOTIFY: {
        const auto* hdr = reinterpret_cast<LPNMHDR>(lParam);
        if (hdr && hdr->hwndFrom == gBackendTabs && hdr->code == TCN_SELCHANGE) {
            updateBackendUi();
            updateTailControls();
            return 0;
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_LOAD_FILE: chooseNam(hwnd); return 0;
        case IDC_LOAD_FOLDER: chooseNamFolder(hwnd); return 0;
        case IDC_BROWSE_OUTPUT: chooseOutput(hwnd); return 0;
        case IDC_BROWSE_RECORDED: chooseRecordedAudio(hwnd); return 0;
        case IDC_BROWSE_CORRECTIVE_IR: chooseCorrectiveIr(hwnd); return 0;
        case IDC_BROWSE_REFINE_TARGET: chooseRefineTarget(hwnd); return 0;
        case IDC_APPLY_CORRECTIVE_IR:
            if (HIWORD(wParam) == BN_CLICKED) updateTailControls();
            return 0;
        case IDC_REFINE_SOURCE:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                if (SendMessageW(gRefineSourceCombo, CB_GETCURSEL, 0, 0) == 0)
                    setText(gRefineTargetEdit, L"");
                updateTailControls();
            }
            return 0;
        case IDC_TAIL_MODE:
            if (HIWORD(wParam) == CBN_SELCHANGE) updateTailControls();
            return 0;
        case IDC_CONVERT: startConversion(hwnd); return 0;
        case IDC_OPEN_OUTPUT: openOutputFolder(hwnd); return 0;
        case IDC_T3K_CONNECT: startT3kAuth(hwnd); return 0;
        case IDC_T3K_SEARCH_BUTTON: startT3kSearch(hwnd); return 0;
        case IDC_T3K_PREVIOUS: startT3kPrevious(hwnd); return 0;
        case IDC_T3K_NEXT: startT3kNext(hwnd); return 0;
        case IDC_T3K_SORT:
            if (HIWORD(wParam)==CBN_SELCHANGE && gT3kClient.connected() && !gT3kBusy && !getText(gT3kSearch).empty())
                startT3kSearchPage(hwnd,1,true);
            return 0;
        case IDC_T3K_SEARCH:
            if (HIWORD(wParam)==EN_UPDATE) return 0;
            break;
        case IDC_T3K_RESULTS:
            if (HIWORD(wParam)==LBN_SELCHANGE) startT3kModels(hwnd);
            return 0;
        case IDC_T3K_USE: startT3kDownload(hwnd); return 0;
        case IDC_T3K_IR_BROWSE: chooseT3kIrWav(hwnd); return 0;
        case IDC_T3K_IR_CLEAR: clearT3kIr(hwnd); return 0;
        case IDC_T3K_PREVIEW_BROWSE: chooseT3kPreviewWav(hwnd); return 0;
        case IDC_T3K_PREVIEW_PLAY: startT3kPreview(hwnd,false); return 0;
        case IDC_T3K_PREVIEW_STOP: stopT3kPreview(); return 0;
        case IDC_UPLOADER_BROWSE: chooseUploaderClo(hwnd); return 0;
        case IDC_UPLOADER_RESCAN:
            refreshUploaderDetection();
            setText(gStatus, getText(gUploaderDevice));
            return 0;
        case IDC_UPLOADER_UPLOAD: startUploader(hwnd); return 0;
        case IDC_GP5_BROWSE: chooseGp5Clo(hwnd); return 0;
        case IDC_GP5_RESCAN:
            refreshGp5Detection();
            setText(gStatus, getText(gGp5Device));
            return 0;
        case IDC_GP5_UPLOAD: startGp5Uploader(hwnd); return 0;
        default: break;
        }
        break;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        wchar_t path[32768]{};
        if (DragQueryFileW(drop, 0, path, static_cast<UINT>(std::size(path)))) {
            fs::path p(path);
            std::error_code ec;
            if (gp200UploaderTabSelected() || gp5UploaderTabSelected()) {
                std::wstring ext = p.extension().wstring();
                for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
                if (ext == L".clo") {
                    if (gp5UploaderTabSelected()) {
                        setText(gGp5CloEdit, p.wstring());
                        setText(gStatus, L"CLO selected. Choose SnapTone 51-80 and press Upload to GP-5/GP-50.");
                    } else {
                        setText(gUploaderCloEdit, p.wstring());
                        setText(gStatus, L"CLO selected. Choose a destination slot and press Upload to GP-200.");
                    }
                } else {
                    const bool gp5 = gp5UploaderTabSelected();
                    MessageBoxW(hwnd,
                                gp5 ? L"The GP-5/GP-50 Uploader accepts .clo files." : L"The GP-200 Uploader accepts .clo files.",
                                gp5 ? L"GP-5/GP-50 Uploader" : L"GP-200 Uploader", MB_OK | MB_ICONINFORMATION);
                }
            } else if (fs::is_directory(p, ec) && !ec) setNamFolder(p);
            else setSingleNam(p);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_APP_STATUS: {
        std::unique_ptr<std::wstring> s(reinterpret_cast<std::wstring*>(lParam));
        if (s) setText(gStatus, *s);
        return 0;
    }
    case WM_APP_T3K_AUTH_DONE: {
        std::unique_ptr<T3kResultMessage> m(reinterpret_cast<T3kResultMessage*>(lParam));
        if(m && m->ok){
            saveT3kRefreshToken(gT3kClient.refreshToken());
            setText(gT3kState,L"Connected. Session will be restored automatically next time.");
            setText(gStatus,L"Tone3000 connected.");
        }
        else if(m){ auto e=wideFromUtf8(m->error); setText(gT3kState,L"Connection failed: "+e); MessageBoxW(hwnd,e.c_str(),L"Tone3000",MB_OK|MB_ICONWARNING); }
        setT3kBusy(false); return 0;
    }
    case WM_APP_T3K_AUTOLOGIN_DONE: {
        std::unique_ptr<T3kResultMessage> m(reinterpret_cast<T3kResultMessage*>(lParam));
        if(m && m->ok){
            saveT3kRefreshToken(gT3kClient.refreshToken());
            setText(gT3kState,L"Connected automatically. Search for a NAM capture.");
            setText(gStatus,L"Tone3000 session restored.");
        } else {
            clearSavedT3kRefreshToken();
            gT3kClient.disconnect();
            setText(gT3kState,L"Saved session expired. Press Connect to authorize again.");
            setText(gStatus,L"Tone3000 needs authorization again.");
        }
        setT3kBusy(false); return 0;
    }
    case WM_APP_T3K_SEARCH_DONE: {
        std::unique_ptr<T3kSearchMessage> m(reinterpret_cast<T3kSearchMessage*>(lParam)); gT3kTones.clear(); gT3kModelItems.clear(); SendMessageW(gT3kResults,LB_RESETCONTENT,0,0); SendMessageW(gT3kModels,CB_RESETCONTENT,0,0);
        if(m && m->ok){
            saveT3kRefreshToken(gT3kClient.refreshToken());
            gT3kPage=m->page; gT3kTotalPages=m->totalPages; gT3kTotalResults=m->totalResults;
            gT3kTones=std::move(m->tones);
            for(const auto&t:gT3kTones){ std::wstring label=wideFromUtf8(t.title+" — "+t.creator+" ["+t.gear+"]"); SendMessageW(gT3kResults,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str())); }
            setText(gT3kState,L"Showing "+std::to_wstring(gT3kTones.size())+L" of "+std::to_wstring(gT3kTotalResults)+L" NAM A2 tones. Select one to load its models.");
        }
        else if(m) setText(gT3kState,L"Search failed: "+wideFromUtf8(m->error)); setT3kBusy(false); return 0;
    }
    case WM_APP_T3K_MODELS_DONE: {
        std::unique_ptr<T3kModelsMessage> m(reinterpret_cast<T3kModelsMessage*>(lParam)); gT3kModelItems.clear(); SendMessageW(gT3kModels,CB_RESETCONTENT,0,0);
        if(m && m->ok){ saveT3kRefreshToken(gT3kClient.refreshToken()); gT3kModelItems=std::move(m->models); for(const auto&x:gT3kModelItems){ auto label=wideFromUtf8(x.name+" | "+x.size+" | NAM v"+x.architectureVersion); SendMessageW(gT3kModels,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str())); } if(!gT3kModelItems.empty()) SendMessageW(gT3kModels,CB_SETCURSEL,0,0); setText(gT3kState,L"Select a model and press Load selected NAM."); }
        else if(m) setText(gT3kState,L"Could not load models: "+wideFromUtf8(m->error)); setT3kBusy(false); return 0;
    }
    case WM_APP_T3K_DOWNLOAD_DONE: {
        std::unique_ptr<T3kDownloadMessage> m(reinterpret_cast<T3kDownloadMessage*>(lParam));
        if(m && m->ok){ saveT3kRefreshToken(gT3kClient.refreshToken()); registerTemporaryT3kNam(m->path); setSingleNam(m->path); gT3kPreviewNam=m->path; EnableWindow(gT3kPreviewPlay,FALSE); stopT3kPreview(); setText(gStatus,L"Tone3000 NAM downloaded and loaded into the converter."); startT3kPreview(hwnd,true); }
        else if(m){ auto e=wideFromUtf8(m->error); setText(gT3kState,L"Download failed: "+e); MessageBoxW(hwnd,e.c_str(),L"Tone3000",MB_OK|MB_ICONWARNING); }
        setT3kBusy(false); return 0;
    }
    case WM_APP_T3K_PREVIEW_DONE: {
        std::unique_ptr<T3kPreviewMessage> m(reinterpret_cast<T3kPreviewMessage*>(lParam));
        gT3kPreviewBusy=false;
        EnableWindow(gT3kPreviewPlay,TRUE);
        if(m && m->ok && m->playing){
            EnableWindow(gT3kPreviewStop,TRUE);
            if (m->irLoaded) {
                std::wstring text = L"Real-time preview playing through NAM + cabinet IR";
                if (m->irOriginalRate > 0 && m->irOriginalRate != 48000)
                    text += L" (IR " + std::to_wstring(m->irOriginalRate) + L" -> 48000 Hz)";
                else
                    text += L" (IR 48000 Hz)";
                text += L".";
                setText(gT3kState, text);
            } else {
                setText(gT3kState,L"Real-time preview playing at "+std::to_wstring(m->sampleRate)+L" Hz through the loaded NAM.");
            }
        } else if(m){
            setText(gT3kState,L"NAM loaded, but real-time preview failed: "+wideFromUtf8(m->error));
        }
        return 0;
    }
    case WM_APP_UPLOAD_PROGRESS: {
        std::unique_ptr<UploadProgressMessage> m(reinterpret_cast<UploadProgressMessage*>(lParam));
        if (m) {
            SendMessageW(gUploaderProgress, PBM_SETRANGE32, 0, m->total > 0 ? m->total : 45);
            SendMessageW(gUploaderProgress, PBM_SETPOS, m->current, 0);
            setText(gStatus, m->status);
        }
        return 0;
    }
    case WM_APP_UPLOAD_DONE: {
        std::unique_ptr<ntc::gp200::UploadResult> r(reinterpret_cast<ntc::gp200::UploadResult*>(lParam));
        gUploadBusy = false;
        EnableWindow(gBackendTabs, TRUE);
        EnableWindow(gUploaderBrowseButton, TRUE);
        EnableWindow(gUploaderSlotCombo, TRUE);
        EnableWindow(gUploaderRescanButton, TRUE);
        refreshUploaderDetection();
        if (r) {
            setText(gStatus, r->message);
            if (r->ok) {
                SendMessageW(gUploaderProgress, PBM_SETPOS, 45, 0);
                MessageBoxW(hwnd, r->message.c_str(), L"GP-200 Uploader", MB_OK | MB_ICONINFORMATION);
            } else {
                MessageBoxW(hwnd, r->message.c_str(), L"GP-200 Uploader", MB_OK | MB_ICONERROR);
            }
        }
        return 0;
    }
    case WM_APP_GP5_UPLOAD_PROGRESS: {
        std::unique_ptr<UploadProgressMessage> m(reinterpret_cast<UploadProgressMessage*>(lParam));
        if (m) {
            SendMessageW(gGp5Progress, PBM_SETRANGE32, 0, m->total > 0 ? m->total : 146);
            SendMessageW(gGp5Progress, PBM_SETPOS, m->current, 0);
            setText(gStatus, m->status);
        }
        return 0;
    }
    case WM_APP_GP5_UPLOAD_DONE: {
        std::unique_ptr<ntc::gp5::UploadResult> r(reinterpret_cast<ntc::gp5::UploadResult*>(lParam));
        gGp5UploadBusy = false;
        EnableWindow(gBackendTabs, TRUE);
        EnableWindow(gGp5BrowseButton, TRUE);
        EnableWindow(gGp5SlotCombo, TRUE);
        EnableWindow(gGp5RescanButton, TRUE);
        refreshGp5Detection();
        if (r) {
            setText(gStatus, r->message);
            if (r->ok) {
                SendMessageW(gGp5Progress, PBM_SETPOS, 146, 0);
                MessageBoxW(hwnd, r->message.c_str(), L"GP-5/GP-50 Uploader", MB_OK | MB_ICONINFORMATION);
            } else {
                MessageBoxW(hwnd, r->message.c_str(), L"GP-5/GP-50 Uploader", MB_OK | MB_ICONERROR);
            }
        }
        return 0;
    }
    case WM_APP_DONE_SINGLE: {
        std::unique_ptr<ntc::ConversionResult> r(reinterpret_cast<ntc::ConversionResult*>(lParam));
        enableControls(true);
        updateBackendUi();
        updateTailControls();
        if (r && r->ok) {
            preserveConvertedT3kNam(r->inputNam);
            const std::wstring resultMessage = L"Conversion complete.\r\n\r\nGP-200 CLO 1024:\r\n" + r->gp2001024.wstring();
            setText(gStatus, L"Done. CLO file generated successfully.");
            const std::wstring doneTitle = L"NAM to CLO";
            MessageBoxW(hwnd, resultMessage.c_str(), doneTitle.c_str(), MB_ICONINFORMATION | MB_OK);
        } else {
            const std::wstring err = r ? ntc::fromUtf8(r->error) : L"Unknown conversion error.";
            setText(gStatus, L"Conversion failed.");
            MessageBoxW(hwnd, err.c_str(), L"Conversion failed", MB_ICONERROR | MB_OK);
        }
        return 0;
    }
    case WM_APP_DONE_BATCH: {
        std::unique_ptr<ntc::BatchConversionResult> r(reinterpret_cast<ntc::BatchConversionResult*>(lParam));
        enableControls(true);
        updateBackendUi();
        updateTailControls();
        if (!r || r->total == 0) {
            setText(gStatus, L"Batch conversion did not find any NAM files.");
            MessageBoxW(hwnd, L"No .nam files were found in the selected folder.", L"Batch conversion", MB_ICONINFORMATION | MB_OK);
            return 0;
        }

        for (const auto& item : r->items) {
            if (item.ok) preserveConvertedT3kNam(item.inputNam);
        }

        std::wstring resultMessage = L"Batch conversion complete.\r\n\r\nProcessed: " + std::to_wstring(r->total)
                                   + L"\r\nSucceeded: " + std::to_wstring(r->succeeded)
                                   + L"\r\nFailed: " + std::to_wstring(r->failed);
        if (r->failed > 0) {
            resultMessage += L"\r\n\r\nFailed files:";
            for (const auto& item : r->items) {
                if (!item.ok) {
                    resultMessage += L"\r\n- " + item.inputNam.filename().wstring();
                    if (!item.error.empty()) resultMessage += L": " + ntc::fromUtf8(item.error);
                }
            }
        }

        setText(gStatus, L"Batch done: " + std::to_wstring(r->succeeded) + L" succeeded, " + std::to_wstring(r->failed) + L" failed.");
        MessageBoxW(hwnd, resultMessage.c_str(), L"NAM to CLO - Batch", (r->failed == 0 ? MB_ICONINFORMATION : MB_ICONWARNING) | MB_OK);
        return 0;
    }
    case WM_CLOSE:
        if (gBusy) {
            if (MessageBoxW(hwnd, L"A conversion is running. Close anyway?", L"NAM to CLO", MB_ICONWARNING | MB_YESNO) != IDYES) return 0;
        }
        DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        gT3kPreviewPlayer.stop();
        if (!gBusy) cleanupUnconvertedT3kNams();
        destroyResources();
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // Prevent Windows DPI virtualization from inflating the whole window on 125%/150% displays.
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // Remove Tone3000 downloads that were never successfully converted.
    // Only downloads registered as temporary by this version are cleaned; older cache files are left untouched.
    cleanupUnconvertedT3kNams();
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TAB_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = wndProc;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    const std::wstring windowTitle = L"NAM to CLO";
    HWND hwnd = CreateWindowExW(0, kClassName, windowTitle.c_str(),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1040, 800,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd) { CoUninitialize(); return 1; }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
