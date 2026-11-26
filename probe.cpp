#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "apiDecoders.h"
#include "apiGUI.h"
#include "apiOptions.h"
#include "apiPlayer.h"
#include "apiPlugin.h"
#include "apiSkins.h"
#include "apiVisuals.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {
constexpr std::array<int, 5> fftMultipliers{1, 2, 4, 8, 16};
constexpr int minAdaptiveFftSize = 256;
constexpr int maxAdaptiveFftSize = 4096;
constexpr int maxFftSize = maxAdaptiveFftSize * fftMultipliers.back();
constexpr int decodeBlockFrames = 4096;
constexpr int baseStripHeight = 35;
constexpr int maxHeightMultiplier = 4;
constexpr double minimumDisplayDb = -68;
constexpr double maximumDisplayDb = -15;
constexpr int paletteLevels = 512;
constexpr double pi = 3.141592653589793;
constexpr GUID iidUnknown = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
IAIMPCore* core;
IAIMPServicePlayer* player;
IAIMPServiceAudioDecoders* decoders;
IAIMPServiceFileInfo* fileInfo;
IAIMPServiceConfig* config;
IAIMPServiceOptionsDialog* optionsDialog;
IAIMPUIMode* uiMode;
IAIMPUIColorSchema* uiColorSchema;
std::wstring skinSettingsPath;

enum class WindowFunction : UINT16 { BlackmanHarris, Hann, Triangular, Hamming, Blackman, Welch };
constexpr std::array windowNames{L"Blackman–Harris", L"Hann", L"Triangular", L"Hamming",
    L"Blackman", L"Welch (Gaussian)"};

struct SpectrumSettings {
    UINT16 multiplier;
    WindowFunction window;
    UINT16 height;
    UINT8 followTrack = 0;
    UINT8 logPercent = 60;

    bool sameAnalysis(const SpectrumSettings& other) const {
        return multiplier == other.multiplier && window == other.window && height == other.height &&
            logPercent == other.logPercent;
    }
};

constexpr SpectrumSettings defaultSettings{4, WindowFunction::BlackmanHarris, 3};
constexpr wchar_t multiplierKey[] = L"StaticSpectrogramProbe\\FftMultiplier";
constexpr wchar_t windowKey[] = L"StaticSpectrogramProbe\\WindowFunction";
constexpr wchar_t followKey[] = L"StaticSpectrogramProbe\\FollowPlayingTrack";
constexpr wchar_t heightKey[] = L"StaticSpectrogramProbe\\HeightMultiplier";
constexpr wchar_t logPercentKey[] = L"StaticSpectrogramProbe\\LogPercent";
std::atomic<SpectrumSettings> settings{defaultSettings};
std::atomic<bool> spectrumActive{false};
std::atomic<bool> switchingLayout{false};

template <typename T>
HRESULT query(T* instance, REFIID id, REFIID implemented, void** result) {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (!IsEqualGUID(id, iidUnknown) && !IsEqualGUID(id, implemented)) return E_NOINTERFACE;
    *result = instance;
    instance->AddRef();
    return S_OK;
}

struct ReleaseAimpObject {
    void operator()(IUnknown* object) const { object->Release(); }
};

template <typename T>
using AimpPtr = std::unique_ptr<T, ReleaseAimpObject>;

AimpPtr<IAIMPString> makeString(const wchar_t* value) {
    IAIMPString* object = nullptr;
    if (FAILED(core->CreateObject(IID_IAIMPString, reinterpret_cast<void**>(&object)))) return {};
    AimpPtr<IAIMPString> result(object);
    if (FAILED(result->SetData(const_cast<wchar_t*>(value), static_cast<INT32>(std::wcslen(value))))) return {};
    return result;
}

SpectrumSettings loadSettings() {
    if (!config) return defaultSettings;
    const auto readInt = [](const wchar_t* key, INT32 fallback) {
        auto path = makeString(key);
        INT32 value;
        return path && SUCCEEDED(config->GetValueAsInt32(path.get(), &value)) ? value : fallback;
    };
    SpectrumSettings result = defaultSettings;
    const int multiplier = readInt(multiplierKey, result.multiplier);
    if (std::find(fftMultipliers.begin(), fftMultipliers.end(), multiplier) != fftMultipliers.end())
        result.multiplier = multiplier;
    const int window = readInt(windowKey, static_cast<int>(result.window));
    if (window >= 0 && static_cast<size_t>(window) < windowNames.size())
        result.window = static_cast<WindowFunction>(window);
    const int height = readInt(heightKey, result.height);
    if (height >= 1 && height <= maxHeightMultiplier) result.height = height;
    result.followTrack = readInt(followKey, result.followTrack) == 1;
    const int percent = std::clamp(readInt(logPercentKey, result.logPercent), 0, 100);
    result.logPercent = 5 * ((percent + 2) / 5);
    return result;
}

struct SpectralWindow {
    int size = 0;
    WindowFunction function = WindowFunction::BlackmanHarris;
    std::array<unsigned, maxFftSize / 2> reversed{};
    std::array<double, maxFftSize> weights{};
    std::array<double, maxFftSize / 2> real{}, imaginary{}, cosines{}, sines{};

    void configure(int fftSize, WindowFunction window) {
        if (size == fftSize && function == window) return;
        const bool sizeChanged = size != fftSize;
        size = fftSize;
        function = window;
        const int half = fftSize / 2;
        for (int i = 0; i <= half; ++i) {
            const double a = pi * i / half;
            const double b = pi * i / (half - 1);
            switch (window) {
                case WindowFunction::Triangular: weights[i] = static_cast<double>(i) / half; break;
                case WindowFunction::Hann: weights[i] = .5 * (1 - std::cos(a)); break;
                case WindowFunction::Hamming: weights[i] = i ? .54 - .46 * std::cos(b) : 0; break;
                case WindowFunction::Blackman:
                    weights[i] = .42 - .50 * std::cos(a) + .08 * std::cos(2 * a);
                    break;
                case WindowFunction::Welch: {
                    const double offset = static_cast<double>(i - (half - 1)) / half;
                    weights[i] = i ? 1 - offset * offset : 0;
                    break;
                }
                case WindowFunction::BlackmanHarris:
                    weights[i] = i ? .35875 - .48829 * std::cos(b) +
                        .14128 * std::cos(2 * b) - .01168 * std::cos(3 * b) : 0;
                    break;
            }
            if (i > 0 && i < half) weights[fftSize - i] = weights[i];
        }
        double sum = 0;
        for (int i = 0; i < fftSize; ++i) sum += weights[i];
        const double gain = 2 / sum;
        for (int i = 0; i < fftSize; ++i) weights[i] *= gain;
        if (!sizeChanged) return;
        for (int i = 0; i < fftSize / 2; ++i) {
            reversed[i] = (reversed[i / 2] >> 1) | ((i & 1) * (fftSize / 4));
            cosines[i] = std::cos(-2 * pi * i / fftSize);
            sines[i] = std::sin(-2 * pi * i / fftSize);
        }
    }

