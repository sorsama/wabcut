// Windows capture: Media Foundation video capture devices (capture cards, UVC webcams, whatever the system exposes as a video
// capture device - a DV camera or deck over FireWire shows up only if its driver does) read with IMFSourceReader.
//   * Devices: MFEnumDeviceSources(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID). Listing opens none.
//   * Formats: the device is activated for the moment it takes to read the reader's native media types (resolution, frame
//     rate, subtype); the camera light may blink. Asked when the user picks the device.
//   * Opening: the native type is selected, then an uncompressed output type is requested (the same layout when the device sends
//     NV12, YUY2, UYVY, I420, P010 or RGB; NV12 for MJPEG, H.264 and anything else, which the reader decodes or converts - MF's
//     decoders and colour converter run inside the reader with MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING).
//   * Delivery: a thread of ours calls ReadSample (synchronous mode) and copies each picture, top-down and packed, into a
//     CaptureFrame. stop() shuts the media source down, which ends a ReadSample that is waiting for a picture that will not come.
//   * Time: capture devices stamp samples with the system clock the steady clock reads (QueryPerformanceCounter, in 100 ns).
//     When a stamp is within two seconds of the steady clock it is used as it is; otherwise (a device that counts from zero)
//     stamps are taken relative to the first sample's arrival.
// Access: the system refuses desktop apps the camera when Settings > Privacy & security > Camera says so; activation then fails
// with E_ACCESSDENIED, which becomes accessDeniedError(Camera).
// Documentation: https://learn.microsoft.com/windows/win32/medfound/source-reader and
// https://learn.microsoft.com/windows/win32/medfound/enumerating-video-capture-devices

#ifdef _WIN32

#include "SystemBackends.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>

namespace wabcut::capture {

namespace {

using Microsoft::WRL::ComPtr;

std::int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// Keeps Media Foundation started and a multithreaded COM apartment alive for the rest of the process once capture is used: a
/// source and its reader outlive the thread that opened them (the bridge's worker), and MFShutdown or the end of the last
/// apartment would pull the platform from under them. One idle thread holds both.
class MfProcessAnchor {
public:
    static void ensure() {
        static MfProcessAnchor anchor;
        (void)anchor;
    }
    MfProcessAnchor(const MfProcessAnchor&) = delete;
    MfProcessAnchor& operator=(const MfProcessAnchor&) = delete;

private:
    MfProcessAnchor() {
        std::promise<void> ready;
        std::future<void> started = ready.get_future();
        m_thread = std::jthread([&ready](std::stop_token stop) {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_FULL);
            ready.set_value();
            std::mutex mutex;
            std::condition_variable_any wake;
            std::unique_lock lock(mutex);
            wake.wait(lock, stop, [] { return false; });
            if (SUCCEEDED(mf)) {
                MFShutdown();
            }
            if (SUCCEEDED(com)) {
                CoUninitialize();
            }
        });
        started.wait();
    }
    std::jthread m_thread;
};

/// COM and Media Foundation for the calling thread, for as long as it lives.
class MfRuntime {
public:
    MfRuntime() {
        MfProcessAnchor::ensure();
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_comOwned = SUCCEEDED(com); // RPC_E_CHANGED_MODE: the thread is an STA already; MF works there as well
        m_hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    }
    ~MfRuntime() {
        if (SUCCEEDED(m_hr)) {
            MFShutdown();
        }
        if (m_comOwned) {
            CoUninitialize();
        }
    }
    MfRuntime(const MfRuntime&) = delete;
    MfRuntime& operator=(const MfRuntime&) = delete;
    HRESULT status() const { return m_hr; }

private:
    HRESULT m_hr = E_FAIL;
    bool m_comOwned = false;
};

std::string toUtf8(const wchar_t* text, int length) {
    if (!text || length <= 0) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(bytes, 0)), '\0');
    if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), bytes, nullptr, nullptr);
    }
    return out;
}

/// The system's text for a device: no control characters, at most 128 bytes (it is shown, never interpreted).
std::string sanitize(std::string text) {
    std::erase_if(text, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; });
    if (text.size() > 128) {
        std::size_t cut = 128;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        text.resize(cut);
    }
    return text;
}

