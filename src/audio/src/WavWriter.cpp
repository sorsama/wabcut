#include <wabcut/audio/WavWriter.h>

#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace wabcut::audio {

namespace {

constexpr std::int64_t kHeaderBytes = 44; // RIFF(12) + "fmt "(8 + 16) + "data"(8)
constexpr std::int64_t kMaxDataBytes = 0xFFFFFFFFLL - 36; // what the RIFF size (36 + data) can still describe
constexpr std::string_view kJournalMagic = "wabcut-wav-journal 1";
constexpr std::uintmax_t kMaxJournalBytes = 64 * 1024;

std::FILE* openFile(const std::filesystem::path& file, const char* mode) {
#if defined(_WIN32)
    const std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen(file.c_str(), wmode.c_str());
#else
    return std::fopen(file.c_str(), mode);
#endif
}

bool seekTo(std::FILE* f, std::int64_t offset) {
#if defined(_WIN32)
    return _fseeki64(f, offset, SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

void put16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}

void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

int bytesPerSample(const WavFormat& f) { return f.bitsPerSample / 8; }

std::array<std::uint8_t, kHeaderBytes> header(const WavFormat& f, std::uint32_t dataBytes) {
    std::array<std::uint8_t, kHeaderBytes> h{};
    const int bps = bytesPerSample(f);
    std::memcpy(h.data(), "RIFF", 4);
    put32(h.data() + 4, 36 + dataBytes);
    std::memcpy(h.data() + 8, "WAVEfmt ", 8);
    put32(h.data() + 16, 16);
    put16(h.data() + 20, f.bitsPerSample == 32 ? 3 : 1); // IEEE float or PCM
    put16(h.data() + 22, static_cast<std::uint16_t>(f.channels));
    put32(h.data() + 24, static_cast<std::uint32_t>(f.sampleRate));
    put32(h.data() + 28, static_cast<std::uint32_t>(f.sampleRate * f.channels * bps));
    put16(h.data() + 32, static_cast<std::uint16_t>(f.channels * bps));
    put16(h.data() + 34, static_cast<std::uint16_t>(f.bitsPerSample));
    std::memcpy(h.data() + 36, "data", 4);
    put32(h.data() + 40, dataBytes);
    return h;
}

Error ioError(const std::filesystem::path& file, const char* what) {
    return Error(ErrorCode::IoError, std::string(what) + " " + pathToUtf8(file) + ": " + std::strerror(errno));
}

/// The journal: the magic line, "<rate> <channels> <bits>", then the note verbatim.
bool writeJournal(const std::filesystem::path& file, const WavFormat& f, std::string_view note) {
    std::string text(kJournalMagic);
    text += '\n';
    text += std::to_string(f.sampleRate) + ' ' + std::to_string(f.channels) + ' ' + std::to_string(f.bitsPerSample) + '\n';
    text += note;
    std::ofstream out(wavJournalPath(file), std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    return static_cast<bool>(out);
}

/// One decimal number of `s` and the space after it.
bool takeNumber(std::string_view& s, int& value) {
    const char* end = s.data() + s.size();
    const auto [next, ec] = std::from_chars(s.data(), end, value);
    if (ec != std::errc{}) {
        return false;
    }
    s.remove_prefix(static_cast<std::size_t>(next - s.data()));
    if (!s.empty() && s.front() == ' ') {
        s.remove_prefix(1);
    }
    return true;
}

} // namespace

std::filesystem::path wavJournalPath(const std::filesystem::path& file) {
    std::filesystem::path journal = file;
    journal += ".journal";
    return journal;
}

std::optional<WavJournal> readWavJournal(const std::filesystem::path& file) {
    const std::filesystem::path journal = wavJournalPath(file);
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(journal, ec);
    if (ec || size > kMaxJournalBytes) {
        return std::nullopt;
    }
    std::ifstream in(journal, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!in || !in.read(text.data(), static_cast<std::streamsize>(text.size()))) {
        return std::nullopt;
    }
    const std::size_t first = text.find('\n');
    if (first == std::string::npos || std::string_view(text).substr(0, first) != kJournalMagic) {
        return std::nullopt;
    }
    const std::size_t second = text.find('\n', first + 1);
    if (second == std::string::npos) {
        return std::nullopt;
    }
    WavJournal out;
    std::string_view line = std::string_view(text).substr(first + 1, second - first - 1);
    if (!takeNumber(line, out.format.sampleRate) || !takeNumber(line, out.format.channels) ||
        !takeNumber(line, out.format.bitsPerSample) || !line.empty()) {
        return std::nullopt;
    }
    out.note = text.substr(second + 1);
    return out;
}

void removeWavJournal(const std::filesystem::path& file) {
    std::error_code ec;
    std::filesystem::remove(wavJournalPath(file), ec);
}

std::expected<std::unique_ptr<WavWriter>, Error> WavWriter::create(const std::filesystem::path& file, WavFormat format,
                                                                   std::int64_t syncFrames, std::string_view journalNote) {
    if (format.sampleRate < 1000 || format.sampleRate > 768000 || format.channels < 1 || format.channels > 8 ||
        (format.bitsPerSample != 16 && format.bitsPerSample != 24 && format.bitsPerSample != 32)) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "unsupported WAV format"));
    }
    std::FILE* f = openFile(file, "wb");
    if (f == nullptr) {
        return std::unexpected(ioError(file, "cannot create"));
    }
    const auto h = header(format, 0);
    if (std::fwrite(h.data(), 1, h.size(), f) != h.size() || std::fflush(f) != 0) {
        std::fclose(f);
        return std::unexpected(ioError(file, "cannot write"));
    }
    // The journal: this file is a take recoverWav() may repair after a crash (A5-P2-12).
    if (!writeJournal(file, format, journalNote)) {
        std::fclose(f);
        std::error_code ec;
        std::filesystem::remove(file, ec);
        removeWavJournal(file);
        return std::unexpected(Error(ErrorCode::IoError, "cannot write the journal of " + pathToUtf8(file)));
    }
    return std::unique_ptr<WavWriter>(new WavWriter(f, file, format, std::max<std::int64_t>(syncFrames, 1)));
}