    void computePower(const std::array<double, maxFftSize>& samples,
                    std::array<double, maxFftSize / 2 + 1>& peaks) {
        // Pack even and odd real samples into a single complex FFT of half the length.
        const int pairs = size / 2;
        for (int i = 0; i < pairs; ++i) {
            real[reversed[i]] = weights[2 * i] * samples[2 * i];
            imaginary[reversed[i]] = weights[2 * i + 1] * samples[2 * i + 1];
        }
        for (int span = 2; span <= pairs; span *= 2) {
            const int half = span / 2;
            const int stride = size / span;
            for (int block = 0; block < pairs; block += span) {
                for (int offset = 0; offset < half; ++offset) {
                    const int start = block + offset;
                    const int end = start + half;
                    const int twiddle = offset * stride;
                    const double re = cosines[twiddle] * real[end] - sines[twiddle] * imaginary[end];
                    const double im = sines[twiddle] * real[end] + cosines[twiddle] * imaginary[end];
                    real[end] = real[start] - re;
                    imaginary[end] = imaginary[start] - im;
                    real[start] += re;
                    imaginary[start] += im;
                }
            }
        }
        const auto storePower = [&](int bin, double re, double im) {
            peaks[bin] = re * re + im * im;
        };
        storePower(0, real[0] + imaginary[0], 0);
        storePower(pairs, real[0] - imaginary[0], 0);
        storePower(pairs / 2, real[pairs / 2], imaginary[pairs / 2]);
        // Recover the even/odd spectra from each conjugate pair before combining their powers.
        for (int bin = 1; bin < pairs / 2; ++bin) {
            const int mirror = pairs - bin;
            const double evenRe = .5 * (real[bin] + real[mirror]);
            const double evenIm = .5 * (imaginary[bin] - imaginary[mirror]);
            const double oddRe = .5 * (imaginary[bin] + imaginary[mirror]);
            const double oddIm = .5 * (real[mirror] - real[bin]);
            const double re = cosines[bin] * oddRe - sines[bin] * oddIm;
            const double im = sines[bin] * oddRe + cosines[bin] * oddIm;
            storePower(bin, evenRe + re, evenIm + im);
            storePower(mirror, evenRe - re, evenIm - im);
        }
    }
};

constexpr auto makePalette(DWORD background, DWORD peak) {
    std::array<DWORD, paletteLevels> palette{};
    for (int i = 0; i < paletteLevels; ++i) {
        const double strength = static_cast<double>(i) / (paletteLevels - 1);
        const auto channel = [=](int shift) {
            const int start = (background >> shift) & 0xff;
            const int end = (peak >> shift) & 0xff;
            return static_cast<DWORD>(start + strength * (end - start) + .5) << shift;
        };
        palette[i] = channel(16) | channel(8) | channel(0);
    }
    return palette;
}

struct SpectrumColors {
    DWORD background;
    std::array<DWORD, paletteLevels> unplayed;
    std::array<DWORD, paletteLevels> played;
    int cursorBrush;
};

constexpr SpectrumColors darkColors{0x282828, makePalette(0x282828, 0xd8d8d8),
    makePalette(0x282828, 0xde8c33), WHITE_BRUSH};
constexpr SpectrumColors lightColors{0xffffff, makePalette(0xffffff, 0x484848),
    makePalette(0xffffff, 0xb45b12), BLACK_BRUSH};

DWORD samplePlayerBackground() {
    HWND mainWindow = nullptr;
    EnumWindows([](HWND window, LPARAM output) -> BOOL {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        wchar_t className[32]{};
        if (process != GetCurrentProcessId() || !IsWindowVisible(window) || IsIconic(window) ||
            !GetClassNameW(window, className, static_cast<int>(std::size(className))) ||
            std::wcscmp(className, L"TAIMPMainForm") != 0) return TRUE;
        *reinterpret_cast<HWND*>(output) = window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&mainWindow));
    if (!mainWindow) return CLR_INVALID;
    RECT bounds{};
    if (!GetWindowRect(mainWindow, &bounds)) return CLR_INVALID;
    const int width = bounds.right - bounds.left;
    const int y = MulDiv(25, GetDpiForWindow(mainWindow), 96);
    if (width < 120 || y <= 0 || bounds.bottom - bounds.top <= y) return CLR_INVALID;

    const HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return CLR_INVALID;
    const HBITMAP bitmap = CreateBitmap(width, y + 1, 1, 32, nullptr);
    DWORD result = CLR_INVALID;
    if (bitmap) {
        const HGDIOBJ previous = SelectObject(dc, bitmap);
        if (previous && PrintWindow(mainWindow, dc, PW_RENDERFULLCONTENT)) {
            const COLORREF first = GetPixel(dc, width / 4, y);
            const COLORREF middle = GetPixel(dc, width / 2, y);
            const COLORREF last = GetPixel(dc, 3 * width / 4, y);
            const COLORREF sample = first == middle || first == last ? first :
                middle == last ? middle : CLR_INVALID;
            // A failed or blank layered-window capture can return black.
            if (sample != CLR_INVALID && sample != 0)
                result = RGB(GetBValue(sample), GetGValue(sample), GetRValue(sample));
        }
        if (previous) SelectObject(dc, previous);
        DeleteObject(bitmap);
    }
    DeleteDC(dc);
    return result;
}