std::string stringAttribute(IMFAttributes* attributes, const GUID& key) {
    wchar_t* value = nullptr;
    UINT32 length = 0;
    if (FAILED(attributes->GetAllocatedString(key, &value, &length)) || !value) {
        return {};
    }
    std::string text = toUtf8(value, static_cast<int>(length));
    CoTaskMemFree(value);
    return text;
}

Error mfError(HRESULT hr, const char* what, bool camera = true) {
    const auto code = static_cast<std::uint32_t>(hr);
    if (hr == E_ACCESSDENIED) {
        return accessDeniedError(camera ? AccessKind::Camera : AccessKind::Microphone);
    }
    if (hr == MF_E_HW_MFT_FAILED_START_STREAMING || hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
        hr == HRESULT_FROM_WIN32(ERROR_BUSY)) {
        return Error{ErrorCode::DeviceError,
                     std::format("{}: the device is in use by another program. Close it (a video call, a recording app) and try again.", what)};
    }
    if (hr == MF_E_NOT_FOUND || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED)) {
        return Error{ErrorCode::NotFound, std::format("{}: the device is gone (unplugged?).", what)};
    }
    return Error{ErrorCode::DeviceError, std::format("{} (Windows error 0x{:08X})", what, code)};
}

class DeviceList {
public:
    DeviceList() = default;
    ~DeviceList() {
        for (UINT32 i = 0; i < m_count; ++i) {
            if (m_items[i]) {
                m_items[i]->Release();
            }
        }
        CoTaskMemFree(m_items);
    }
    DeviceList(const DeviceList&) = delete;
    DeviceList& operator=(const DeviceList&) = delete;

    HRESULT enumerate() {
        ComPtr<IMFAttributes> attributes;
        HRESULT hr = MFCreateAttributes(&attributes, 1);
        if (FAILED(hr)) {
            return hr;
        }
        hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (FAILED(hr)) {
            return hr;
        }
        return MFEnumDeviceSources(attributes.Get(), &m_items, &m_count);
    }
    UINT32 count() const { return m_count; }
    IMFActivate* at(UINT32 i) const { return m_items[i]; }
    IMFActivate* find(const std::string& link) const {
        for (UINT32 i = 0; i < m_count; ++i) {
            if (stringAttribute(m_items[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK) == link) {
                return m_items[i];
            }
        }
        return nullptr;
    }

private:
    IMFActivate** m_items = nullptr;
    UINT32 m_count = 0;
};

struct Subtype {
    const GUID* guid;
    const char* name;
    bool compressed;
    bool plain;      ///< the reader can hand the same layout over: no conversion requested
    PixelFormat pixel;
};

const Subtype* subtypeOf(const GUID& guid) {
    static const Subtype kTable[] = {
        {&MFVideoFormat_NV12, "NV12", false, true, PixelFormat::Nv12},
        {&MFVideoFormat_YUY2, "YUY2", false, true, PixelFormat::Yuy2},
        {&MFVideoFormat_UYVY, "UYVY", false, true, PixelFormat::Uyvy},
        {&MFVideoFormat_I420, "I420", false, true, PixelFormat::I420},
        {&MFVideoFormat_IYUV, "I420", false, true, PixelFormat::I420},
        {&MFVideoFormat_P010, "P010", false, true, PixelFormat::P010},
        {&MFVideoFormat_RGB24, "RGB24", false, true, PixelFormat::Bgr24},
        {&MFVideoFormat_RGB32, "RGB32", false, true, PixelFormat::Bgra},
        {&MFVideoFormat_ARGB32, "ARGB32", false, true, PixelFormat::Bgra},
        {&MFVideoFormat_MJPG, "MJPG", true, false, PixelFormat::Nv12},
        {&MFVideoFormat_H264, "H264", true, false, PixelFormat::Nv12},
        {&MFVideoFormat_HEVC, "HEVC", true, false, PixelFormat::Nv12},
        {&MFVideoFormat_YV12, "YV12", false, false, PixelFormat::Nv12},
    };
    for (const Subtype& s : kTable) {
        if (guid == *s.guid) {
            return &s;
        }
    }
    return nullptr;
}

/// A four-character code of a subtype GUID the table does not know: "NV21", "Y800".
std::string fourCc(const GUID& guid) {
    std::string text;
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((guid.Data1 >> (8 * i)) & 0xff);
        text.push_back(c >= 0x20 && c < 0x7f ? c : '?');
    }
    return text;
}

