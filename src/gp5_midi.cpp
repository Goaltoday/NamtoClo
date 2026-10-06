#include "gp5_midi.hpp"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace ntc::gp5 {
namespace {

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return s;
}

bool looksLikeGp5(const std::wstring& name) {
    const auto n = lower(name);
    return n.find(L"gp-5") != std::wstring::npos
        || n.find(L"gp5") != std::wstring::npos
        || n.find(L"gp-50") != std::wstring::npos
        || n.find(L"gp50") != std::wstring::npos;
}

std::wstring mmError(MMRESULT code, bool input) {
    wchar_t text[256]{};
    const MMRESULT r = input
        ? midiInGetErrorTextW(code, text, static_cast<UINT>(std::size(text)))
        : midiOutGetErrorTextW(code, text, static_cast<UINT>(std::size(text)));
    if (r == MMSYSERR_NOERROR) return text;
    return L"MIDI error " + std::to_wstring(code);
}

bool nibbleDecodeSysEx(const std::uint8_t* data,
                       std::size_t size,
                       std::vector<std::uint8_t>& decoded) {
    decoded.clear();
    if (!data || size < 4 || data[0] != 0xF0 || data[size - 1] != 0xF7) return false;
    const std::size_t encodedSize = size - 2;
    if ((encodedSize & 1u) != 0u) return false;
    decoded.reserve(encodedSize / 2);
    for (std::size_t i = 1; i + 1 < size - 1; i += 2) {
        if (data[i] > 0x0Fu || data[i + 1] > 0x0Fu) return false;
        decoded.push_back(static_cast<std::uint8_t>((data[i] << 4) | data[i + 1]));
    }
    return true;
}

class MidiSession {
public:
    ~MidiSession() { close(); }

    bool open(const MidiDetection& d, std::wstring& error) {
        close();
        closing_.store(false);
        {
            std::lock_guard<std::mutex> lock(rxMutex_);
            ackReceived_ = false;
            completionReceived_ = false;
        }

        MMRESULT r = midiInOpen(&midiIn_, d.inputId,
                                reinterpret_cast<DWORD_PTR>(&MidiSession::midiInCallback),
                                reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
        if (r != MMSYSERR_NOERROR) {
            error = L"Cannot open GP-5/GP-50 MIDI input: " + mmError(r, true);
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
                error = L"Cannot prepare GP-5/GP-50 MIDI input buffer: " + mmError(r, true);
                close();
                return false;
            }
            preparedInputs_ = i + 1;
            r = midiInAddBuffer(midiIn_, &h, sizeof(h));
            if (r != MMSYSERR_NOERROR) {
                error = L"Cannot queue GP-5/GP-50 MIDI input buffer: " + mmError(r, true);
                close();
                return false;
            }
        }

        r = midiInStart(midiIn_);
        if (r != MMSYSERR_NOERROR) {
            error = L"Cannot start GP-5/GP-50 MIDI input: " + mmError(r, true);
            close();
            return false;
        }

        r = midiOutOpen(&midiOut_, d.outputId, 0, 0, CALLBACK_NULL);
        if (r != MMSYSERR_NOERROR) {
            error = L"Cannot open GP-5/GP-50 MIDI output: " + mmError(r, false);
            midiOut_ = nullptr;
            close();
            return false;
        }
        return true;
    }

