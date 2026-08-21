#include "native_converter.hpp"
#include "common.hpp"
#include "gp200_midi.hpp"
#include "resource.h"

#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>

#include <filesystem>
#include <cwctype>
#include <memory>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace {
constexpr wchar_t kClassName[] = L"NamToCloMainWindow";
constexpr UINT WM_APP_STATUS = WM_APP + 1;
constexpr UINT WM_APP_DONE_SINGLE = WM_APP + 2;
constexpr UINT WM_APP_DONE_BATCH = WM_APP + 3;
constexpr UINT WM_APP_UPLOAD_PROGRESS = WM_APP + 4;
constexpr UINT WM_APP_UPLOAD_DONE = WM_APP + 5;
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
constexpr int IDC_BACKEND_TABS = 124;
constexpr int IDC_UPLOADER_CLO_PATH = 125;
constexpr int IDC_UPLOADER_BROWSE = 126;
constexpr int IDC_UPLOADER_SLOT = 127;
constexpr int IDC_UPLOADER_RESCAN = 128;
constexpr int IDC_UPLOADER_UPLOAD = 129;
constexpr int IDC_UPLOADER_DEVICE = 130;
constexpr int IDC_UPLOADER_PROGRESS = 131;

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
HWND gRefineCheck = nullptr;
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

bool uploaderTabSelected() {
    return gBackendTabs && TabCtrl_GetCurSel(gBackendTabs) == 1;
}

void showControl(HWND h, bool show) {
    if (h) ShowWindow(h, show ? SW_SHOW : SW_HIDE);
}