struct NativeType {
    VideoFormat format;
    GUID subtype{};
    DWORD index = 0;
};

bool describe(IMFMediaType* type, DWORD index, NativeType& out) {
    GUID major{};
    if (FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video) {
        return false;
    }
    GUID subtype{};
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) {
        return false;
    }
    UINT32 width = 0, height = 0;
    if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height)) || width < 2 || height < 2 || (width & 1) != 0 ||
        (height & 1) != 0) {
        return false;
    }
    UINT32 rateNum = 0, rateDen = 0;
    if (FAILED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &rateNum, &rateDen)) || rateNum == 0 || rateDen == 0) {
        return false;
    }
    UINT32 parNum = 1, parDen = 1;
    if (FAILED(MFGetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, &parNum, &parDen)) || parNum == 0 || parDen == 0) {
        parNum = parDen = 1;
    }
    UINT32 interlace = MFVideoInterlace_Progressive;
    type->GetUINT32(MF_MT_INTERLACE_MODE, &interlace);

    const Subtype* known = subtypeOf(subtype);
    VideoFormat f;
    f.width = static_cast<int>(width);
    f.height = static_cast<int>(height);
    f.frameRate = Rate{static_cast<std::int32_t>(std::min<UINT32>(rateNum, INT32_MAX)), static_cast<std::int32_t>(std::min<UINT32>(rateDen, INT32_MAX))}.normalized();
    f.pixelAspect = Rate{static_cast<std::int32_t>(std::min<UINT32>(parNum, 1000)), static_cast<std::int32_t>(std::min<UINT32>(parDen, 1000))};
    f.interlaced = interlace == MFVideoInterlace_FieldInterleavedUpperFirst || interlace == MFVideoInterlace_FieldInterleavedLowerFirst ||
                   interlace == MFVideoInterlace_FieldSingleUpper || interlace == MFVideoInterlace_FieldSingleLower;
    f.nativeName = known ? known->name : fourCc(subtype);
    f.compressed = known ? known->compressed : true;
    f.pixel = known ? known->pixel : PixelFormat::Nv12;
    out.format = f;
    out.subtype = subtype;
    out.index = index;
    return true;
}

bool sameRate(Rate a, Rate b) {
    return a.isValid() && b.isValid() &&
           std::abs(static_cast<double>(a.num) / a.den - static_cast<double>(b.num) / b.den) < 0.01;
}

/// Activates `activate` and wraps the media source in a source reader.
struct OpenedReader {
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSourceReader> reader;
};

std::expected<OpenedReader, Error> openReader(IMFActivate* activate) {
    OpenedReader opened;
    HRESULT hr = activate->ActivateObject(IID_PPV_ARGS(&opened.source));
    if (FAILED(hr)) {
        return std::unexpected(mfError(hr, "The device cannot be opened"));
    }
    ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes(&attributes, 2);
    if (SUCCEEDED(hr)) {
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }
    hr = MFCreateSourceReaderFromMediaSource(opened.source.Get(), attributes.Get(), &opened.reader);
    if (FAILED(hr)) {
        opened.source->Shutdown();
        activate->ShutdownObject();
        return std::unexpected(mfError(hr, "The device cannot be read"));
    }
    return opened;
}

std::vector<NativeType> nativeTypes(IMFSourceReader* reader) {
    std::vector<NativeType> types;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        const HRESULT hr = reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &type);
        if (hr == MF_E_NO_MORE_TYPES || (FAILED(hr) && hr != MF_E_INVALIDMEDIATYPE)) {
            break;
        }
        NativeType t;
        if (SUCCEEDED(hr) && describe(type.Get(), i, t)) {
            types.push_back(std::move(t));
        }
        if (i > 4096) {
            break;
        }
    }
    return types;
}

class MfVideoSource final : public VideoSource {
public:
    MfVideoSource(ComPtr<IMFActivate> activate, OpenedReader opened, std::string name, VideoFormat format, GUID outputSubtype)
        : m_activate(std::move(activate)), m_opened(std::move(opened)), m_name(std::move(name)), m_format(std::move(format)),
          m_outputSubtype(outputSubtype) {
        if (refreshType() && m_width >= 2 && m_height >= 2) {
            m_format.width = m_width; // the picture, not the padded buffer
            m_format.height = m_height;
        }
    }
    ~MfVideoSource() override {
        stop();
        release();
    }

