#pragma once

// 32-bit float WAV files (WAVE_FORMAT_IEEE_FLOAT), written as a stream: Enhance Speech's takes and the separated
// stems (A.4). FFmpeg (media::AudioDecoder) reads them back sample-exact. The header's sizes are written on close();
// a file that would pass 4 GiB is refused (RIFF's limit).

#include <wabcut/model/Error.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

namespace wabcut::ai::audio {

class WavWriter {
public:
    /// Errors: InvalidArgument (channels outside 1..16, rate outside 8 kHz..768 kHz), IoError.
    static std::expected<WavWriter, Error> create(const std::filesystem::path& path, int channels, int rate);

    WavWriter(WavWriter&&) noexcept = default;
    WavWriter& operator=(WavWriter&&) noexcept = default;
    ~WavWriter();

    /// Appends interleaved frames. Errors: InvalidArgument (not whole frames), IoError, Unsupported (past 4 GiB).
    std::expected<void, Error> write(std::span<const float> interleaved);
    /// Writes the sizes and closes the file. Errors: IoError.
    std::expected<void, Error> close();

    std::int64_t frames() const { return m_frames; }
    int channels() const { return m_channels; }

private:
    WavWriter() = default;

    std::ofstream m_out;
    std::filesystem::path m_path;
    int m_channels = 0;
    int m_rate = 0;
    std::int64_t m_frames = 0;
    bool m_open = false;
};

struct WavInfo {
    int channels = 0;
    int rate = 0;
    std::int64_t frames = 0;
};

/// The format of a float WAV written by WavWriter. Errors: IoError, ParseError (not one).
std::expected<WavInfo, Error> readWavInfo(const std::filesystem::path& path);
/// Its samples (interleaved). Errors as readWavInfo().
std::expected<std::vector<float>, Error> readWavSamples(const std::filesystem::path& path, WavInfo* info = nullptr);

} // namespace wabcut::ai::audio