void showConversionUi(HWND hwnd, bool show) {
    const HWND controls[] = {
        gInputEdit, gOutEdit, gLoadFileButton, gLoadFolderButton, gBrowseButton,
        gConvertButton, gOpenButton, gTailCombo, gRecordedEdit, gBrowseRecordedButton,
        gCorrectiveCheck, gCorrectiveEdit, gBrowseCorrectiveButton, gRefineCheck,
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

void refreshUploaderDetection() {
    const auto d = ntc::gp200::detectGp200Midi();
    setText(gUploaderDevice, ntc::gp200::describeDetection(d));
    if (!gUploadBusy)
        EnableWindow(gUploaderUploadButton, d.inputFound && d.outputFound ? TRUE : FALSE);
}

void updateBackendUi() {
    HWND hwnd = gBackendTabs ? GetParent(gBackendTabs) : nullptr;
    const bool uploader = uploaderTabSelected();
    showConversionUi(hwnd, !uploader);
    showUploaderUi(hwnd, uploader);
    if (uploader) {
        setText(gSubtitle, L"Upload CLO files directly to a GP-200 SnapTone slot via USB MIDI.");
        refreshUploaderDetection();
        if (!gUploadBusy) setText(gStatus, L"GP-200 Uploader ready.");
    } else {
        setText(gSubtitle, L"Convert one NAM or batch-convert every NAM in a selected folder.");
        setText(gInfo,
            L"Place nam_input_wav.wav next to NamToClo.exe. The original stimulus is always used.\r\n"
            L"Tail / Reamp, Corrective IR and Tone Match are optional.");
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
    EnableWindow(gRefineCheck, enable);
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
    if (uploaderTabSelected()) return;
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

    EnableWindow(gRefineCheck, TRUE);
    const bool refineEnabled = SendMessageW(gRefineCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(gRefineTargetEdit, refineEnabled ? TRUE : FALSE);
    EnableWindow(gBrowseRefineTargetButton, refineEnabled ? TRUE : FALSE);
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
    refine.enabled = SendMessageW(gRefineCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    refine.passes = 4;
    refine.referenceWav = fs::path(getText(gRefineTargetEdit));

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
    gUi.uploaderCard = RECT{ margin, 145, clientW - margin, 535 };
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
    moveCtrl(gRefineCheck, contentX, gUi.sectionRefine.top + 32, 560, 24);
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
    SendMessageW(gBackendTabs, TCM_SETITEMSIZE, 0, MAKELPARAM(220, 25));
    TCITEMW tab{};
    tab.mask = TCIF_TEXT;
    tab.pszText = const_cast<LPWSTR>(L"Convert to CLO");
    TabCtrl_InsertItem(gBackendTabs, 0, &tab);
    tab.pszText = const_cast<LPWSTR>(L"GP-200 Uploader");
    TabCtrl_InsertItem(gBackendTabs, 1, &tab);
    TabCtrl_SetCurSel(gBackendTabs, 0);

    createSectionLabel(hwnd, 1002, L"Input NAM or folder");
    createSectionLabel(hwnd, 1003, L"Output folder");
    createSectionLabel(hwnd, 1005, L"Tail / Reamp source");
    createSectionLabel(hwnd, 1006, L"Recorded WAV (adapted automatically to 20.000 s)");
    createSectionLabel(hwnd, 1008, L"Corrective IR");
    createSectionLabel(hwnd, 1009, L"Tone Match");
    createSectionLabel(hwnd, 1010, L"Tone Match reference WAV (optional; first 20 s used)");

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

    gRefineCheck = CreateWindowW(L"BUTTON", L"Apply Tone Match (slow)",
                                 WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                 0, 0, 520, 24, hwnd, controlId(IDC_REFINE_CLO), nullptr, nullptr);
    applyFont(gRefineCheck);
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

    if (uploaderTabSelected()) {
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
    const bool primary = id == IDC_CONVERT || id == IDC_UPLOADER_UPLOAD;

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
        if (ctrl == gSubtitle || ctrl == GetDlgItem(hwnd, 1001)
            || ctrl == GetDlgItem(hwnd, 1002) || ctrl == GetDlgItem(hwnd, 1003) || ctrl == GetDlgItem(hwnd, 1004)
            || ctrl == GetDlgItem(hwnd, 1005) || ctrl == GetDlgItem(hwnd, 1006) || ctrl == GetDlgItem(hwnd, 1007)
            || ctrl == GetDlgItem(hwnd, 1008) || ctrl == GetDlgItem(hwnd, 1009) || ctrl == GetDlgItem(hwnd, 1010)
            || ctrl == GetDlgItem(hwnd, 1011) || ctrl == GetDlgItem(hwnd, 1012) || ctrl == GetDlgItem(hwnd, 1013)
            || ctrl == GetDlgItem(hwnd, 1014) || ctrl == gUploaderDevice) {
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
        case IDC_REFINE_CLO:
            if (HIWORD(wParam) == BN_CLICKED) updateTailControls();
            return 0;
        case IDC_TAIL_MODE:
            if (HIWORD(wParam) == CBN_SELCHANGE) updateTailControls();
            return 0;
        case IDC_CONVERT: startConversion(hwnd); return 0;
        case IDC_OPEN_OUTPUT: openOutputFolder(hwnd); return 0;
        case IDC_UPLOADER_BROWSE: chooseUploaderClo(hwnd); return 0;
        case IDC_UPLOADER_RESCAN:
            refreshUploaderDetection();
            setText(gStatus, getText(gUploaderDevice));
            return 0;
        case IDC_UPLOADER_UPLOAD: startUploader(hwnd); return 0;
        default: break;
        }
        break;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        wchar_t path[32768]{};
        if (DragQueryFileW(drop, 0, path, static_cast<UINT>(std::size(path)))) {
            fs::path p(path);
            std::error_code ec;
            if (uploaderTabSelected()) {
                std::wstring ext = p.extension().wstring();
                for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
                if (ext == L".clo") {
                    setText(gUploaderCloEdit, p.wstring());
                    setText(gStatus, L"CLO selected. Choose a destination slot and press Upload to GP-200.");
                } else {
                    MessageBoxW(hwnd, L"The GP-200 Uploader accepts .clo files.", L"GP-200 Uploader", MB_OK | MB_ICONINFORMATION);
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
    case WM_APP_DONE_SINGLE: {
        std::unique_ptr<ntc::ConversionResult> r(reinterpret_cast<ntc::ConversionResult*>(lParam));
        enableControls(true);
        updateBackendUi();
        updateTailControls();
        if (r && r->ok) {
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
