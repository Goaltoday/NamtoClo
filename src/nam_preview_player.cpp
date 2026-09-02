#include "nam_preview_player.hpp"
#include "common.hpp"

#include <NAM/get_dsp.h>
#include <CDSPResampler.h>

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace ntc {
namespace fs = std::filesystem;
namespace {

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) |
           (static_cast<std::uint16_t>(p[1]) << 8);
}

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

bool readWavMono(const fs::path& path,
                 std::vector<float>& mono,
                 std::uint32_t& sampleRate,
                 std::string& error,
                 bool requireMono = true) {
    mono.clear();
    sampleRate = 0;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Cannot open preview WAV.";
        return false;
    }

    char riff[12]{};
    in.read(riff, 12);
    if (in.gcount() != 12 || std::memcmp(riff, "RIFF", 4) != 0 ||
        std::memcmp(riff + 8, "WAVE", 4) != 0) {
        error = "Selected preview file is not a RIFF/WAVE file.";
        return false;
    }

    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
    std::uint16_t blockAlign = 0;
    std::vector<std::uint8_t> data;

    while (in) {
        char id[4]{};
        char szb[4]{};
        in.read(id, 4);
        if (in.gcount() != 4) break;
        in.read(szb, 4);
        if (in.gcount() != 4) break;
        const std::uint32_t size = le32(reinterpret_cast<const std::uint8_t*>(szb));

        if (std::memcmp(id, "fmt ", 4) == 0) {
            std::vector<std::uint8_t> f(size);
            if (size) in.read(reinterpret_cast<char*>(f.data()), size);
            if (f.size() < 16) {
                error = "Invalid preview WAV fmt chunk.";
                return false;
            }
            format = le16(f.data());
            channels = le16(f.data() + 2);
            sampleRate = le32(f.data() + 4);
            blockAlign = le16(f.data() + 12);
            bits = le16(f.data() + 14);
            if (format == 0xfffe && f.size() >= 26) format = le16(f.data() + 24);
        } else if (std::memcmp(id, "data", 4) == 0) {
            data.resize(size);
            if (size) in.read(reinterpret_cast<char*>(data.data()), size);
        } else {
            in.seekg(size, std::ios::cur);
        }
        if (size & 1u) in.seekg(1, std::ios::cur);
    }

    if ((format != 1 && format != 3) || channels == 0 || sampleRate == 0 ||
        blockAlign == 0 || data.empty()) {
        error = "Preview WAV must be PCM or IEEE-float audio.";
        return false;
    }
    if (requireMono && channels != 1) {
        error = "Preview WAV must be mono (1 channel).";
        return false;
    }

    const std::size_t bytesPerSample = (bits + 7u) / 8u;
    if (bytesPerSample == 0 || blockAlign < bytesPerSample * channels) {
        error = "Unsupported preview WAV layout.";
        return false;
    }
    if (!((format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
          (format == 3 && (bits == 32 || bits == 64)))) {
        error = "Unsupported preview WAV bit depth.";
        return false;
    }

    const std::size_t frames = data.size() / blockAlign;
    if (frames == 0) {
        error = "Preview WAV contains no audio.";
        return false;
    }
    mono.resize(frames);

    auto sampleAt = [&](const std::uint8_t* p) -> float {
        if (format == 3 && bits == 32) {
            float v = 0.0f;
            std::memcpy(&v, p, 4);
            return std::isfinite(v) ? v : 0.0f;
        }
        if (format == 3 && bits == 64) {
            double v = 0.0;
            std::memcpy(&v, p, 8);
            return std::isfinite(v) ? static_cast<float>(v) : 0.0f;
        }
        if (format == 1 && bits == 8)
            return (static_cast<int>(*p) - 128) / 128.0f;
        if (format == 1 && bits == 16) {
            const auto v = static_cast<std::int16_t>(le16(p));
            return static_cast<float>(v / 32768.0f);
        }
        if (format == 1 && bits == 24) {
            std::int32_t v = static_cast<std::int32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
            if (v & 0x800000) v |= ~0xffffff;
            return static_cast<float>(v / 8388608.0f);
        }
        if (format == 1 && bits == 32) {
            const auto v = static_cast<std::int32_t>(le32(p));
            return static_cast<float>(static_cast<double>(v) / 2147483648.0);
        }
        return 0.0f;
    };

    for (std::size_t i = 0; i < frames; ++i) {
        const auto* frame = data.data() + i * blockAlign;
        double sum = 0.0;
        for (std::uint16_t c = 0; c < channels; ++c)
            sum += sampleAt(frame + c * bytesPerSample);
        mono[i] = static_cast<float>(sum / channels);
    }
    return true;
}