bool cueRange(IAIMPString* uri, double duration, int rate, INT64 streamFrames,
              INT64& firstFrame, INT64& frameCount) {
    IAIMPVirtualFile* track = nullptr;
    if (FAILED(fileInfo->GetVirtualFile(uri, 0, &track)) || !track) return false;
    double start = 0, end = 0;
    const HRESULT hasStart = track->GetValueAsFloat(AIMP_VIRTUALFILE_PROPID_CLIPSTART, &start);
    const HRESULT hasEnd = track->GetValueAsFloat(AIMP_VIRTUALFILE_PROPID_CLIPFINISH, &end);
    track->Release();
    if (FAILED(hasStart) || !std::isfinite(start) || start < 0 ||
        start >= static_cast<double>(streamFrames) / rate) return false;
    if (FAILED(hasEnd) || end <= 0) end = start + duration;
    if (!std::isfinite(end) || end <= start || std::abs(end - start - duration) > 1) return false;
    const INT64 endFrame = std::llround(end * rate);
    firstFrame = std::llround(start * rate);
    frameCount = (endFrame < streamFrames ? endFrame : streamFrames) - firstFrame;
    return frameCount > 0 && std::abs(static_cast<double>(frameCount) / rate - duration) <= 1;
}

class Visualization final : public IAIMPExtensionEmbeddedVisualization {
    LONG references = 1;
    int width = 0;
    int height = 0;
    int plotHeight = 0;
    bool hasImage = false;
    AimpPtr<IAIMPPlaylistItem> activeTrack;
    AimpPtr<IAIMPAudioDecoder> decoder;
    INT64 frames = 0;
    INT32 channels = 0;
    INT32 format = 0;
    int frameBytes = 0;
    int columns = 0;
    int nextColumn = 0;
    int columnStep = 0;
    bool initialPass = false;
    INT64 clipStart = 0;
    INT64 windowStart = 0;
    INT64 unreadFrames = 0;
    int decodedFrames = 0;
    int decodedOffset = 0;
    std::array<short, decodeBlockFrames * 2> pcm{};
    std::array<float, decodeBlockFrames * 2> floating{};
    std::array<double, maxFftSize> samples{};
    std::array<double, maxFftSize / 2 + 1> peaks{};
    std::vector<int> bandEndBins;
    std::vector<DWORD> unplayedPixels;
    std::vector<DWORD> playedPixels;
    SpectralWindow spectralWindow;
    SpectrumColors colors = darkColors;
    std::vector<UINT16> pixelLevels;
    bool samplingTheme = false;
    std::chrono::steady_clock::time_point nextThemeRead{};
    static constexpr UINT unknownAppearance = static_cast<UINT>(-1);
    UINT skinAppearance = unknownAppearance;
    DWORD capturedBackground = CLR_INVALID;
    DWORD playedColor = 0xde8c33;
    SpectrumSettings analysisSettings = defaultSettings;

    void discardTrack() {
        decoder.reset();
        activeTrack.reset();
        hasImage = false;
    }

    bool beginAnalysis(IAIMPString* uri) {
        IAIMPAudioDecoder* rawDecoder = nullptr;
        const HRESULT result = decoders->CreateDecoderForFileURI(uri,
            AIMP_DECODER_FLAGS_FORCE_CREATE_INSTANCE, nullptr, &rawDecoder);
        AimpPtr<IAIMPAudioDecoder> source(rawDecoder);
        INT32 rate = 0;
        if (FAILED(result) || !source || !source->GetStreamInfo(&rate, &channels, &format)) return false;
        const bool isFloat = format == AIMP_DECODER_SAMPLEFORMAT_32BITFLOAT;
        if (!source->IsSeekable() || rate < 12000 || channels < 1 || channels > 2 ||
            (!isFloat && format != AIMP_DECODER_SAMPLEFORMAT_16BIT)) return false;
        frameBytes = channels * (isFloat ? 4 : 2);
        const INT64 streamFrames = source->GetSize() / frameBytes;
        INT64 firstFrame = 0;
        frames = streamFrames;
        if (frames <= 0) return false;
        double trackDuration = 0;
        if (SUCCEEDED(player->GetDuration(&trackDuration)) && trackDuration > 0 &&
            std::abs(static_cast<double>(streamFrames) / rate - trackDuration) > 1 &&
            !cueRange(uri, trackDuration, rate, streamFrames, firstFrame, frames)) return false;
        clipStart = firstFrame;
        decoder = std::move(source);
        columns = static_cast<int>(std::min<INT64>(width, frames));
        // Aim for a window a quarter as long as the time represented by one column.
        const double targetSize = static_cast<double>(frames) / columns / 4;
        int adaptiveSize = minAdaptiveFftSize;
        while (adaptiveSize < maxAdaptiveFftSize && targetSize >= 1.5 * adaptiveSize) adaptiveSize *= 2;
        const int fftSize = adaptiveSize * analysisSettings.multiplier;
        spectralWindow.configure(fftSize, analysisSettings.window);
        const int binCount = fftSize / 2;
        const double logWeight = analysisSettings.logPercent / 100.0;
        bandEndBins.resize(plotHeight);
        for (int band = 0; band < plotHeight - 1; ++band) {
            const double x = static_cast<double>(band + 1) / (plotHeight + 1);
            const double edge = (1 - logWeight) * binCount * x + logWeight * std::pow(binCount, x);
            bandEndBins[band] = static_cast<int>(std::ceil(edge));
        }
        bandEndBins.back() = binCount + 1;
        const DWORD backgroundPixel = colors.unplayed.front();
        unplayedPixels.assign(static_cast<size_t>(plotHeight) * columns, backgroundPixel);
        playedPixels.assign(unplayedPixels.size(), backgroundPixel);
        pixelLevels.assign(unplayedPixels.size(), 0);
        const double framesPerColumn = static_cast<double>(frames) / columns;
        initialPass = true;
        columnStep = 16 * framesPerColumn / rate < 14 ? 8 : 16;
        nextColumn = 0;
        windowStart = -2 * fftSize;
        unreadFrames = 0;
        decodedFrames = 0;
        decodedOffset = 0;
        return true;
    }