    std::string description() const override { return m_name; }
    VideoFormat format() const override { return m_format; }

    std::expected<void, Error> start(FrameSink sink) override {
        if (m_thread.joinable()) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "The device is delivering already"});
        }
        m_sink = std::move(sink);
        m_stop = false;
        m_thread = std::thread([this] { run(); });
        return {};
    }

    void stop() noexcept override {
        m_stop = true;
        if (m_opened.source) {
            m_opened.source->Shutdown(); // ends a ReadSample that waits for a picture
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    std::string failure() const override {
        std::lock_guard lock(m_mutex);
        return m_failure;
    }

    std::uint64_t reportedDrops() const override { return m_ticks.load(); }

    /// Reads the current output type: the size the buffers are laid out for (a decoder pads it: 1920 x 1088 for a 1080-line
    /// picture), the part that is the picture (the type's minimum display aperture; without one, the format that was chosen), and
    /// the stride. False when it cannot be read.
    bool refreshType() {
        ComPtr<IMFMediaType> current;
        if (FAILED(m_opened.reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current))) {
            return false;
        }
        UINT32 width = 0, height = 0;
        if (FAILED(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &width, &height)) || width < 2 || height < 2) {
            return false;
        }
        m_frameWidth = static_cast<int>(width);
        m_frameHeight = static_cast<int>(height);
        int cropX = 0, cropY = 0;
        int visibleW = m_frameWidth, visibleH = m_frameHeight;
        MFVideoArea area{};
        UINT32 got = 0;
        if (SUCCEEDED(current->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof area, &got)) &&
            got == sizeof area && area.Area.cx > 0 && area.Area.cy > 0 && area.OffsetX.value >= 0 && area.OffsetY.value >= 0 &&
            area.OffsetX.value + area.Area.cx <= m_frameWidth && area.OffsetY.value + area.Area.cy <= m_frameHeight) {
            cropX = area.OffsetX.value;
            cropY = area.OffsetY.value;
            visibleW = static_cast<int>(area.Area.cx);
            visibleH = static_cast<int>(area.Area.cy);
        } else if (m_format.width >= 2 && m_format.height >= 2 && m_format.width <= m_frameWidth && m_format.height <= m_frameHeight) {
            visibleW = m_format.width; // no aperture: the picture is the top-left corner of the size that was asked for
            visibleH = m_format.height;
        }
        m_cropX = cropX & ~1;
        m_cropY = cropY & ~1;
        m_width = visibleW & ~1;
        m_height = visibleH & ~1;
        INT32 stride = 0;
        if (FAILED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&stride))) || stride == 0) {
            LONG computed = 0;
            GUID subtype{};
            current->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (SUCCEEDED(MFGetStrideForBitmapInfoHeader(subtype.Data1, width, &computed)) && computed != 0) {
                stride = computed;
            } else {
                stride = static_cast<INT32>(planeStride(m_format.pixel, m_frameWidth, 0));
            }
        }
        m_stride = stride;
        return true;
    }