WavWriter::WavWriter(std::FILE* file, std::filesystem::path path, WavFormat format, std::int64_t syncFrames)
    : m_file(file), m_path(std::move(path)), m_format(format), m_syncFrames(syncFrames) {}

WavWriter::~WavWriter() { (void)finish(); }

std::expected<void, Error> WavWriter::write(std::span<const float> interleaved) {
    if (m_file == nullptr) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the WAV is finished"));
    }
    const auto ch = static_cast<std::size_t>(m_format.channels);
    const std::size_t frames = interleaved.size() / ch;
    const int bps = bytesPerSample(m_format);
    std::vector<std::uint8_t> bytes(frames * ch * static_cast<std::size_t>(bps));
    std::uint8_t* out = bytes.data();
    for (std::size_t i = 0; i < frames * ch; ++i) {
        const float v = std::clamp(interleaved[i], -1.0f, 1.0f);
        if (m_format.bitsPerSample == 32) {
            std::memcpy(out, &v, 4);
            out += 4;
        } else if (m_format.bitsPerSample == 24) {
            const auto s = static_cast<std::int32_t>(std::lround(static_cast<double>(v) * 8388607.0));
            out[0] = static_cast<std::uint8_t>(s);
            out[1] = static_cast<std::uint8_t>(s >> 8);
            out[2] = static_cast<std::uint8_t>(s >> 16);
            out += 3;
        } else {
            const auto s = static_cast<std::int16_t>(std::lround(static_cast<double>(v) * 32767.0));
            out[0] = static_cast<std::uint8_t>(s);
            out[1] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(s) >> 8);
            out += 2;
        }
    }
    if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), m_file) != bytes.size()) {
        return std::unexpected(ioError(m_path, "cannot write"));
    }
    m_frames += static_cast<std::int64_t>(frames);
    m_unsynced += static_cast<std::int64_t>(frames);
    if (m_unsynced >= m_syncFrames) {
        return writeSizes();
    }
    return {};
}

std::expected<void, Error> WavWriter::writeSizes() {
    const std::int64_t dataBytes = m_frames * m_format.channels * bytesPerSample(m_format);
    const auto h = header(m_format, static_cast<std::uint32_t>(std::min<std::int64_t>(dataBytes, 0xFFFFFFFFLL - 36)));
    // The sizes, then back to the end for the next samples; flushed so a crash leaves this much readable.
    if (!seekTo(m_file, 4) || std::fwrite(h.data() + 4, 1, 4, m_file) != 4 || !seekTo(m_file, 40) ||
        std::fwrite(h.data() + 40, 1, 4, m_file) != 4 || !seekTo(m_file, kHeaderBytes + dataBytes) || std::fflush(m_file) != 0) {
        return std::unexpected(ioError(m_path, "cannot update"));
    }
    m_unsynced = 0;
    return {};
}

std::expected<void, Error> WavWriter::finish() {
    if (m_file == nullptr) {
        return {};
    }
    auto sized = writeSizes();
    const bool closed = std::fclose(m_file) == 0;
    m_file = nullptr;
    if (!sized) {
        return sized;
    }
    if (!closed) {
        return std::unexpected(ioError(m_path, "cannot close"));
    }
    return {};
}

