#include <wabcut/ai/audio/WavFile.h>

#include <wabcut/model/MediaLocator.h>

#include <array>
#include <cstring>

namespace wabcut::ai::audio {

namespace {

constexpr std::uint32_t kHeaderBytes = 44;
constexpr std::uint64_t kMaxData = 0xFFFFFFFFull - kHeaderBytes;

void put16(std::array<char, kHeaderBytes>& h, std::size_t at, std::uint16_t v) {
    h[at] = static_cast<char>(v & 0xFF);
    h[at + 1] = static_cast<char>((v >> 8) & 0xFF);
}
void put32(std::array<char, kHeaderBytes>& h, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        h[at + static_cast<std::size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xFF);
    }
}
std::uint32_t get32(const char* p) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) {
        v = (v << 8) | static_cast<unsigned char>(p[i]);
    }
    return v;
}
std::uint16_t get16(const char* p) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(p[0]) | (static_cast<unsigned char>(p[1]) << 8));
}

std::array<char, kHeaderBytes> header(int channels, int rate, std::uint32_t dataBytes) {
    std::array<char, kHeaderBytes> h{};
    std::memcpy(h.data(), "RIFF", 4);
    put32(h, 4, 36 + dataBytes);
    std::memcpy(h.data() + 8, "WAVEfmt ", 8);
    put32(h, 16, 16);
    put16(h, 20, 3); // WAVE_FORMAT_IEEE_FLOAT
    put16(h, 22, static_cast<std::uint16_t>(channels));
    put32(h, 24, static_cast<std::uint32_t>(rate));
    put32(h, 28, static_cast<std::uint32_t>(rate) * static_cast<std::uint32_t>(channels) * 4);
    put16(h, 32, static_cast<std::uint16_t>(channels * 4));
    put16(h, 34, 32);
    std::memcpy(h.data() + 36, "data", 4);
    put32(h, 40, dataBytes);
    return h;
}

} // namespace

std::expected<WavWriter, Error> WavWriter::create(const std::filesystem::path& path, int channels, int rate) {
    if (channels < 1 || channels > 16 || rate < 8000 || rate > 768000) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "a WAV needs 1 to 16 channels and a rate of 8 to 768 kHz"));
    }
    WavWriter w;
    w.m_out.open(path, std::ios::binary | std::ios::trunc);
    if (!w.m_out) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot write " + pathToUtf8(path)));
    }
    const auto h = header(channels, rate, 0);
    w.m_out.write(h.data(), h.size());
    if (!w.m_out) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot write " + pathToUtf8(path)));
    }
    w.m_path = path;
    w.m_channels = channels;
    w.m_rate = rate;
    w.m_open = true;
    return w;
}

WavWriter::~WavWriter() {
    if (m_open) {
        (void)close();
    }
}

std::expected<void, Error> WavWriter::write(std::span<const float> interleaved) {
    if (!m_open) {
        return std::unexpected(Error(ErrorCode::IoError, "the WAV is closed"));
    }
    if (interleaved.size() % static_cast<std::size_t>(m_channels) != 0) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "a WAV write must hold whole frames"));
    }
    const std::uint64_t bytes = (static_cast<std::uint64_t>(m_frames) * static_cast<std::uint64_t>(m_channels) + interleaved.size()) * 4;
    if (bytes > kMaxData) {
        return std::unexpected(Error(ErrorCode::Unsupported, "the WAV would pass 4 GiB"));
    }
    m_out.write(reinterpret_cast<const char*>(interleaved.data()), static_cast<std::streamsize>(interleaved.size_bytes()));
    if (!m_out) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot write " + pathToUtf8(m_path)));
    }
    m_frames += static_cast<std::int64_t>(interleaved.size() / static_cast<std::size_t>(m_channels));
    return {};
}

std::expected<void, Error> WavWriter::close() {
    if (!m_open) {
        return {};
    }
    m_open = false;
    const auto dataBytes = static_cast<std::uint32_t>(static_cast<std::uint64_t>(m_frames) * static_cast<std::uint64_t>(m_channels) * 4);
    const auto h = header(m_channels, m_rate, dataBytes);
    m_out.seekp(0);
    m_out.write(h.data(), h.size());
    m_out.close();
    if (!m_out) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot finish " + pathToUtf8(m_path)));
    }
    return {};
}

std::expected<WavInfo, Error> readWavInfo(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot read " + pathToUtf8(path)));
    }
    std::array<char, kHeaderBytes> h{};
    in.read(h.data(), h.size());
    if (in.gcount() != static_cast<std::streamsize>(h.size()) || std::memcmp(h.data(), "RIFF", 4) != 0 ||
        std::memcmp(h.data() + 8, "WAVEfmt ", 8) != 0 || get16(h.data() + 20) != 3 || get16(h.data() + 34) != 32 ||
        std::memcmp(h.data() + 36, "data", 4) != 0) {
        return std::unexpected(Error(ErrorCode::ParseError, pathToUtf8(path) + " is not a float WAV"));
    }
    WavInfo info;
    info.channels = get16(h.data() + 22);
    info.rate = static_cast<int>(get32(h.data() + 24));
    if (info.channels <= 0) {
        return std::unexpected(Error(ErrorCode::ParseError, pathToUtf8(path) + " has no channels"));
    }
    info.frames = static_cast<std::int64_t>(get32(h.data() + 40) / (4u * static_cast<std::uint32_t>(info.channels)));
    return info;
}

std::expected<std::vector<float>, Error> readWavSamples(const std::filesystem::path& path, WavInfo* infoOut) {
    auto info = readWavInfo(path);
    if (!info) {
        return std::unexpected(info.error());
    }
    std::ifstream in(path, std::ios::binary);
    in.seekg(kHeaderBytes);
    std::vector<float> samples(static_cast<std::size_t>(info->frames) * static_cast<std::size_t>(info->channels));
    in.read(reinterpret_cast<char*>(samples.data()), static_cast<std::streamsize>(samples.size() * sizeof(float)));
    if (static_cast<std::size_t>(in.gcount()) != samples.size() * sizeof(float)) {
        return std::unexpected(Error(ErrorCode::ParseError, pathToUtf8(path) + " is shorter than its header says"));
    }
    if (infoOut != nullptr) {
        *infoOut = *info;
    }
    return samples;
}

} // namespace wabcut::ai::audio
