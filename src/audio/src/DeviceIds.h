#pragma once

// Internal: miniaudio device ids as persistable strings, shared by the playback device (DeviceIo.cpp) and the capture
// device (AudioInput.cpp). ma_device_id is a backend-specific union; it is exposed as "<backend>:<native id>" so it can
// be stored in user preferences and compared as a plain string. Only included by the two .cpp files that see miniaudio.

#include <miniaudio.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

namespace wabcut::audio::detail {

inline void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

template <typename WChar, std::size_t N>
std::string utf16ToUtf8(const WChar (&text)[N]) {
    std::string out;
    for (std::size_t i = 0; i < N && text[i] != 0; ++i) {
        std::uint32_t cp = static_cast<std::uint16_t>(text[i]);
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < N) {
            const auto low = static_cast<std::uint16_t>(text[i + 1]);
            if (low >= 0xDC00 && low < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00U);
                ++i;
            }
        }
        appendUtf8(out, cp);
    }
    return out;
}

template <std::size_t N>
std::string boundedString(const char (&text)[N]) {
    return std::string(text, std::find(text, text + N, '\0'));
}

inline std::string encodeDeviceId(ma_backend backend, const ma_device_id& id) {
    std::string native;
    const char* prefix = "unknown";
    switch (backend) {
    case ma_backend_wasapi: prefix = "wasapi"; native = utf16ToUtf8(id.wasapi); break;
    case ma_backend_dsound: {
        prefix = "dsound";
        static constexpr char kHex[] = "0123456789abcdef";
        for (const ma_uint8 b : id.dsound) {
            native.push_back(kHex[b >> 4]);
            native.push_back(kHex[b & 0xF]);
        }
        break;
    }
    case ma_backend_winmm: prefix = "winmm"; native = std::to_string(id.winmm); break;
    case ma_backend_coreaudio: prefix = "coreaudio"; native = boundedString(id.coreaudio); break;
    case ma_backend_sndio: prefix = "sndio"; native = boundedString(id.sndio); break;
    case ma_backend_audio4: prefix = "audio4"; native = boundedString(id.audio4); break;
    case ma_backend_oss: prefix = "oss"; native = boundedString(id.oss); break;
    case ma_backend_pulseaudio: prefix = "pulse"; native = boundedString(id.pulse); break;
    case ma_backend_alsa: prefix = "alsa"; native = boundedString(id.alsa); break;
    case ma_backend_jack: prefix = "jack"; native = std::to_string(id.jack); break;
    case ma_backend_aaudio: prefix = "aaudio"; native = std::to_string(id.aaudio); break;
    case ma_backend_opensl: prefix = "opensl"; native = std::to_string(id.opensl); break;
    case ma_backend_webaudio: prefix = "webaudio"; native = boundedString(id.webaudio); break;
    case ma_backend_custom: prefix = "custom"; native = std::to_string(id.custom.i); break;
    case ma_backend_null: prefix = "null"; native = std::to_string(id.nullbackend); break;
    }
    if (native.empty()) {
        return {};
    }
    return std::string(prefix) + ":" + native;
}

/// The device of `type` (playback or capture) whose encoded id is `encodedId`.
inline std::optional<ma_device_id> findDevice(ma_context& context, const std::string& encodedId, ma_device_type type) {
    ma_device_info* playback = nullptr;
    ma_uint32 playbackCount = 0;
    ma_device_info* capture = nullptr;
    ma_uint32 captureCount = 0;
    if (ma_context_get_devices(&context, &playback, &playbackCount, &capture, &captureCount) != MA_SUCCESS) {
        return std::nullopt;
    }
    ma_device_info* list = type == ma_device_type_capture ? capture : playback;
    const ma_uint32 count = type == ma_device_type_capture ? captureCount : playbackCount;
    for (ma_uint32 i = 0; i < count; ++i) {
        if (encodeDeviceId(context.backend, list[i].id) == encodedId) {
            return list[i].id;
        }
    }
    return std::nullopt;
}

} // namespace wabcut::audio::detail