    bool sendSysEx(const std::vector<std::uint8_t>& bytes, std::wstring& error) {
        if (!midiOut_ || bytes.empty()) {
            error = L"GP-5/GP-50 MIDI output is not open.";
            return false;
        }

        MIDIHDR hdr{};
        hdr.lpData = reinterpret_cast<LPSTR>(const_cast<std::uint8_t*>(bytes.data()));
        hdr.dwBufferLength = static_cast<DWORD>(bytes.size());

        MMRESULT r = midiOutPrepareHeader(midiOut_, &hdr, sizeof(hdr));
        if (r != MMSYSERR_NOERROR) {
            error = L"Cannot prepare GP-5/GP-50 MIDI SysEx: " + mmError(r, false);
            return false;
        }

        r = midiOutLongMsg(midiOut_, &hdr, sizeof(hdr));
        if (r != MMSYSERR_NOERROR) {
            midiOutUnprepareHeader(midiOut_, &hdr, sizeof(hdr));
            error = L"Cannot send GP-5/GP-50 MIDI SysEx: " + mmError(r, false);
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while ((hdr.dwFlags & MHDR_DONE) == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                midiOutReset(midiOut_);
                midiOutUnprepareHeader(midiOut_, &hdr, sizeof(hdr));
                error = L"Timed out while sending GP-5/GP-50 MIDI SysEx.";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        midiOutUnprepareHeader(midiOut_, &hdr, sizeof(hdr));
        return true;
    }

    void prepareForBlock() {
        std::lock_guard<std::mutex> lock(rxMutex_);
        ackReceived_ = false;
    }

    bool waitForAck(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(rxMutex_);
        return rxCv_.wait_for(lock, timeout, [this] { return ackReceived_; });
    }

    void prepareForCompletion() {
        std::lock_guard<std::mutex> lock(rxMutex_);
        completionReceived_ = false;
    }

    bool waitForCompletion(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(rxMutex_);
        return rxCv_.wait_for(lock, timeout, [this] { return completionReceived_; });
    }

private:
    static void CALLBACK midiInCallback(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR) {
        if (msg != MIM_LONGDATA || instance == 0 || param1 == 0) return;
        auto* self = reinterpret_cast<MidiSession*>(instance);
        self->handleLongData(reinterpret_cast<MIDIHDR*>(param1));
    }

    void handleLongData(MIDIHDR* hdr) {
        if (!hdr) return;
        if (hdr->dwBytesRecorded > 0) {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(hdr->lpData);
            std::vector<std::uint8_t> decoded;
            if (nibbleDecodeSysEx(bytes, static_cast<std::size_t>(hdr->dwBytesRecorded), decoded)) {
                static constexpr std::array<std::uint8_t, 7> ack { 0xB2,0x01,0x00,0x03,0x14,0x08,0x00 };
                static constexpr std::array<std::uint8_t, 10> completion { 0xCE,0x01,0x00,0x06,0x12,0x1B,0x03,0x00,0x00,0x00 };
                bool notify = false;
                {
                    std::lock_guard<std::mutex> lock(rxMutex_);
                    if (decoded.size() == ack.size() && std::equal(decoded.begin(), decoded.end(), ack.begin())) {
                        ackReceived_ = true;
                        notify = true;
                    }
                    if (decoded.size() == completion.size() && std::equal(decoded.begin(), decoded.end(), completion.begin())) {
                        completionReceived_ = true;
                        notify = true;
                    }
                }
                if (notify) rxCv_.notify_all();
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
    std::mutex rxMutex_;
    std::condition_variable rxCv_;
    bool ackReceived_ = false;
    bool completionReceived_ = false;
    std::atomic<bool> closing_{false};
};

} // namespace

MidiDetection detectGp5Midi() {
    MidiDetection d;
    for (UINT i = 0; i < midiInGetNumDevs(); ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR && looksLikeGp5(caps.szPname)) {
            d.inputFound = true;
            d.inputId = i;
            d.inputName = caps.szPname;
            break;
        }
    }
    for (UINT i = 0; i < midiOutGetNumDevs(); ++i) {
        MIDIOUTCAPSW caps{};
        if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR && looksLikeGp5(caps.szPname)) {
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
    if (d.inputFound) return L"GP-5/GP-50 MIDI input found, but MIDI output is missing.";
    if (d.outputFound) return L"GP-5/GP-50 MIDI output found, but MIDI input is missing.";
    return L"GP-5/GP-50 MIDI not detected. Connect the pedal and press Rescan.";
}

UploadResult uploadCloToGp5(const std::filesystem::path& cloFile,
                            int slot,
                            UploadProgress progress) {
    CloUploadData data;
    std::wstring error;
    if (!buildCloUpload(cloFile, slot, data, error))
        return { false, L"Upload failed: " + error };

    const auto detection = detectGp5Midi();
    if (!detection.inputFound || !detection.outputFound)
        return { false, L"Upload failed: " + describeDetection(detection) };

    MidiSession session;
    if (!session.open(detection, error))
        return { false, L"Upload failed: " + error };

    const int total = static_cast<int>(data.chunks.size());
    session.prepareForCompletion();

    for (int i = 0; i < total; ++i) {
        constexpr int maxAttempts = 3;
        bool acknowledged = false;
        for (int attempt = 1; attempt <= maxAttempts && !acknowledged; ++attempt) {
            session.prepareForBlock();
            if (!session.sendSysEx(data.chunks[static_cast<std::size_t>(i)], error))
                return { false, L"Upload failed: " + error };

            acknowledged = session.waitForAck(std::chrono::milliseconds(800));
            if (!acknowledged && progress) {
                std::wstringstream ss;
                ss << L"No ACK for block " << (i + 1) << L"; retry " << attempt << L" / " << maxAttempts << L"...";
                progress(i, total, ss.str());
            }
        }
        if (!acknowledged) {
            std::wstringstream ss;
            ss << L"Upload failed: GP-5/GP-50 ACK timeout at block " << (i + 1) << L" / " << total << L".";
            return { false, ss.str() };
        }

        if (progress) {
            std::wstringstream ss;
            ss << L"Uploading GP-5/GP-50 block " << (i + 1) << L" / " << total << L"...";
            progress(i + 1, total, ss.str());
        }
        // Captures advance after the ACK with only a very small gap.
        if (i + 1 < total) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (progress) progress(total, total, L"Waiting for GP-5/GP-50 final confirmation...");
    if (!session.waitForCompletion(std::chrono::milliseconds(2000)))
        return { false, L"All blocks were acknowledged, but the GP-5/GP-50 final confirmation timed out." };

    return { true, L"GP-5/GP-50 SnapTone upload completed successfully." };
}

} // namespace ntc::gp5