    bool moveWindow(INT64 start) {
        const int fftSize = spectralWindow.size;
        const INT64 delta = start - windowStart;
        if (delta == 0) return true;
        int firstSample = 0;
        if (delta > 0 && delta < fftSize) {
            firstSample = fftSize - static_cast<int>(delta);
            std::move(samples.begin() + delta, samples.begin() + fftSize, samples.begin());
        } else {
            const INT64 readStart = std::max<INT64>(0, start);
            const INT64 byteOffset = (clipStart + readStart) * frameBytes;
            if (decoder->GetPosition() != byteOffset && !decoder->SetPosition(byteOffset)) return false;
            unreadFrames = frames - readStart;
            decodedFrames = decodedOffset = 0;
        }
        windowStart = start;
        const bool isFloat = format == AIMP_DECODER_SAMPLEFORMAT_32BITFLOAT;
        const int begin = std::clamp<INT64>(-start, firstSample, fftSize);
        const int end = std::clamp<INT64>(frames - start, begin, fftSize);
        std::fill(samples.begin() + firstSample, samples.begin() + begin, 0);
        std::fill(samples.begin() + end, samples.begin() + fftSize, 0);
        for (int i = begin; i < end;) {
            if (decodedOffset == decodedFrames) {
                const int requested = static_cast<int>(std::min<INT64>(decodeBlockFrames, unreadFrames));
                void* buffer = isFloat ? static_cast<void*>(floating.data()) : pcm.data();
                decodedFrames = decoder->Read(buffer, requested * frameBytes) / frameBytes;
                if (decodedFrames <= 0 || decodedFrames > requested) return false;
                unreadFrames -= decodedFrames;
                decodedOffset = 0;
            }
            const int count = std::min(end - i, decodedFrames - decodedOffset);
            int index = decodedOffset * channels;
            for (int j = 0; j < count; ++j, index += channels)
                samples[i + j] = isFloat ? floating[index] : pcm[index] / 32768.0;
            decodedOffset += count;
            i += count;
        }
        return true;
    }

    bool analyzeWindow() {
        const int nyquistBin = spectralWindow.size / 2;
        const INT64 center = frames / columns * nextColumn + frames % columns * nextColumn / columns;
        if (!moveWindow(center - nyquistBin)) return false;
        spectralWindow.computePower(samples, peaks);
        for (int bin = 0; bin <= nyquistBin; ++bin) {
            peaks[bin] = 10 * std::log10(std::max(1e-14, peaks[bin]));
        }

        const int span = std::min(columnStep, columns - nextColumn);
        int firstBin = 0;
        for (int band = 0; band < plotHeight; ++band) {
            const int endBin = bandEndBins[band];
            const int count = endBin - firstBin;
            double meanDb = peaks[std::min(firstBin, nyquistBin)];
            if (count > 0) {
                double sum = 0;
                for (int bin = firstBin; bin < endBin; ++bin) sum += peaks[bin];
                meanDb = sum / count;
            }
            firstBin = endBin;
            const double level = (meanDb - minimumDisplayDb) / (maximumDisplayDb - minimumDisplayDb);
            const int colorIndex = static_cast<int>(std::clamp(level, 0.0, 1.0) * (paletteLevels - 1));
            const int y = plotHeight - 1 - band;
            const size_t pixel = static_cast<size_t>(y) * columns + nextColumn;
            std::fill_n(pixelLevels.begin() + pixel, span, colorIndex);
            std::fill_n(unplayedPixels.begin() + pixel, span, colors.unplayed[colorIndex]);
            std::fill_n(playedPixels.begin() + pixel, span, colors.played[colorIndex]);
        }

        // Each shorter pass fills the gaps between positions calculated on previous passes.
        nextColumn += initialPass ? columnStep : 2 * columnStep;
        while (nextColumn >= columns && columnStep > 0) {
            initialPass = false;
            columnStep /= 2;
            nextColumn = columnStep;
        }
        return true;
    }

    void analyzeNextBlock() {
        if (!decoder) return;
        using Clock = std::chrono::steady_clock;
        const auto deadline = Clock::now() + std::chrono::milliseconds(4);
        do {
            if (!analyzeWindow()) {
                decoder.reset();
                hasImage = false;
                return;
            }
        } while (columnStep > 0 && Clock::now() < deadline);
        if (columnStep == 0) decoder.reset();
    }

    void updateImage() {
        IAIMPPlaylistItem* item = nullptr;
        if (player->GetState() == AIMP_PLAYER_STATE_STOPPED ||
            FAILED(player->GetPlaylistItem(&item)) || !item) {
            discardTrack();
            return;
        }
        if (item == activeTrack.get()) {
            item->Release();
            return;
        }
        discardTrack();
        activeTrack.reset(item);
        IAIMPString* uri = nullptr;
        const HRESULT result = item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_FILENAME,
            IID_IAIMPString, reinterpret_cast<void**>(&uri));
        if (FAILED(result) || !uri) return;
        const wchar_t* path = uri->GetData();
        const bool localFile = path && uri->GetLength() >= 3 && path[1] == L':' && path[2] == L'\\';
        hasImage = localFile && beginAnalysis(uri);
        uri->Release();
    }