std::vector<float> resample(const std::vector<float>& in, double inRate, double outRate) {
    if (in.empty()) return {};
    const float srcF = static_cast<float>(inRate);
    const float dstF = static_cast<float>(outRate);
    if (srcF == dstF) return in;
    if (in.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return {};

    const int inCount = static_cast<int>(in.size());
    const int targetCount = std::max(0, static_cast<int>(static_cast<float>(inCount) * dstF / srcF));
    std::vector<float> out(static_cast<std::size_t>(targetCount), 0.0f);
    if (targetCount == 0) return out;

    std::vector<double> block(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) block[i] = static_cast<double>(in[i]);

    r8b::CDSPResampler24 rs(static_cast<double>(srcF), static_cast<double>(dstF), inCount, 2.0);
    std::size_t previous = 0;
    bool first = true;
    while (previous < out.size()) {
        if (!first) std::fill(block.begin(), block.end(), 0.0);
        first = false;
        double* produced = nullptr;
        const int count = rs.process(block.data(), inCount, produced);
        if (count < 0 || produced == nullptr) break;
        const std::size_t current = static_cast<std::size_t>(count);
        if (current > previous) {
            const std::size_t take = std::min<std::size_t>(current - previous, out.size() - previous);
            for (std::size_t i = 0; i < take; ++i)
                out[previous + i] = static_cast<float>(produced[i]);
        }
        previous = current;
    }
    rs.clear();
    return out;
}

std::optional<std::size_t> objectEnd(const std::string& s, std::size_t start) {
    if (start >= s.size() || s[start] != '{') return std::nullopt;
    int depth = 0;
    bool str = false;
    bool esc = false;
    for (std::size_t i = start; i < s.size(); ++i) {
        const char c = s[i];
        if (str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') str = false;
            continue;
        }
        if (c == '"') { str = true; continue; }
        if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return i + 1;
    }
    return std::nullopt;
}

bool prepareFullA2(const fs::path& namPath,
                   const fs::path& work,
                   fs::path& result,
                   std::string& error) {
    result = namPath;
    std::ifstream f(namPath, std::ios::binary);
    if (!f) return true;
    std::string s((std::istreambuf_iterator<char>(f)), {});
    if (s.find("\"SlimmableContainer\"") == std::string::npos) return true;

    const auto sk = s.find("\"submodels\"");
    if (sk == std::string::npos) return true;
    const auto ao = s.find('[', sk);
    if (ao == std::string::npos) return true;

    double best = -std::numeric_limits<double>::infinity();
    std::string bestModel;
    std::size_t p = ao + 1;
    while (p < s.size()) {
        while (p < s.size() && (std::isspace(static_cast<unsigned char>(s[p])) || s[p] == ',')) ++p;
        if (p >= s.size() || s[p] == ']') break;
        if (s[p] != '{') break;
        auto e = objectEnd(s, p);
        if (!e) break;

        const auto mk = s.find("\"max_value\"", p);
        const auto mod = s.find("\"model\"", p);
        if (mk < *e && mod < *e) {
            const auto col = s.find(':', mk);
            if (col == std::string::npos || col >= *e) { p = *e; continue; }
            char* ep = nullptr;
            const double v = std::strtod(s.c_str() + col + 1, &ep);
            const auto mc = s.find(':', mod);
            if (mc == std::string::npos || mc >= *e) { p = *e; continue; }
            const auto mo = s.find('{', mc);
            auto me = mo == std::string::npos ? std::nullopt : objectEnd(s, mo);
            if (me && *me <= *e && v > best) {
                best = v;
                bestModel = s.substr(mo, *me - mo);
            }
        }
        p = *e;
    }

    if (bestModel.empty()) {
        error = "Could not extract the Full submodel from A2 SlimmableContainer.";
        return false;
    }

    std::error_code ec;
    fs::create_directories(work, ec);
    if (ec) {
        error = "Cannot create realtime preview work directory.";
        return false;
    }
    result = work / L"realtime_a2_full.nam";
    std::ofstream o(result, std::ios::binary | std::ios::trunc);
    if (!o) {
        error = "Cannot create temporary A2 Full NAM.";
        return false;
    }
    o.write(bestModel.data(), static_cast<std::streamsize>(bestModel.size()));
    return o.good();
}


} // namespace