private:
    void release() {
        m_opened.reader.Reset();
        m_opened.source.Reset();
        if (m_activate) {
            m_activate->ShutdownObject();
            m_activate.Reset();
        }
    }

    void fail(std::string message) {
        std::lock_guard lock(m_mutex);
        if (m_failure.empty()) {
            m_failure = std::move(message);
        }
    }

    void run() {
        MfRuntime runtime;
        if (!refreshType()) {
            fail("The device did not say what its pictures look like.");
            return;
        }
        while (!m_stop.load()) {
            DWORD streamIndex = 0;
            DWORD flags = 0;
            LONGLONG stamp = 0;
            ComPtr<IMFSample> sample;
            const HRESULT hr = m_opened.reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &streamIndex,
                                                           &flags, &stamp, &sample);
            if (m_stop.load()) {
                return;
            }
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR) != 0) {
                fail("The device stopped delivering pictures (it may have been unplugged).");
                return;
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                fail("The device stopped delivering pictures.");
                return;
            }
            if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
                refreshType(); // a capture card whose input signal changed
            }
            if ((flags & MF_SOURCE_READERF_STREAMTICK) != 0) {
                m_ticks.fetch_add(1);
            }
            if (sample) {
                deliver(sample.Get(), stamp);
            }
        }
    }

    std::int64_t hostTime(LONGLONG stamp, std::int64_t arrival) {
        const std::int64_t ns = static_cast<std::int64_t>(stamp) * 100;
        if (!m_mapKnown) {
            m_mapKnown = true;
            m_sameClock = std::llabs(arrival - ns) < 2'000'000'000LL;
            m_arrival0 = arrival;
            m_stamp0 = ns;
            m_lastStamp = ns - 1;
        }
        const bool increasing = ns > m_lastStamp;
        m_lastStamp = ns;
        if (!increasing) {
            return arrival; // a device that stamps nothing, or goes backwards: dated by when the picture arrived
        }
        std::int64_t host = m_sameClock ? ns : m_arrival0 + (ns - m_stamp0);
        if (m_sameClock && (host > arrival + 200'000'000LL || host < arrival - 5'000'000'000LL)) {
            host = arrival; // a stamp on the computer's clock that makes no sense
        }
        return host;
    }

    /// Rows of the Y plane as the buffer holds them: a decoder's buffer is padded (a 1080-line picture in 1088 rows), and the
    /// chroma follows the padded plane. A planar 4:2:0 layout is pitch x rows x 3/2 bytes long.
    static int allocatedRows(PixelFormat pixel, int height, DWORD length, std::ptrdiff_t absPitch) {
        const bool planar = pixel == PixelFormat::Nv12 || pixel == PixelFormat::P010 || pixel == PixelFormat::I420;
        if (!planar || absPitch <= 0 || length == 0) {
            return height;
        }
        const auto rows = static_cast<std::int64_t>(length) * 2 / 3 / absPitch;
        return rows >= height && rows <= height + 256 ? static_cast<int>(rows) : height;
    }

    static void copyRows(const std::uint8_t* first, std::ptrdiff_t pitch, int rows, std::size_t rowBytes, std::uint8_t* dst) {
        for (int y = 0; y < rows; ++y) {
            std::memcpy(dst + static_cast<std::size_t>(y) * rowBytes, first + static_cast<std::ptrdiff_t>(y) * pitch, rowBytes);
        }
    }

    void deliver(IMFSample* sample, LONGLONG stamp) {
        const std::int64_t arrival = steadyNowNs();
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
            return;
        }
        const int width = m_width;
        const int height = m_height;
        const int cropX = m_cropX;
        const int cropY = m_cropY;
        const PixelFormat pixel = m_format.pixel;
        const std::uint8_t* scan0 = nullptr;
        std::ptrdiff_t pitch = 0;
        ComPtr<IMF2DBuffer> flat;
        BYTE* linear = nullptr;
        bool locked2d = false;
        bool lockedLinear = false;
        if (SUCCEEDED(buffer.As(&flat))) {
            BYTE* top = nullptr;
            LONG p = 0;
            if (SUCCEEDED(flat->Lock2D(&top, &p))) {
                scan0 = top;
                pitch = p;
                locked2d = true;
            }
        }
        if (!locked2d) {
            DWORD maxLength = 0, currentLength = 0;
            if (FAILED(buffer->Lock(&linear, &maxLength, &currentLength)) || !linear) {
                return;
            }
            lockedLinear = true;
            pitch = m_stride != 0 ? m_stride : planeStride(pixel, m_frameWidth, 0);
            // A bottom-up picture (negative stride, RGB): its top row is the last one in memory.
            scan0 = pitch < 0 ? linear + static_cast<std::ptrdiff_t>(m_frameHeight - 1) * (-pitch) : linear;
            if (static_cast<std::size_t>(currentLength) < frameBytes(pixel, m_frameWidth, m_frameHeight) && pitch > 0) {
                buffer->Unlock();
                return; // a short buffer
            }
        }
        const std::ptrdiff_t absPitch = pitch < 0 ? -pitch : pitch;
        DWORD bufferLength = 0;
        buffer->GetCurrentLength(&bufferLength);
        const int rowsHeld = allocatedRows(pixel, m_frameHeight, bufferLength, absPitch);
        const auto x = static_cast<std::ptrdiff_t>(cropX);
        const auto y = static_cast<std::ptrdiff_t>(cropY);
        CaptureFrame frame = makeFrame(pixel, width, height, hostTime(stamp, arrival), [&](std::uint8_t* dst) {
            switch (pixel) {
            case PixelFormat::Nv12:
            case PixelFormat::P010: {
                const std::ptrdiff_t sample = pixel == PixelFormat::P010 ? 2 : 1; // bytes per luma sample (a CbCr pair is as wide)
                const auto rowBytes = static_cast<std::size_t>(planeStride(pixel, width, 0));
                copyRows(scan0 + y * pitch + x * sample, pitch, height, rowBytes, dst);
                copyRows(scan0 + static_cast<std::ptrdiff_t>(rowsHeld) * pitch + (y / 2) * pitch + x * sample, pitch, (height + 1) / 2, rowBytes,
                         dst + rowBytes * static_cast<std::size_t>(height));
                break;
            }
            case PixelFormat::I420: {
                const auto halfRow = static_cast<std::size_t>((width + 1) / 2);
                const std::ptrdiff_t chromaPitch = absPitch / 2;
                std::uint8_t* out = dst;
                copyRows(scan0 + y * pitch + x, pitch, height, static_cast<std::size_t>(width), out);
                out += static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
                const std::uint8_t* u = scan0 + static_cast<std::ptrdiff_t>(rowsHeld) * pitch;
                copyRows(u + (y / 2) * chromaPitch + x / 2, chromaPitch, (height + 1) / 2, halfRow, out);
                out += halfRow * static_cast<std::size_t>((height + 1) / 2);
                const std::uint8_t* v = u + static_cast<std::ptrdiff_t>((rowsHeld + 1) / 2) * chromaPitch;
                copyRows(v + (y / 2) * chromaPitch + x / 2, chromaPitch, (height + 1) / 2, halfRow, out);
                break;
            }
            default: {
                const std::ptrdiff_t bytesPerPixel = pixel == PixelFormat::Bgra ? 4 : pixel == PixelFormat::Bgr24 ? 3 : 2;
                copyRows(scan0 + y * pitch + x * bytesPerPixel, pitch, height, static_cast<std::size_t>(planeStride(pixel, width, 0)), dst);
                break;
            }
            }
        });
        if (locked2d) {
            flat->Unlock2D();
        } else if (lockedLinear) {
            buffer->Unlock();
        }
        m_sink(std::move(frame));
    }

    ComPtr<IMFActivate> m_activate;
    OpenedReader m_opened;
    std::string m_name;
    VideoFormat m_format;
    GUID m_outputSubtype{};
    FrameSink m_sink;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<std::uint64_t> m_ticks{0};
    mutable std::mutex m_mutex;
    std::string m_failure;
    int m_frameWidth = 0; ///< the size the buffers are laid out for
    int m_frameHeight = 0;
    int m_cropX = 0;      ///< where the picture starts in it
    int m_cropY = 0;
    int m_width = 0;      ///< the picture's size
    int m_height = 0;
    INT32 m_stride = 0;
    bool m_mapKnown = false;
    bool m_sameClock = false;
    std::int64_t m_arrival0 = 0;
    std::int64_t m_stamp0 = 0;
    std::int64_t m_lastStamp = 0;
};