public:
    HRESULT WINAPI QueryInterface(REFIID id, void** result) override {
        return query<IAIMPExtensionEmbeddedVisualization>(this, id,
            IID_IAIMPExtensionEmbeddedVisualization, result);
    }
    ULONG WINAPI AddRef() override { return InterlockedIncrement(&references); }
    ULONG WINAPI Release() override { return InterlockedDecrement(&references); }
    DWORD WINAPI GetFlags() override { return AIMP_VISUAL_FLAGS_NOT_SUSPEND; }
    HRESULT WINAPI GetMaxDisplaySize(INT32*, INT32*) override { return E_FAIL; }
    HRESULT WINAPI GetName(IAIMPString** name) override {
        HRESULT result = core->CreateObject(IID_IAIMPString, reinterpret_cast<void**>(name));
        static wchar_t title[] = L"Static spectrogram probe";
        return SUCCEEDED(result) ? (*name)->SetData(title, static_cast<INT32>(std::wcslen(title))) : result;
    }
    HRESULT WINAPI Initialize(INT32 newWidth, INT32 newHeight) override {
        Resize(newWidth, newHeight);
        spectrumActive.store(true);
        return S_OK;
    }
    void WINAPI Finalize() override {
        spectrumActive.store(false);
        discardTrack();
    }
    void WINAPI Resize(INT32 newWidth, INT32 newHeight) override {
        if (newWidth != width) discardTrack();
        width = newWidth;
        height = newHeight;
    }
    void WINAPI Action(INT32 x, INT32, INT32 action) override {
        if (action != AIMP_VISUAL_ACTION_CLICK || !hasImage || width <= 1 || x < 0 || x >= width) return;
        double duration = 0;
        if (SUCCEEDED(player->GetDuration(&duration)) && duration > 0) player->SetPosition(duration * x / width);
    }
    void WINAPI Draw(HCANVAS canvas, PAIMPVisualData) override {
        if (width <= 0 || height <= 0 || switchingLayout.load()) return;
        if (samplingTheme) return;
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextThemeRead) {
            nextThemeRead = now + std::chrono::seconds(1);
            if (!skinSettingsPath.empty()) {
                skinAppearance = GetPrivateProfileIntW(L"ThemeAppearanceSwitcher", L"Position",
                    -1, skinSettingsPath.c_str());
            }
            samplingTheme = true;
            const DWORD sample = samplePlayerBackground();
            samplingTheme = false;
            if (sample != CLR_INVALID) capturedBackground = sample;

            INT32 style = AIMPUI_STYLE_DARK;
            if (uiMode) uiMode->GetValueAsInt32(AIMPUI_MODE_PROPID_STYLE, &style);
            const bool savedLight = skinAppearance != unknownAppearance ?
                (skinAppearance & 1) == 0 : style == AIMPUI_STYLE_LIGHT;
            const auto& fallback = savedLight ? lightColors : darkColors;
            const DWORD background = capturedBackground == CLR_INVALID ? fallback.background : capturedBackground;
            const int luma = 299 * ((background >> 16) & 255) +
                587 * ((background >> 8) & 255) + 114 * (background & 255);
            const bool light = luma >= 128000;
            DWORD accent = light ? 0xffb45b12 : 0xffde8c33;
            if (uiColorSchema) uiColorSchema->ApplyToARGB(&accent);
            accent &= 0xffffff;
            if (background != colors.background || accent != playedColor) {
                playedColor = accent;
                colors = {background, makePalette(background, light ? 0x484848 : 0xd8d8d8),
                    makePalette(background, accent), light ? BLACK_BRUSH : WHITE_BRUSH};
                for (size_t i = 0; i < pixelLevels.size(); ++i) {
                    unplayedPixels[i] = colors.unplayed[pixelLevels[i]];
                    playedPixels[i] = colors.played[pixelLevels[i]];
                }
            }
        }
        const auto selected = settings.load();
        const int newPlotHeight = std::clamp(MulDiv(baseStripHeight * selected.height,
            GetDeviceCaps(canvas, LOGPIXELSY), 96), 1, height);
        if (!selected.sameAnalysis(analysisSettings) || plotHeight != newPlotHeight) {
            analysisSettings = selected;
            plotHeight = newPlotHeight;
            discardTrack();
        }
        updateImage();
        analyzeNextBlock();
        RECT area = {0, 0, width, height};
        const COLORREF previousBrushColor = SetDCBrushColor(canvas,
            RGB((colors.background >> 16) & 255, (colors.background >> 8) & 255, colors.background & 255));
        FillRect(canvas, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SetDCBrushColor(canvas, previousBrushColor);
        if (!hasImage || initialPass) return;
        // The seek-bar display takes the center slice of AIMP's shared visualization frame.
        const int plotTop = (height - plotHeight) / 2;
        BITMAPINFO image = {};
        image.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        image.bmiHeader.biWidth = columns;
        image.bmiHeader.biHeight = -plotHeight;
        image.bmiHeader.biPlanes = 1;
        image.bmiHeader.biBitCount = 32;
        const auto drawPixels = [&](const auto& pixels) {
            // GDI's default mode combines discarded pixels with bitwise AND.
            const int previousMode = SetStretchBltMode(canvas, COLORONCOLOR);
            StretchDIBits(canvas, 0, plotTop, width, plotHeight, 0, 0, columns, plotHeight,
                pixels.data(), &image, DIB_RGB_COLORS, SRCCOPY);
            if (previousMode != 0) SetStretchBltMode(canvas, previousMode);
        };
        drawPixels(unplayedPixels);
        double duration = 0, position = 0;
        if (FAILED(player->GetDuration(&duration)) || duration <= 0 ||
            FAILED(player->GetPosition(&position))) return;
        const int x = static_cast<int>(position * (width - 1) / duration);
        if (x > 0) {
            const int state = SaveDC(canvas);
            if (state != 0) {
                IntersectClipRect(canvas, 0, plotTop, x, plotTop + plotHeight);
                drawPixels(playedPixels);
                RestoreDC(canvas, state);
            }
        }
        RECT line = {x, plotTop, x + 1, plotTop + plotHeight};
        FillRect(canvas, &line, static_cast<HBRUSH>(GetStockObject(colors.cursorBrush)));
    }
};

Visualization visualizer;

class WaveformProvider final : public IAIMPExtensionWaveformProvider {
    LONG references = 1;
public:
    HRESULT WINAPI QueryInterface(REFIID id, void** result) override {
        return query<IAIMPExtensionWaveformProvider>(this, id, IID_IAIMPExtensionWaveformProvider, result);
    }
    ULONG WINAPI AddRef() override { return InterlockedIncrement(&references); }
    ULONG WINAPI Release() override { return InterlockedDecrement(&references); }
    HRESULT WINAPI Calculate(IAIMPString*, IAIMPTaskOwner*, PAIMPWaveformPeakInfo peaks, INT32 count) override {
        if (!spectrumActive.load()) return E_NOTIMPL;
        if (!peaks || count <= 0) return E_INVALIDARG;
        // The skin still asks for its underlying seek bar. Satisfy it without opening the audio file.
        std::fill_n(peaks, count, TAIMPWaveformPeakInfo{});
        return S_OK;
    }
};
WaveformProvider waveformProvider;