struct NamPreviewPlayer::Impl {
    mutable std::mutex mutex;
    std::unique_ptr<nam::DSP> dsp;
    std::vector<float> source;
    std::vector<float> ir;
    fs::path workDir;
    int rate = 0;
    int irOriginalRate = 0;
    std::thread thread;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> isPlaying{false};
    HANDLE playbackEvent = nullptr;

    ~Impl() {
        stop();
        std::error_code ec;
        if (!workDir.empty()) fs::remove_all(workDir, ec);
    }

    void stop() {
        stopRequested.store(true, std::memory_order_release);
        // Wake the playback worker immediately if it is waiting for waveOut.
        {
            std::scoped_lock lock(mutex);
            if (playbackEvent) SetEvent(playbackEvent);
        }
        if (thread.joinable()) thread.join();
        isPlaying.store(false, std::memory_order_release);
    }

    bool start(std::string& error) {
        stop();
        int localRate = 0;
        {
            std::scoped_lock lock(mutex);
            if (!dsp || source.empty() || rate <= 0) {
                error = "Realtime preview is not loaded.";
                return false;
            }
            localRate = rate;
        }
        WAVEFORMATEX probe{};
        probe.wFormatTag = WAVE_FORMAT_PCM;
        probe.nChannels = 2;
        probe.nSamplesPerSec = static_cast<DWORD>(localRate);
        probe.wBitsPerSample = 16;
        probe.nBlockAlign = static_cast<WORD>((probe.nChannels * probe.wBitsPerSample) / 8);
        probe.nAvgBytesPerSec = probe.nSamplesPerSec * probe.nBlockAlign;
        const MMRESULT query = waveOutOpen(nullptr, WAVE_MAPPER, &probe, 0, 0, WAVE_FORMAT_QUERY);
        if (query != MMSYSERR_NOERROR) {
            error = "The default Windows audio output cannot open the NAM sample rate (" + std::to_string(localRate) + " Hz).";
            return false;
        }
        stopRequested.store(false, std::memory_order_release);
        isPlaying.store(true, std::memory_order_release);
        thread = std::thread([this] { run(); });
        return true;
    }

    void run() {
        // This is a preview player, not a low-latency live input path.  A deeper
        // queue is intentional: it gives Windows/NAM enough scheduling margin
        // without changing the fact that the NAM is processed block-by-block.
        constexpr std::size_t kBufferCount = 8;
        constexpr int kFramesPerBuffer = 1024;

        nam::DSP* localDsp = nullptr;
        const std::vector<float>* src = nullptr;
        const std::vector<float>* localIr = nullptr;
        int sampleRate = 0;
        {
            // Only snapshot the objects. load() always calls stop() before it can
            // replace them, so they remain valid until this worker has finished.
            std::scoped_lock lock(mutex);
            localDsp = dsp.get();
            src = &source;
            localIr = &ir;
            sampleRate = rate;
        }
        if (!localDsp || !src || src->empty() || sampleRate <= 0) {
            isPlaying.store(false, std::memory_order_release);
            return;
        }

        try {
            localDsp->Reset(static_cast<double>(sampleRate), kFramesPerBuffer);
        } catch (...) {
            isPlaying.store(false, std::memory_order_release);
            return;
        }

        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = static_cast<DWORD>(sampleRate);
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = static_cast<WORD>((fmt.nChannels * fmt.wBitsPerSample) / 8);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) {
            isPlaying.store(false, std::memory_order_release);
            return;
        }
        {
            std::scoped_lock lock(mutex);
            playbackEvent = event;
        }

        HWAVEOUT wave = nullptr;
        const MMRESULT openResult = waveOutOpen(
            &wave, WAVE_MAPPER, &fmt,
            reinterpret_cast<DWORD_PTR>(event), 0, CALLBACK_EVENT);
        if (openResult != MMSYSERR_NOERROR) {
            {
                std::scoped_lock lock(mutex);
                playbackEvent = nullptr;
            }
            CloseHandle(event);
            isPlaying.store(false, std::memory_order_release);
            return;
        }