class MediaFoundationBackend final : public CaptureBackend {
public:
    std::string name() const override { return "Media Foundation"; }
    bool supported() const override { return true; }

    std::expected<std::vector<VideoDevice>, Error> videoDevices() override {
        MfRuntime runtime;
        if (FAILED(runtime.status())) {
            return std::unexpected(mfError(runtime.status(), "Media Foundation cannot start"));
        }
        DeviceList list;
        if (const HRESULT hr = list.enumerate(); FAILED(hr)) {
            return std::unexpected(mfError(hr, "The video devices cannot be listed"));
        }
        std::vector<VideoDevice> devices;
        for (UINT32 i = 0; i < list.count(); ++i) {
            VideoDevice d;
            d.id = stringAttribute(list.at(i), MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
            d.name = sanitize(stringAttribute(list.at(i), MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME));
            if (d.name.empty()) {
                d.name = "Video device " + std::to_string(i + 1);
            }
            if (!d.id.empty()) {
                devices.push_back(std::move(d));
            }
        }
        return devices;
    }

    std::expected<std::vector<VideoFormat>, Error> videoFormats(const std::string& deviceId) override {
        MfRuntime runtime;
        if (FAILED(runtime.status())) {
            return std::unexpected(mfError(runtime.status(), "Media Foundation cannot start"));
        }
        DeviceList list;
        if (const HRESULT hr = list.enumerate(); FAILED(hr)) {
            return std::unexpected(mfError(hr, "The video devices cannot be listed"));
        }
        IMFActivate* activate = list.find(deviceId);
        if (!activate) {
            return std::unexpected(Error{ErrorCode::NotFound, "That video device is gone (unplugged?)."});
        }
        auto opened = openReader(activate);
        if (!opened) {
            return std::unexpected(opened.error());
        }
        std::vector<NativeType> types = nativeTypes(opened->reader.Get());
        opened->reader.Reset();
        opened->source->Shutdown();
        opened->source.Reset();
        activate->ShutdownObject();

        std::vector<VideoFormat> formats;
        for (NativeType& t : types) {
            if (std::ranges::find(formats, t.format) == formats.end()) {
                formats.push_back(std::move(t.format));
            }
        }
        std::ranges::stable_sort(formats, [](const VideoFormat& a, const VideoFormat& b) {
            const long long areaA = static_cast<long long>(a.width) * a.height;
            const long long areaB = static_cast<long long>(b.width) * b.height;
            if (areaA != areaB) {
                return areaA > areaB;
            }
            if (std::abs(a.fps() - b.fps()) > 0.01) {
                return a.fps() > b.fps();
            }
            return !a.compressed && b.compressed;
        });
        if (formats.empty()) {
            return std::unexpected(Error{ErrorCode::DeviceError, "The device lists no picture format this program can capture."});
        }
        return formats;
    }

    std::expected<std::vector<AudioDevice>, Error> audioDevices() override { return enumerateSystemAudioDevices(); }

    std::expected<std::unique_ptr<VideoSource>, Error> openVideo(const std::string& deviceId, const VideoFormat& wanted) override {
        MfRuntime runtime;
        if (FAILED(runtime.status())) {
            return std::unexpected(mfError(runtime.status(), "Media Foundation cannot start"));
        }
        DeviceList list;
        if (const HRESULT hr = list.enumerate(); FAILED(hr)) {
            return std::unexpected(mfError(hr, "The video devices cannot be listed"));
        }
        IMFActivate* found = list.find(deviceId);
        if (!found) {
            return std::unexpected(Error{ErrorCode::NotFound, "That video device is gone (unplugged?)."});
        }
        ComPtr<IMFActivate> activate(found); // the list's reference is released with the list; this one keeps the device
        const std::string name = sanitize(stringAttribute(found, MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME));
        auto opened = openReader(found);
        if (!opened) {
            return std::unexpected(opened.error());
        }
        auto fail = [&](Error error) -> std::expected<std::unique_ptr<VideoSource>, Error> {
            opened->reader.Reset();
            opened->source->Shutdown();
            opened->source.Reset();
            found->ShutdownObject();
            return std::unexpected(std::move(error));
        };

        // The device's native type that is the chosen format.
        const std::vector<NativeType> types = nativeTypes(opened->reader.Get());
        const NativeType* native = nullptr;
        for (const NativeType& t : types) {
            if (t.format.width == wanted.width && t.format.height == wanted.height && sameRate(t.format.frameRate, wanted.frameRate) &&
                t.format.nativeName == wanted.nativeName) {
                native = &t;
                break;
            }
        }
        if (!native) {
            return fail(Error{ErrorCode::InvalidArgument, "The device does not deliver that format any more."});
        }
        ComPtr<IMFMediaType> nativeType;
        HRESULT hr = opened->reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), native->index, &nativeType);
        if (SUCCEEDED(hr)) {
            hr = opened->reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, nativeType.Get());
        }
        if (FAILED(hr)) {
            return fail(mfError(hr, "The device refused that format"));
        }
        // What the reader hands over: the same layout when it is a plain one, else NV12 (decoded or converted by the reader).
        const Subtype* known = subtypeOf(native->subtype);
        const GUID outputSubtype = known && known->plain ? native->subtype : MFVideoFormat_NV12;
        const Subtype* outputKnown = subtypeOf(outputSubtype);
        ComPtr<IMFMediaType> output;
        hr = MFCreateMediaType(&output);
        if (SUCCEEDED(hr)) {
            output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            output->SetGUID(MF_MT_SUBTYPE, outputSubtype);
            hr = opened->reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, output.Get());
        }
        if (FAILED(hr) && outputSubtype != MFVideoFormat_NV12) {
            // The plain layout is refused (some drivers): let the reader convert to NV12.
            output.Reset();
            hr = MFCreateMediaType(&output);
            if (SUCCEEDED(hr)) {
                output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                hr = opened->reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, output.Get());
            }
        }
        if (FAILED(hr)) {
            return fail(mfError(hr, "The device's pictures cannot be converted"));
        }
        VideoFormat format = native->format;
        format.pixel = SUCCEEDED(hr) && outputKnown ? outputKnown->pixel : PixelFormat::Nv12;
        // The type the reader settled on decides the layout.
        ComPtr<IMFMediaType> current;
        if (SUCCEEDED(opened->reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current))) {
            GUID subtype{};
            if (SUCCEEDED(current->GetGUID(MF_MT_SUBTYPE, &subtype))) {
                if (const Subtype* s = subtypeOf(subtype); s && !s->compressed) {
                    format.pixel = s->pixel;
                }
            }
        }
        opened->reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        opened->reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
        return std::make_unique<MfVideoSource>(std::move(activate), std::move(*opened), name.empty() ? "Video device" : name, format,
                                               outputSubtype);
    }

    std::expected<std::unique_ptr<AudioSource>, Error> openAudio(const std::string& deviceId, int sampleRate, int channels) override {
        return openSystemAudioSource(deviceId, sampleRate, channels);
    }
};

} // namespace