bool applyStripLayout(int factor, bool follow) {
    IAIMPServiceSkinsManager* manager = nullptr;
    if (FAILED(core->QueryInterface(IID_IAIMPServiceSkinsManager, reinterpret_cast<void**>(&manager)))) return false;
    AimpPtr<IAIMPServiceSkinsManager> skins(manager);
    IAIMPPropertyList* properties = nullptr;
    if (FAILED(skins->QueryInterface(IID_IAIMPPropertyList, reinterpret_cast<void**>(&properties)))) return false;
    AimpPtr<IAIMPPropertyList> skinProperties(properties);
    IAIMPString* current = nullptr;
    if (FAILED(skinProperties->GetValueAsObject(AIMP_SERVICE_SKINSMAN_PROPID_SKIN, IID_IAIMPString,
        reinterpret_cast<void**>(&current)))) return false;
    AimpPtr<IAIMPString> currentSkin(current);
    const bool active = currentSkin && currentSkin->GetLength() > 0 && CompareStringOrdinal(
        currentSkin->GetData(), currentSkin->GetLength(), L"SpectrogramProbe.acs6", -1, TRUE) == CSTR_EQUAL;
    std::wstring filename(32768, L'\0');
    const auto length = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), filename.data(), filename.size());
    if (!length || length == filename.size()) return false;
    filename.resize(length);
    filename.resize(filename.find_last_of(L"\\/") + 1);
    filename += L"layouts\\" + std::to_wstring(factor) + (follow ? L"\\follow" : L"") + L"\\SpectrogramProbe.acs6";
    auto layout = makeString(filename.c_str());
    if (!layout || FAILED(skins->Install(layout.get(), 0))) return false;
    if (!active) return true;
    // Beta 8 keeps the loaded layout when the same filename is reapplied.
    auto builtin = makeString(L"");
    return builtin && SUCCEEDED(skins->Select(builtin.get())) && SUCCEEDED(skins->Select(currentSkin.get()));
}

class OptionsFrame final : public IAIMPOptionsDialogFrame, public IAIMPUIChangeEvents {
    LONG references = 1;
    IAIMPUIForm* form = nullptr;
    IAIMPUIComboBox* multiplier = nullptr;
    IAIMPUIComboBox* window = nullptr;
    IAIMPUIComboBox* stripHeight = nullptr;
    IAIMPUICheckBox* followTrack = nullptr;
    IAIMPUISlider* scale = nullptr;
    bool loading = false;

    bool createLabel(IAIMPServiceUI* ui, const wchar_t* caption, RECT bounds) {
        IAIMPUILabel* labelObject = nullptr;
        if (FAILED(ui->CreateControl(form, form, nullptr, nullptr, IID_IAIMPUILabel,
            reinterpret_cast<void**>(&labelObject)))) return false;
        AimpPtr<IAIMPUILabel> label(labelObject);
        auto text = makeString(caption);
        if (!text || FAILED(label->SetValueAsObject(AIMPUI_LABEL_PROPID_TEXT, text.get()))) return false;
        return SUCCEEDED(label->SetPlacement({ualNone, {}, {1, 1, 0, 0}, bounds}));
    }

    bool createScale(IAIMPServiceUI* ui) {
        if (!createLabel(ui, L"Шкала частот", {12, 136, 160, 160}) ||
            !createLabel(ui, L"Линейная", {168, 158, 246, 182}) ||
            !createLabel(ui, L"Логарифмическая", {271, 158, 403, 182}) ||
            FAILED(ui->CreateControl(form, form, nullptr, static_cast<IAIMPUIChangeEvents*>(this),
                IID_IAIMPUISlider, reinterpret_cast<void**>(&scale)))) return false;
        return SUCCEEDED(scale->SetValueAsInt32(AIMPUI_SLIDER_PROPID_AUTOSIZE, 0)) &&
            SUCCEEDED(scale->SetValueAsInt32(AIMPUI_SLIDER_PROPID_HORIZONTAL, 1)) &&
            SUCCEEDED(scale->SetValueAsInt32(AIMPUI_SLIDER_PROPID_VALUEMIN, 0)) &&
            SUCCEEDED(scale->SetValueAsInt32(AIMPUI_SLIDER_PROPID_VALUEMAX, 20)) &&
            SUCCEEDED(scale->SetValueAsInt32(AIMPUI_SLIDER_PROPID_PAGESIZE, 1)) &&
            SUCCEEDED(scale->SetPlacement({ualNone, {}, {1, 1, 0, 0}, {168, 132, 386, 158}}));
    }

    bool createChoice(IAIMPServiceUI* ui, const wchar_t* caption, int top, IAIMPUIComboBox** control) {
        if (!createLabel(ui, caption, {12, top + 4, 160, top + 28})) return false;
        if (FAILED(ui->CreateControl(form, form, nullptr, static_cast<IAIMPUIChangeEvents*>(this),
            IID_IAIMPUIComboBox, reinterpret_cast<void**>(control)))) return false;
        return SUCCEEDED((*control)->SetPlacement({ualNone, {}, {1, 1, 0, 0}, {168, top, 350, top + 26}})) &&
            SUCCEEDED((*control)->SetValueAsInt32(AIMPUI_COMBOBOX_PROPID_STYLE, AIMPUI_COMBOBOX_STYLE_LIST));
    }

    bool populateChoices() {
        const auto addFactor = [](IAIMPUIComboBox* box, int factor) {
            wchar_t label[10];
            std::swprintf(label, std::size(label), L"×%d", factor);
            auto text = makeString(label);
            return text && SUCCEEDED(box->Add(text.get(), 0));
        };
        for (int factor : fftMultipliers) {
            if (!addFactor(multiplier, factor)) return false;
        }
        for (int factor = 1; factor <= maxHeightMultiplier; ++factor) {
            if (!addFactor(stripHeight, factor)) return false;
        }
        for (const wchar_t* name : windowNames) {
            auto text = makeString(name);
            if (!text || FAILED(window->Add(text.get(), 0))) return false;
        }
        return true;
    }

    void select(SpectrumSettings values) {
        loading = true;
        const auto index = std::find(fftMultipliers.begin(), fftMultipliers.end(), values.multiplier) -
            fftMultipliers.begin();
        multiplier->SetValueAsInt32(AIMPUI_COMBOBOX_PROPID_ITEMINDEX, static_cast<INT32>(index));
        window->SetValueAsInt32(AIMPUI_COMBOBOX_PROPID_ITEMINDEX, static_cast<INT32>(values.window));
        stripHeight->SetValueAsInt32(AIMPUI_COMBOBOX_PROPID_ITEMINDEX, values.height - 1);
        followTrack->SetValueAsInt32(AIMPUI_CHECKBOX_PROPID_STATE, values.followTrack);
        scale->SetValueAsInt32(AIMPUI_SLIDER_PROPID_VALUE, values.logPercent / 5);
        loading = false;
    }

