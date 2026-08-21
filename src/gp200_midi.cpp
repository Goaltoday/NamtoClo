#include "gp200_midi.hpp"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace ntc::gp200 {
namespace {

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

bool looksLikeGp200(const std::wstring& name) {
    const auto n = lower(name);
    return n.find(L"gp-200") != std::wstring::npos
        || n.find(L"gp200") != std::wstring::npos
        || n.find(L"valeton") != std::wstring::npos;
}

std::wstring mmError(MMRESULT code, bool input) {
    wchar_t text[256]{};
    const MMRESULT r = input
        ? midiInGetErrorTextW(code, text, static_cast<UINT>(std::size(text)))
        : midiOutGetErrorTextW(code, text, static_cast<UINT>(std::size(text)));
    if (r == MMSYSERR_NOERROR) return text;
    return L"MIDI error " + std::to_wstring(code);
}

class MidiSession {
public:
    ~MidiSession() { close(); }

    bool open(const MidiDetection& d, std::wstring& error) {
        close();
        closing_.store(false);
        expectedSlot_ = -1;
        ackReceived_ = false;
        identityReceived_ = false;

        MMRESULT r = midiInOpen(&midiIn_, d.inputId,
                                reinterpret_cast<DWORD_PTR>(&MidiSession::midiInCallback),
                                reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
        if (r != MMSYSERR_NOERROR) {
            error = L"Cannot open GP-200 MIDI input: " + mmError(r, true);
            midiIn_ = nullptr;
            return false;
        }

        for (auto& b : inputBuffers_) b.resize(2048);
        for (std::size_t i = 0; i < inputHeaders_.size(); ++i) {
            auto& h = inputHeaders_[i];
            h = {};
            h.lpData = reinterpret_cast<LPSTR>(inputBuffers_[i].data());
            h.dwBufferLength = static_cast<DWORD>(inputBuffers_[i].size());
            r = midiInPrepareHeader(midiIn_, &h, sizeof(h));
            if (r != MMSYSERR_NOERROR) {
                error = L"Cannot prepare GP-200 MIDI input buffer: " + mmError(r, true);
                close();
                return false;
            }
            preparedInputs_ = i + 1;
            r = midiInAddBuffer(midiIn_, &h, sizeof(h));
            if (r != MMSYSERR_NOERROR) {
                error = L"Cannot queue GP-200 MIDI input buffer: " + mmError(r, true);
                close();
                return false;
            }
        }

        r = midiInStart(midiIn_);
        if (r != MMSYSERR_NOERROR) {
            error = L"Cannot start GP-200 MIDI input: " + mmError(r, true);
            close();
            return false;
        }

        r = midiOutOpen(&midiOut_, d.outputId, 0, 0, CALLBACK_NULL);
        if (r != MMSYSERR_NOERROR) {
            error = L"Cannot open GP-200 MIDI output: " + mmError(r, false);
            midiOut_ = nullptr;
            close();
            return false;
        }
        return true;
    }

    bool sendSysEx(const std::vector<std::uint8_t>& bytes, std::wstring& error) {
        if (!midiOut_ || bytes.empty()) {
            error = L"GP-200 MIDI output is not open.";
            return false;
        }

        MIDIHDR hdr{};
        hdr.lpData = reinterpret_cast<LPSTR>(const_cast<std::uint8_t*>(bytes.data()));
        hdr.dwBufferLength = static_cast<DWORD>(bytes.size());

        MMRESULT r = midiOutPrepareHeader(midiOut_, &hdr, sizeof(hdr));
        if (r != MMSYSERR_NOERROR) {
            error = L"Cannot prepare MIDI SysEx: " + mmError(r, false);
            return false;
        }

        r = midiOutLongMsg(midiOut_, &hdr, sizeof(hdr));
        if (r != MMSYSERR_NOERROR) {
            midiOutUnprepareHeader(midiOut_, &hdr, sizeof(hdr));
            error = L"Cannot send MIDI SysEx: " + mmError(r, false);
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while ((hdr.dwFlags & MHDR_DONE) == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                error = L"Timed out while sending MIDI SysEx.";
                midiOutReset(midiOut_);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        midiOutUnprepareHeader(midiOut_, &hdr, sizeof(hdr));
        return error.empty();
    }

    void expectIdentity() {
        std::lock_guard<std::mutex> lock(ackMutex_);
        identityReceived_ = false;
    }

    bool waitForIdentity(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(ackMutex_);
        return ackCv_.wait_for(lock, timeout, [this] { return identityReceived_; });
    }

    void expectAck(int slot) {
        std::lock_guard<std::mutex> lock(ackMutex_);
        expectedSlot_ = slot;
        ackReceived_ = false;
    }

    bool waitForAck(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(ackMutex_);
        return ackCv_.wait_for(lock, timeout, [this] { return ackReceived_; });
    }

private:
    static void CALLBACK midiInCallback(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR) {
        if (msg != MIM_LONGDATA || instance == 0 || param1 == 0) return;
        auto* self = reinterpret_cast<MidiSession*>(instance);
        auto* hdr = reinterpret_cast<MIDIHDR*>(param1);
        self->handleLongData(hdr);
    }

    void handleLongData(MIDIHDR* hdr) {
        if (!hdr) return;
        if (hdr->dwBytesRecorded > 0) {
            const auto* data = reinterpret_cast<const std::uint8_t*>(hdr->lpData);
            const int size = static_cast<int>(hdr->dwBytesRecorded);

            const bool gpHeader = size >= 10 &&
                data[0] == 0xF0 && data[1] == 0x21 && data[2] == 0x25 && data[3] == 0x7E &&
                data[4] == 0x47 && data[5] == 0x50 && data[6] == 0x2D && data[7] == 0x32;
            if (gpHeader && data[8] == 0x12 && data[9] == 0x08) {
                {
                    std::lock_guard<std::mutex> lock(ackMutex_);
                    identityReceived_ = true;
                }
                ackCv_.notify_all();
            }

            int expected = -1;
            {
                std::lock_guard<std::mutex> lock(ackMutex_);
                expected = expectedSlot_;
            }

            if (expected >= 0 && size >= 38) {
                const bool matchesObservedAck =
                    data[8] == 0x12 && data[9] == 0x0c &&
                    data[13] == 0x01 && data[14] == 0x04 && data[15] == 0x01 &&
                    data[18] == 0x08 && data[26] == 0x01 &&
                    static_cast<int>(data[22]) == expected;
                if (matchesObservedAck) {
                    {
                        std::lock_guard<std::mutex> lock(ackMutex_);
                        ackReceived_ = true;
                    }
                    ackCv_.notify_all();
                }
            }
        }

        if (midiIn_ && !closing_.load()) {
            hdr->dwBytesRecorded = 0;
            midiInAddBuffer(midiIn_, hdr, sizeof(*hdr));
        }
    }

    void close() {
        closing_.store(true);
        if (midiOut_) {
            midiOutReset(midiOut_);
            midiOutClose(midiOut_);
            midiOut_ = nullptr;
        }
        if (midiIn_) {
            midiInStop(midiIn_);
            midiInReset(midiIn_);
            for (std::size_t i = 0; i < preparedInputs_; ++i)
                midiInUnprepareHeader(midiIn_, &inputHeaders_[i], sizeof(MIDIHDR));
            midiInClose(midiIn_);
            midiIn_ = nullptr;
            preparedInputs_ = 0;
        }
    }

    HMIDIIN midiIn_ = nullptr;
    HMIDIOUT midiOut_ = nullptr;
    std::array<std::vector<std::uint8_t>, 4> inputBuffers_;
    std::array<MIDIHDR, 4> inputHeaders_{};
    std::size_t preparedInputs_ = 0;
    std::mutex ackMutex_;
    std::condition_variable ackCv_;
    int expectedSlot_ = -1;
    bool ackReceived_ = false;
    bool identityReceived_ = false;
    std::atomic<bool> closing_{false};
};

} // namespace

MidiDetection detectGp200Midi() {
    MidiDetection d;
    for (UINT i = 0; i < midiInGetNumDevs(); ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR && looksLikeGp200(caps.szPname)) {
            d.inputFound = true;
            d.inputId = i;
            d.inputName = caps.szPname;
            break;
        }
    }
    for (UINT i = 0; i < midiOutGetNumDevs(); ++i) {
        MIDIOUTCAPSW caps{};
        if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR && looksLikeGp200(caps.szPname)) {
            d.outputFound = true;
            d.outputId = i;
            d.outputName = caps.szPname;
            break;
        }
    }
    return d;
}

std::wstring describeDetection(const MidiDetection& d) {
    if (d.inputFound && d.outputFound) {
        if (d.inputName == d.outputName) return L"Detected: " + d.outputName;
        return L"Detected IN: " + d.inputName + L" | OUT: " + d.outputName;
    }
    if (d.inputFound) return L"GP-200 MIDI input found, but MIDI output is missing.";
    if (d.outputFound) return L"GP-200 MIDI output found, but MIDI input is missing.";
    return L"GP-200 MIDI not detected. Connect the pedal and press Rescan.";
}

UploadResult uploadCloToGp200(const std::filesystem::path& cloFile,
                              int globalSlot,
                              UploadProgress progress) {
    CloUploadData data;
    std::wstring error;
    if (!buildCloUpload(cloFile, globalSlot, data, error))
        return { false, L"Upload failed: " + error };

    const auto detection = detectGp200Midi();
    if (!detection.inputFound || !detection.outputFound)
        return { false, L"Upload failed: " + describeDetection(detection) };

    MidiSession session;
    if (!session.open(detection, error))
        return { false, L"Upload failed: " + error };

    // Mirror the startup sequence already confirmed in the VST before any
    // editor transaction: Identity -> Enter Editor Mode -> 100 ms.
    // The standalone uploader does not need the VST's subsequent state dump.
    const std::vector<std::uint8_t> identity {
        0xF0,0x21,0x25,0x7E,0x47,0x50,0x2D,0x32,0x11,0x04,0x00,
        0x00,0x00,0x00,0x01,0x02,0x00,0x00,0x00,0x00,0x00,0xF7
    };
    const std::vector<std::uint8_t> enterEditor {
        0xF0,0x21,0x25,0x7E,0x47,0x50,0x2D,0x32,0x11,0x12,0x00,
        0x00,0x00,0xF7
    };
    if (progress) progress(0, static_cast<int>(data.chunks.size()), L"Identifying GP-200...");
    session.expectIdentity();
    if (!session.sendSysEx(identity, error))
        return { false, L"Upload failed: " + error };
    if (!session.waitForIdentity(std::chrono::milliseconds(1200)))
        return { false, L"Upload failed: GP-200 identity response timeout." };
    if (progress) progress(0, static_cast<int>(data.chunks.size()), L"Entering GP-200 editor mode...");
    if (!session.sendSysEx(enterEditor, error))
        return { false, L"Upload failed: " + error };
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (progress) progress(0, static_cast<int>(data.chunks.size()), L"Preparing destination slot...");
    session.expectAck(globalSlot);
    if (!session.sendSysEx(data.prepareMessage, error))
        return { false, L"Upload failed: " + error };

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const int total = static_cast<int>(data.chunks.size());
    for (int i = 0; i < total; ++i) {
        if (!session.sendSysEx(data.chunks[static_cast<std::size_t>(i)], error))
            return { false, L"Upload failed: " + error };
        if (progress) {
            std::wstringstream ss;
            ss << L"Uploading block " << (i + 1) << L" / " << total << L"...";
            progress(i + 1, total, ss.str());
        }
        if (i + 1 < total)
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    if (progress) progress(total, total, L"Waiting for GP-200 confirmation...");
    if (!session.waitForAck(std::chrono::milliseconds(2000)))
        return { false, L"Upload failed: GP-200 confirmation timeout." };

    return { true, L"Sound Clone upload completed successfully." };
}

} // namespace ntc::gp200