std::shared_ptr<CaptureBackend> makeMediaFoundationBackend() {
    return std::make_shared<MediaFoundationBackend>();
}

std::expected<std::unique_ptr<VideoSource>, Error> openMediaFoundationFile(const std::filesystem::path& file) {
    MfRuntime runtime;
    if (FAILED(runtime.status())) {
        return std::unexpected(mfError(runtime.status(), "Media Foundation cannot start"));
    }
    ComPtr<IMFAttributes> attributes;
    HRESULT hr = MFCreateAttributes(&attributes, 2);
    if (SUCCEEDED(hr)) {
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }
    OpenedReader opened;
    hr = MFCreateSourceReaderFromURL(file.c_str(), attributes.Get(), &opened.reader);
    if (FAILED(hr)) {
        return std::unexpected(mfError(hr, "The file cannot be opened"));
    }
    ComPtr<IMFMediaType> output;
    hr = MFCreateMediaType(&output);
    if (SUCCEEDED(hr)) {
        output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        hr = opened.reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, output.Get());
    }
    if (FAILED(hr)) {
        return std::unexpected(mfError(hr, "The file's pictures cannot be converted"));
    }
    ComPtr<IMFMediaType> current;
    UINT32 width = 0, height = 0, rateNum = 0, rateDen = 1;
    hr = opened.reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current);
    if (SUCCEEDED(hr)) {
        hr = MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &width, &height);
    }
    if (FAILED(hr) || width < 2 || height < 2) {
        return std::unexpected(Error{ErrorCode::DeviceError, "The file has no picture"});
    }
    MFGetAttributeRatio(current.Get(), MF_MT_FRAME_RATE, &rateNum, &rateDen);
    VideoFormat format;
    format.width = static_cast<int>(width & ~1u); // the constructor puts the picture's own size in (the aperture)
    format.height = static_cast<int>(height & ~1u);
    format.frameRate = rateNum > 0 && rateDen > 0
                           ? Rate{static_cast<std::int32_t>(rateNum), static_cast<std::int32_t>(rateDen)}.normalized()
                           : Rate{30, 1};
    format.pixel = PixelFormat::Nv12;
    format.nativeName = "file";
    opened.reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    opened.reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    return std::make_unique<MfVideoSource>(ComPtr<IMFActivate>{}, std::move(opened), "Video file", format, MFVideoFormat_NV12);
}

} // namespace wabcut::capture

#endif // _WIN32