    void save() {
        INT32 factorIndex = -1, windowIndex = -1, heightIndex = -1, scaleIndex = -1;
        if (FAILED(multiplier->GetValueAsInt32(AIMPUI_COMBOBOX_PROPID_ITEMINDEX, &factorIndex)) ||
            FAILED(window->GetValueAsInt32(AIMPUI_COMBOBOX_PROPID_ITEMINDEX, &windowIndex)) ||
            FAILED(stripHeight->GetValueAsInt32(AIMPUI_COMBOBOX_PROPID_ITEMINDEX, &heightIndex)) ||
            FAILED(scale->GetValueAsInt32(AIMPUI_SLIDER_PROPID_VALUE, &scaleIndex)) ||
            scaleIndex < 0 || scaleIndex > 20 || heightIndex < 0 || heightIndex >= maxHeightMultiplier ||
            factorIndex < 0 || static_cast<size_t>(factorIndex) >= fftMultipliers.size() ||
            windowIndex < 0 || static_cast<size_t>(windowIndex) >= windowNames.size()) return;

        INT32 follow = 0;
        if (FAILED(followTrack->GetValueAsInt32(AIMPUI_CHECKBOX_PROPID_STATE, &follow))) return;
        const SpectrumSettings chosen{static_cast<UINT16>(fftMultipliers[factorIndex]), static_cast<WindowFunction>(windowIndex),
            static_cast<UINT16>(heightIndex + 1), static_cast<UINT8>(follow == AIMPUI_CHECKSTATE_CHECKED),
            static_cast<UINT8>(5 * scaleIndex)};
        const auto previous = settings.load();
        if (chosen.sameAnalysis(previous) && chosen.followTrack == previous.followTrack) return;
        auto multiplierPath = makeString(multiplierKey);
        auto windowPath = makeString(windowKey);
        auto heightPath = makeString(heightKey);
        const auto followPath = makeString(followKey);
        const auto scalePath = makeString(logPercentKey);
        if (!multiplierPath || !windowPath || !heightPath || !followPath || !scalePath) return;
        const auto persist = [&](SpectrumSettings values) {
            return SUCCEEDED(config->SetValueAsInt32(multiplierPath.get(), values.multiplier)) &&
                SUCCEEDED(config->SetValueAsInt32(windowPath.get(), static_cast<INT32>(values.window))) &&
                SUCCEEDED(config->SetValueAsInt32(heightPath.get(), values.height)) &&
                SUCCEEDED(config->SetValueAsInt32(followPath.get(), values.followTrack)) &&
                SUCCEEDED(config->SetValueAsInt32(scalePath.get(), values.logPercent)) && SUCCEEDED(config->FlushCache());
        };
        const bool resize = chosen.height != previous.height || chosen.followTrack != previous.followTrack;
        switchingLayout.store(resize);
        const bool saved = persist(chosen) && (!resize || applyStripLayout(chosen.height, chosen.followTrack));
        if (saved) settings.store(chosen);
        switchingLayout.store(false);
        if (!saved) {
            persist(previous);
            if (form) {
                select(previous);
                MessageBoxW(form->GetHandle(), L"Не удалось сохранить настройки спектрограммы или применить обложку.",
                    L"Спектрограмма", MB_OK | MB_ICONERROR);
            }
        }
    }

public:
    HRESULT WINAPI QueryInterface(REFIID id, void** result) override {
        if (IsEqualGUID(id, IID_IAIMPUIChangeEvents)) {
            return query<IAIMPUIChangeEvents>(this, id, IID_IAIMPUIChangeEvents, result);
        }
        return query<IAIMPOptionsDialogFrame>(this, id, IID_IAIMPOptionsDialogFrame, result);
    }
    ULONG WINAPI AddRef() override { return InterlockedIncrement(&references); }
    ULONG WINAPI Release() override { return InterlockedDecrement(&references); }

    HRESULT WINAPI GetName(IAIMPString** name) override {
        if (!name) return E_POINTER;
        *name = makeString(L"Спектрограмма").release();
        return *name ? S_OK : E_FAIL;
    }

    HWND WINAPI CreateFrame(HWND parent) override {
        IAIMPServiceUI* service = nullptr;
        if (FAILED(core->QueryInterface(IID_IAIMPServiceUI, reinterpret_cast<void**>(&service)))) return nullptr;
        AimpPtr<IAIMPServiceUI> ui(service);
        auto name = makeString(L"StaticSpectrogramOptions");
        if (!name || FAILED(ui->CreateForm(parent, AIMPUI_SERVICE_CREATEFORM_FLAGS_CHILD,
            name.get(), nullptr, &form))) return nullptr;
        loading = true;
        form->SetValueAsInt32(AIMPUI_FORM_PROPID_BORDERSTYLE, AIMPUI_FLAGS_BORDERSTYLE_NONE);
        if (!createChoice(ui.get(), L"Множитель FFT", 12, &multiplier) ||
            !createChoice(ui.get(), L"Оконная функция", 52, &window) ||
            !createChoice(ui.get(), L"Высота полосы", 92, &stripHeight) || !createScale(ui.get()) || !populateChoices()) {
            DestroyFrame();
            return nullptr;
        }
        auto caption = makeString(L"Выделять играющий трек в фонотеке");
        if (!caption || FAILED(ui->CreateControl(form, form, nullptr, static_cast<IAIMPUIChangeEvents*>(this),
            IID_IAIMPUICheckBox, reinterpret_cast<void**>(&followTrack))) ||
            FAILED(followTrack->SetValueAsObject(AIMPUI_CHECKBOX_PROPID_CAPTION, caption.get())) ||
            FAILED(followTrack->SetPlacement({ualNone, {}, {1, 1, 0, 0}, {12, 186, 380, 212}}))) {
            DestroyFrame();
            return nullptr;
        }
        select(settings.load());
        return form->GetHandle();
    }

    void WINAPI DestroyFrame() override {
        loading = true;
        if (multiplier) std::exchange(multiplier, nullptr)->Release();
        if (window) std::exchange(window, nullptr)->Release();
        if (stripHeight) std::exchange(stripHeight, nullptr)->Release();
        if (followTrack) std::exchange(followTrack, nullptr)->Release();
        if (scale) std::exchange(scale, nullptr)->Release();
        if (form) {
            AimpPtr<IAIMPUIForm> oldForm(std::exchange(form, nullptr));
            oldForm->Destroy(FALSE);
        }
        loading = false;
    }