namespace {

/// recoverWav() with `repair`, inspectWav() without (then the file is only read).
std::expected<RecoveredWav, Error> examineWav(const std::filesystem::path& file, bool repair) {
    const std::string name = pathToUtf8(file);
    std::error_code ec;
    // The path itself, links not followed: a link (or junction) to a file elsewhere is never read or rewritten (S5-P2-11).
    const std::filesystem::file_type type = std::filesystem::symlink_status(file, ec).type();
    if (ec || type == std::filesystem::file_type::not_found) {
        return std::unexpected(Error(ErrorCode::NotFound, "no file " + name));
    }
    if (type != std::filesystem::file_type::regular) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, name + " is a link or not a file: it is left as it is"));
    }
    const auto size = static_cast<std::int64_t>(std::filesystem::file_size(file, ec));
    if (ec) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot read " + name));
    }
    // Read only: a file that is not to be repaired is never opened for writing.
    std::array<std::uint8_t, kHeaderBytes> h{};
    {
        std::FILE* f = openFile(file, "rb");
        if (f == nullptr) {
            return std::unexpected(ioError(file, "cannot open"));
        }
        const bool read = size >= kHeaderBytes && std::fread(h.data(), 1, h.size(), f) == h.size();
        std::fclose(f);
        if (!read || std::memcmp(h.data(), "RIFF", 4) != 0 || std::memcmp(h.data() + 8, "WAVEfmt ", 8) != 0 ||
            get32(h.data() + 16) != 16 || std::memcmp(h.data() + 36, "data", 4) != 0) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, name + " is not a canonical WAV"));
        }
    }
    RecoveredWav out;
    out.format.channels = get16(h.data() + 22);
    out.format.sampleRate = static_cast<int>(get32(h.data() + 24));
    out.format.bitsPerSample = get16(h.data() + 34);
    const int frameBytes = out.format.channels * (out.format.bitsPerSample / 8);
    if (frameBytes <= 0 || (out.format.bitsPerSample != 16 && out.format.bitsPerSample != 24 && out.format.bitsPerSample != 32)) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, name + " has an unsupported sample format"));
    }
    const std::int64_t available = size - kHeaderBytes;
    if (available > kMaxDataBytes) {
        // Its sizes would wrap in the 32-bit header fields.
        return std::unexpected(Error(ErrorCode::InvalidArgument, name + " is larger than a WAV header can describe (4 GiB)"));
    }
    out.frames = available / frameBytes;
    out.droppedBytes = available - out.frames * frameBytes;
    const std::int64_t dataBytes = out.frames * frameBytes;
    if (static_cast<std::int64_t>(get32(h.data() + 40)) == dataBytes && static_cast<std::int64_t>(get32(h.data() + 4)) == 36 + dataBytes &&
        out.droppedBytes == 0) {
        return out; // the header covers the file
    }
    // Only a take this writer made is rewritten: another recorder's WAV (a LIST or bext chunk after "data") is not one
    // whose length is its data (A5-P2-12).
    const std::optional<WavJournal> journal = readWavJournal(file);
    if (!journal || journal->format != out.format) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, name + " was not recorded by Wabcut: it is left as it is"));
    }
    out.needsRepair = true;
    if (!repair) {
        return out;
    }
    // A file with another name elsewhere (a hard link) is not only this take: it is not changed either.
    if (std::filesystem::hard_link_count(file, ec) != 1 || ec) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, name + " has other names (hard links): it is left as it is"));
    }
    std::FILE* f = openFile(file, "r+b");
    if (f == nullptr) {
        return std::unexpected(ioError(file, "cannot open"));
    }
    out.repaired = true;
    const auto fixed = header(out.format, static_cast<std::uint32_t>(dataBytes));
    const bool ok = seekTo(f, 4) && std::fwrite(fixed.data() + 4, 1, 4, f) == 4 && seekTo(f, 40) &&
                    std::fwrite(fixed.data() + 40, 1, 4, f) == 4 && std::fflush(f) == 0;
    std::fclose(f);
    if (!ok) {
        return std::unexpected(ioError(file, "cannot repair"));
    }
    if (out.droppedBytes != 0) {
        std::filesystem::resize_file(file, static_cast<std::uintmax_t>(kHeaderBytes + dataBytes), ec);
        if (ec) {
            return std::unexpected(Error(ErrorCode::IoError, "cannot truncate " + name + ": " + ec.message()));
        }
    }
    return out;
}

} // namespace

std::expected<RecoveredWav, Error> recoverWav(const std::filesystem::path& file) { return examineWav(file, true); }

std::expected<RecoveredWav, Error> inspectWav(const std::filesystem::path& file) { return examineWav(file, false); }

} // namespace wabcut::audio