        std::array<std::vector<std::int16_t>, kBufferCount> pcm;
        std::array<WAVEHDR, kBufferCount> headers{};
        std::size_t prepared = 0;
        for (std::size_t i = 0; i < kBufferCount; ++i) {
            pcm[i].resize(static_cast<std::size_t>(kFramesPerBuffer) * 2u);
            headers[i].lpData = reinterpret_cast<LPSTR>(pcm[i].data());
            headers[i].dwBufferLength = static_cast<DWORD>(pcm[i].size() * sizeof(std::int16_t));
            if (waveOutPrepareHeader(wave, &headers[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
                break;
            ++prepared;
        }
        if (prepared != kBufferCount) {
            waveOutReset(wave);
            for (std::size_t i = 0; i < prepared; ++i)
                waveOutUnprepareHeader(wave, &headers[i], sizeof(WAVEHDR));
            waveOutClose(wave);
            {
                std::scoped_lock lock(mutex);
                playbackEvent = nullptr;
            }
            CloseHandle(event);
            isPlaying.store(false, std::memory_order_release);
            return;
        }

        std::array<NAM_SAMPLE, kFramesPerBuffer> input{};
        std::array<NAM_SAMPLE, kFramesPerBuffer> output{};
        NAM_SAMPLE* inputs[1] = { input.data() };
        NAM_SAMPLE* outputs[1] = { output.data() };

        // Optional cabinet IR. Both NAM processing and the IR path run at
        // 48 kHz, so the IR is resampled only once during load() when needed.
        std::vector<float> firDelay(localIr ? localIr->size() : 0u, 0.0f);
        std::size_t firWrite = 0;
        auto processIr = [&](float x) -> float {
            if (!localIr || localIr->empty()) return x;
            const std::size_t n = localIr->size();
            firDelay[firWrite] = x;
            double y = 0.0;
            // Split the circular FIR into two contiguous ranges to avoid a
            // modulo operation for every tap.
            std::size_t k = 0;
            for (; k <= firWrite; ++k)
                y += static_cast<double>((*localIr)[k]) * firDelay[firWrite - k];
            for (; k < n; ++k)
                y += static_cast<double>((*localIr)[k]) * firDelay[n + firWrite - k];
            if (++firWrite == n) firWrite = 0;
            return std::isfinite(y) ? static_cast<float>(y) : 0.0f;
        };

        std::size_t pos = 0;
        std::size_t queued = 0;
        std::array<bool, kBufferCount> active{};

        auto queueBuffer = [&](std::size_t i) -> bool {
            if (pos >= src->size() || stopRequested.load(std::memory_order_acquire)) return false;
            const int n = static_cast<int>(std::min<std::size_t>(kFramesPerBuffer, src->size() - pos));
            for (int s = 0; s < n; ++s)
                input[static_cast<std::size_t>(s)] = static_cast<NAM_SAMPLE>((*src)[pos + static_cast<std::size_t>(s)]);
            try {
                localDsp->process(inputs, outputs, n);
            } catch (...) {
                stopRequested.store(true, std::memory_order_release);
                return false;
            }
            for (int s = 0; s < n; ++s) {
                float v = static_cast<float>(output[static_cast<std::size_t>(s)]);
                if (!std::isfinite(v)) v = 0.0f;
                v = processIr(v);
                v = std::clamp(v, -1.0f, 1.0f);
                const auto sample = static_cast<std::int16_t>(std::lrint(v * 32767.0f));
                pcm[i][static_cast<std::size_t>(s) * 2u] = sample;
                pcm[i][static_cast<std::size_t>(s) * 2u + 1u] = sample;
            }
            headers[i].dwBufferLength = static_cast<DWORD>(static_cast<std::size_t>(n) * 2u * sizeof(std::int16_t));
            headers[i].dwFlags &= ~WHDR_DONE;
            if (waveOutWrite(wave, &headers[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                stopRequested.store(true, std::memory_order_release);
                return false;
            }
            pos += static_cast<std::size_t>(n);
            active[i] = true;
            ++queued;
            return true;
        };

        for (std::size_t i = 0; i < kBufferCount && pos < src->size(); ++i)
            queueBuffer(i);

        while (queued > 0 && !stopRequested.load(std::memory_order_acquire)) {
            // waveOut signals this event when a buffer completes. A finite timeout
            // also guarantees that Stop can never strand the UI indefinitely.
            WaitForSingleObject(event, 100);
            for (std::size_t i = 0; i < kBufferCount; ++i) {
                if (active[i] && (headers[i].dwFlags & WHDR_DONE) != 0) {
                    active[i] = false;
                    --queued;
                    if (pos < src->size() && !stopRequested.load(std::memory_order_acquire))
                        queueBuffer(i);
                }
            }
        }

        if (stopRequested.load(std::memory_order_acquire)) {
            waveOutReset(wave);
            queued = 0;
            active.fill(false);
        } else {
            // Let the final queued buffers complete before unpreparing them.
            while (queued > 0) {
                WaitForSingleObject(event, 100);
                for (std::size_t i = 0; i < kBufferCount; ++i) {
                    if (active[i] && (headers[i].dwFlags & WHDR_DONE) != 0) {
                        active[i] = false;
                        --queued;
                    }
                }
            }
        }

        for (auto& h : headers) waveOutUnprepareHeader(wave, &h, sizeof(WAVEHDR));
        waveOutClose(wave);
        {
            std::scoped_lock lock(mutex);
            playbackEvent = nullptr;
        }
        CloseHandle(event);
        isPlaying.store(false, std::memory_order_release);
    }
};

NamPreviewPlayer::NamPreviewPlayer() : impl_(std::make_unique<Impl>()) {}
NamPreviewPlayer::~NamPreviewPlayer() = default;

bool NamPreviewPlayer::load(const fs::path& namPath,
                            const fs::path& sourceWav,
                            const fs::path& irWav,
                            std::string& error) {
    error.clear();
    impl_->stop();

    std::error_code ec;
    if (!fs::exists(namPath, ec) || ec) {
        error = "Preview NAM does not exist.";
        return false;
    }
    if (!fs::exists(sourceWav, ec) || ec) {
        error = "Selected preview WAV does not exist.";
        return false;
    }
    if (!irWav.empty() && (!fs::exists(irWav, ec) || ec)) {
        error = "Selected cabinet IR WAV does not exist.";
        return false;
    }

    std::vector<float> source;
    std::uint32_t sourceRate = 0;
    if (!readWavMono(sourceWav, source, sourceRate, error)) return false;

    std::vector<float> ir48;
    std::uint32_t irRate = 0;
    if (!irWav.empty()) {
        std::vector<float> rawIr;
        if (!readWavMono(irWav, rawIr, irRate, error, false)) {
            if (error.rfind("Preview", 0) == 0) error.replace(0, 7, "IR");
            return false;
        }
        ir48 = (irRate == 48000u) ? std::move(rawIr)
                                  : resample(rawIr, static_cast<double>(irRate), 48000.0);
        if (ir48.empty()) {
            error = "Could not resample the cabinet IR to 48000 Hz.";
            return false;
        }
        // Remove only numerically empty tail samples. No normalization is done:
        // the IR's original gain remains intact.
        while (ir48.size() > 1 && std::abs(ir48.back()) < 1.0e-9f) ir48.pop_back();
    }

    const auto base = fs::temp_directory_path(ec);
    if (ec) {
        error = "Windows temporary directory is unavailable.";
        return false;
    }
    const auto work = base / L"NamToClo" / L"RealtimePreview";
    fs::remove_all(work, ec);
    ec.clear();
    fs::create_directories(work, ec);
    if (ec) {
        error = "Cannot create realtime preview work directory.";
        return false;
    }

    fs::path modelPath;
    if (!prepareFullA2(namPath, work, modelPath, error)) return false;

    try {
        auto dsp = nam::get_dsp(modelPath);
        if (!dsp) {
            error = "NeuralAmpModelerCore could not load the preview NAM.";
            return false;
        }
        // NAM preview runs on a fixed 48 kHz processing path.  Preview audio
        // and cabinet IRs are both converted once to 48 kHz before playback.
        constexpr double kPreviewRate = 48000.0;
        auto adapted = std::abs(kPreviewRate - static_cast<double>(sourceRate)) < 0.5
                         ? std::move(source)
                         : resample(source, static_cast<double>(sourceRate), kPreviewRate);
        if (adapted.empty()) {
            error = "Could not prepare preview audio at 48000 Hz.";
            return false;
        }

        std::scoped_lock lock(impl_->mutex);
        impl_->dsp = std::move(dsp);
        impl_->source = std::move(adapted);
        impl_->ir = std::move(ir48);
        impl_->rate = 48000;
        impl_->irOriginalRate = static_cast<int>(irRate);
        impl_->workDir = work;
        return true;
    } catch (const std::exception& e) {
        error = std::string("Realtime NAM preview: ") + e.what();
        return false;
    }
}

bool NamPreviewPlayer::play(std::string& error) { return impl_->start(error); }
void NamPreviewPlayer::stop() { impl_->stop(); }

bool NamPreviewPlayer::ready() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->dsp != nullptr && !impl_->source.empty() && impl_->rate > 0;
}

bool NamPreviewPlayer::playing() const { return impl_->isPlaying.load(std::memory_order_acquire); }

int NamPreviewPlayer::sampleRate() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->rate;
}

bool NamPreviewPlayer::irLoaded() const {
    std::scoped_lock lock(impl_->mutex);
    return !impl_->ir.empty();
}

int NamPreviewPlayer::irOriginalSampleRate() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->irOriginalRate;
}

} // namespace ntc