    void WINAPI Notification(INT32 id) override {
        if (!multiplier || !window || !stripHeight || !followTrack || !scale) return;
        if (id == AIMP_SERVICE_OPTIONSDIALOG_NOTIFICATION_LOAD) select(settings.load());
        if (id == AIMP_SERVICE_OPTIONSDIALOG_NOTIFICATION_SAVE) save();
        if (id == AIMP_SERVICE_OPTIONSDIALOG_NOTIFICATION_RESET) {
            select(defaultSettings);
            OnChanged(nullptr);
        }
    }

    void WINAPI OnChanged(IUnknown*) override {
        if (!loading && multiplier && window && stripHeight && followTrack && scale && optionsDialog) optionsDialog->FrameModified(this);
    }
};

OptionsFrame optionsFrame;

class Plugin final : public IAIMPPlugin, public IAIMPExternalSettingsDialog {
    LONG references = 1;
    void releaseServices() {
        if (uiColorSchema) std::exchange(uiColorSchema, nullptr)->Release();
        if (uiMode) std::exchange(uiMode, nullptr)->Release();
        skinSettingsPath.clear();
        if (optionsDialog) std::exchange(optionsDialog, nullptr)->Release();
        if (config) std::exchange(config, nullptr)->Release();
        if (fileInfo) std::exchange(fileInfo, nullptr)->Release();
        if (decoders) std::exchange(decoders, nullptr)->Release();
        if (player) std::exchange(player, nullptr)->Release();
        core = nullptr;
    }
    bool optionsRegistered = false;
    bool waveformRegistered = false;

public:
    HRESULT WINAPI QueryInterface(REFIID id, void** result) override {
        if (IsEqualGUID(id, IID_IAIMPExternalSettingsDialog)) {
            return query<IAIMPExternalSettingsDialog>(this, id, IID_IAIMPExternalSettingsDialog, result);
        }
        return query<IAIMPPlugin>(this, id, iidUnknown, result);
    }
    ULONG WINAPI AddRef() override { return InterlockedIncrement(&references); }
    ULONG WINAPI Release() override { return InterlockedDecrement(&references); }
    PChar WINAPI InfoGet(INT32 field) override {
        static wchar_t name[] = L"Static spectrogram probe";
        static wchar_t author[] = L"Local test";
        static wchar_t empty[] = L"";
        if (field == AIMP_PLUGIN_INFO_NAME) return name;
        if (field == AIMP_PLUGIN_INFO_AUTHOR) return author;
        return empty;
    }
    DWORD WINAPI InfoGetCategories() override { return AIMP_PLUGIN_CATEGORY_VISUALS; }
    void WINAPI Show(HWND) override {
        if (optionsRegistered) optionsDialog->FrameShow(static_cast<IAIMPOptionsDialogFrame*>(&optionsFrame), TRUE);
    }
    HRESULT WINAPI Initialize(IAIMPCore* host) override {
        core = host;
        HRESULT result = host->QueryInterface(IID_IAIMPServicePlayer, reinterpret_cast<void**>(&player));
        if (SUCCEEDED(result))
            result = host->QueryInterface(IID_IAIMPServiceAudioDecoders, reinterpret_cast<void**>(&decoders));
        if (SUCCEEDED(result))
            result = host->QueryInterface(IID_IAIMPServiceFileInfo, reinterpret_cast<void**>(&fileInfo));
        if (FAILED(result)) {
            releaseServices();
            return result;
        }
        host->QueryInterface(IID_IAIMPServiceConfig, reinterpret_cast<void**>(&config));
        IAIMPServiceUI* rawUI = nullptr;
        if (SUCCEEDED(host->QueryInterface(IID_IAIMPServiceUI, reinterpret_cast<void**>(&rawUI))) && rawUI) {
            AimpPtr<IAIMPServiceUI> ui(rawUI);
            ui->QueryInterface(IID_IAIMPUIMode, reinterpret_cast<void**>(&uiMode));
            ui->QueryInterface(IID_IAIMPUIColorSchema, reinterpret_cast<void**>(&uiColorSchema));
        }
        IAIMPString* rawSkinPath = nullptr;
        if (SUCCEEDED(host->GetPath(AIMP_CORE_PATH_SKINS, &rawSkinPath)) && rawSkinPath) {
            AimpPtr<IAIMPString> skinPath(rawSkinPath);
            skinSettingsPath.assign(skinPath->GetData(), skinPath->GetLength());
            if (!skinSettingsPath.empty() && skinSettingsPath.back() != L'\\')
                skinSettingsPath += L'\\';
            skinSettingsPath += L"SpectrogramProbe.ini";
        }
        settings.store(loadSettings());
        result = host->RegisterExtension(IID_IAIMPServiceVisualizations, &visualizer);
        if (SUCCEEDED(result)) {
            waveformRegistered = SUCCEEDED(host->RegisterExtension(IID_IAIMPServiceWaveform, &waveformProvider));
            if (config && SUCCEEDED(host->QueryInterface(IID_IAIMPServiceOptionsDialog,
                reinterpret_cast<void**>(&optionsDialog)))) {
                optionsRegistered = SUCCEEDED(host->RegisterExtension(IID_IAIMPServiceOptionsDialog,
                    static_cast<IAIMPOptionsDialogFrame*>(&optionsFrame)));
            }
        }
        if (FAILED(result)) releaseServices();
        return result;
    }
    HRESULT WINAPI Finalize() override {
        if (waveformRegistered) core->UnregisterExtension(&waveformProvider);
        waveformRegistered = false;
        if (optionsRegistered) core->UnregisterExtension(static_cast<IAIMPOptionsDialogFrame*>(&optionsFrame));
        optionsFrame.DestroyFrame();
        optionsRegistered = false;
        if (core) core->UnregisterExtension(&visualizer);
        releaseServices();
        return S_OK;
    }
    void WINAPI SystemNotification(INT32, IUnknown*) override {}
};
Plugin plugin;
}

extern "C" EXPORT HRESULT WINAPI AIMPPluginGetHeader(IAIMPPlugin** header) {
    if (!header) return E_POINTER;
    *header = &plugin;
    plugin.AddRef();
    return S_OK;
}
