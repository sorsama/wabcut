#include <wabcut/capture/CaptureFiles.h>

#include <wabcut/model/MediaLocator.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <cerrno>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <fstream>
#include <memory>
#include <sstream>
#include <system_error>

namespace wabcut::capture {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kJournalMagic = "wabcut-capture-journal 1";
constexpr std::uintmax_t kMaxJournalBytes = 64 * 1024;

bool reservedName(std::string_view name) {
    static constexpr std::array<std::string_view, 4> kPlain = {"CON", "PRN", "AUX", "NUL"};
    std::string upper(name);
    std::ranges::transform(upper, upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    const auto dot = upper.find('.');
    if (dot != std::string::npos) {
        upper.resize(dot);
    }
    if (std::ranges::find(kPlain, upper) != kPlain.end()) {
        return true;
    }
    return upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) && upper[3] >= '1' && upper[3] <= '9';
}

bool isPlainFile(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    return !ec && status.type() == fs::file_type::regular;
}

std::string isoNow() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buffer[32] = {};
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

struct FormatContextDeleter {
    void operator()(AVFormatContext* p) const noexcept { avformat_close_input(&p); }
};
struct OutputContextDeleter {
    void operator()(AVFormatContext* p) const noexcept {
        if (p) {
            if (p->pb) {
                avio_closep(&p->pb);
            }
            avformat_free_context(p);
        }
    }
};
struct PacketDeleter {
    void operator()(AVPacket* p) const noexcept { av_packet_free(&p); }
};

} // namespace

std::string safeFileName(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool gap = false; // a dropped character or a run of spaces becomes one space between the words
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7f || ch == ' ' || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' ||
            ch == '|' || ch == '?' || ch == '*') {
            gap = !out.empty();
            continue;
        }
        if (gap) {
            out.push_back(' ');
            gap = false;
        }
        out.push_back(ch);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    std::size_t first = 0;
    while (first < out.size() && (out[first] == ' ' || out[first] == '.')) {
        ++first;
    }
    out.erase(0, first);
    constexpr std::size_t kMaxBytes = 100;
    if (out.size() > kMaxBytes) {
        std::size_t cut = kMaxBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
            --cut; // not in the middle of a character
        }
        out.resize(cut);
        while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
            out.pop_back();
        }
    }
    if (out.empty()) {
        return "Capture";
    }
    if (reservedName(out)) {
        out += "_";
    }
    return out;
}

std::string_view captureExtension() {
    return "mkv";
}

namespace {

bool stemTaken(const fs::path& folder, const std::string& stem, std::string_view extension) {
    std::error_code ec;
    const fs::path file = folder / (stem + "." + std::string(extension));
    return fs::exists(file, ec) || fs::exists(journalPath(file), ec);
}

} // namespace

std::string defaultClipName(const fs::path& folder) {
    for (int n = 1; n < 100000; ++n) {
        std::string name = std::format("Capture {}", n);
        if (!stemTaken(folder, name, "mkv")) {
            return name;
        }
    }
    return "Capture";
}

fs::path captureFilePath(const fs::path& folder, std::string_view name, std::string_view extension) {
    const std::string stem = safeFileName(name);
    if (!stemTaken(folder, stem, extension)) {
        return folder / (stem + "." + std::string(extension));
    }
    for (int n = 2; n < 100000; ++n) {
        const std::string candidate = std::format("{} ({})", stem, n);
        if (!stemTaken(folder, candidate, extension)) {
            return folder / (candidate + "." + std::string(extension));
        }
    }
    return folder / (stem + "." + std::string(extension));
}

Rate snapFrameRate(Rate reported) {
    if (!reported.isValid()) {
        return Rate{30, 1};
    }
    const double fps = static_cast<double>(reported.num) / reported.den;
    struct Standard {
        Rate rate;
        double fps;
    };
    static const std::array<Standard, 12> kStandard = {{
        {{24000, 1001}, 24000.0 / 1001.0}, {{24, 1}, 24.0},   {{25, 1}, 25.0},   {{30000, 1001}, 30000.0 / 1001.0},
        {{30, 1}, 30.0},                   {{50, 1}, 50.0},   {{60000, 1001}, 60000.0 / 1001.0}, {{60, 1}, 60.0},
        {{120000, 1001}, 120000.0 / 1001.0}, {{120, 1}, 120.0}, {{15, 1}, 15.0}, {{10, 1}, 10.0},
    }};
    for (const Standard& s : kStandard) {
        if (std::abs(fps - s.fps) / s.fps < 0.0005) { // 29.97 and 30 differ by 0.1 %
            return s.rate;
        }
    }
    return reported.normalized();
}

// ---- The journal ----------------------------------------------------------------------------------------------------

fs::path journalPath(const fs::path& file) {
    fs::path journal = file;
    journal += ".capture";
    return journal;
}

std::int64_t currentProcessId() {
#ifdef _WIN32
    return static_cast<std::int64_t>(GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(getpid());
#endif
}

std::int64_t processStartStamp(std::int64_t pid) {
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process) {
        return 0;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    std::int64_t stamp = 0;
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        stamp = static_cast<std::int64_t>((static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime);
    }
    CloseHandle(process);
    return stamp;
#else
    (void)pid;
    return 0;
#endif
}

bool processIsRunning(std::int64_t pid, std::int64_t stamp) {
    if (pid <= 0) {
        return false;
    }
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process) {
        return GetLastError() == ERROR_ACCESS_DENIED; // there, but not ours to look at
    }
    DWORD code = 0;
    bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    if (running && stamp != 0) {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
            const auto now = static_cast<std::int64_t>((static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime);
            running = now == stamp; // another process under a reused id is not the writer
        }
    }
    CloseHandle(process);
    return running;
#else
    (void)stamp;
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

std::expected<void, Error> writeJournal(const fs::path& file, const std::string& note) {
    const std::int64_t pid = currentProcessId();
    std::string text(kJournalMagic);
    text += std::format("\npid {}\nstamp {}\nstarted {}\n\n", pid, processStartStamp(pid), isoNow());
    text += note;
    std::ofstream out(journalPath(file), std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot write the capture journal next to " + pathToUtf8(file)});
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot write the capture journal next to " + pathToUtf8(file)});
    }
    return {};
}

std::optional<CaptureJournal> readJournal(const fs::path& file) {
    const fs::path path = journalPath(file);
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size > kMaxJournalBytes) {
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    in.read(text.data(), static_cast<std::streamsize>(size));
    text.resize(static_cast<std::size_t>(in.gcount()));
    const std::size_t headerEnd = text.find("\n\n");
    const std::string head = text.substr(0, headerEnd == std::string::npos ? text.size() : headerEnd);
    std::istringstream lines(head);
    std::string line;
    if (!std::getline(lines, line) || line != kJournalMagic) {
        return std::nullopt;
    }
    CaptureJournal journal;
    while (std::getline(lines, line)) {
        const std::size_t space = line.find(' ');
        if (space == std::string::npos) {
            continue;
        }
        const std::string_view key(line.data(), space);
        const std::string_view value(line.data() + space + 1, line.size() - space - 1);
        if (key == "pid") {
            (void)std::from_chars(value.data(), value.data() + value.size(), journal.pid);
        } else if (key == "stamp") {
            (void)std::from_chars(value.data(), value.data() + value.size(), journal.processStamp);
        } else if (key == "started") {
            journal.startedAt = std::string(value);
        }
    }
    if (headerEnd != std::string::npos) {
        journal.note = text.substr(headerEnd + 2);
    }
    return journal;
}

void removeJournal(const fs::path& file) {
    std::error_code ec;
    fs::remove(journalPath(file), ec);
}

std::vector<InterruptedCapture> findInterruptedCaptures(const fs::path& folder, const std::vector<fs::path>& activeFiles) {
    std::vector<InterruptedCapture> found;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        return found;
    }
    const std::int64_t self = currentProcessId();
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& journalFile = it->path();
        if (journalFile.extension() != ".capture" || !isPlainFile(journalFile)) {
            continue;
        }
        fs::path file = journalFile;
        file.replace_extension(); // drops ".capture"
        if (!isPlainFile(file)) {
            continue;
        }
        std::error_code linkEc;
        if (fs::hard_link_count(file, linkEc) != 1 || linkEc) {
            continue;
        }
        const auto journal = readJournal(file);
        if (!journal) {
            continue;
        }
        const bool mine = journal->pid == self;
        const bool active = std::ranges::any_of(activeFiles, [&](const fs::path& a) {
            std::error_code same;
            return fs::equivalent(a, file, same) && !same;
        });
        if (mine ? active : processIsRunning(journal->pid, journal->processStamp)) {
            continue;
        }
        InterruptedCapture capture;
        capture.file = file;
        capture.journal = *journal;
        std::error_code sizeEc;
        capture.bytes = fs::file_size(file, sizeEc);
        found.push_back(std::move(capture));
    }
    std::ranges::sort(found, [](const InterruptedCapture& a, const InterruptedCapture& b) { return a.file.filename() < b.file.filename(); });
    return found;
}

// ---- Recovery -------------------------------------------------------------------------------------------------------

std::expected<RecoveredCapture, Error> recoverCapture(const InterruptedCapture& capture) {
    const std::string source = pathToUtf8(capture.file);
    fs::path temp = capture.file;
    temp += ".recovering";
    const std::string target = pathToUtf8(temp);
    std::error_code ec;
    auto cleanup = [&] {
        std::error_code ignore;
        fs::remove(temp, ignore);
    };

    AVFormatContext* rawIn = nullptr;
    if (avformat_open_input(&rawIn, source.c_str(), nullptr, nullptr) < 0) {
        // Only a file too small to hold a picture (empty, or a bare header) is removed. A big one that cannot be opened (a
        // file another program holds, a codec this build lacks) may be the only copy of a take: it stays where it is.
        constexpr std::uintmax_t kBareHeaderBytes = 64 * 1024;
        std::error_code sizeError;
        const std::uintmax_t size = fs::file_size(capture.file, sizeError);
        if (!sizeError && size > kBareHeaderBytes) {
            return std::unexpected(Error{ErrorCode::IoError, "The interrupted capture could not be opened; it was left where it is"});
        }
        removeJournal(capture.file);
        fs::remove(capture.file, ec);
        return std::unexpected(Error{ErrorCode::NotFound, "The interrupted capture holds no picture"});
    }
    std::unique_ptr<AVFormatContext, FormatContextDeleter> in(rawIn);
    (void)avformat_find_stream_info(in.get(), nullptr); // a file cut short may not give every stream's details

    AVFormatContext* rawOut = nullptr;
    if (avformat_alloc_output_context2(&rawOut, nullptr, "matroska", target.c_str()) < 0 || !rawOut) {
        return std::unexpected(Error{ErrorCode::Unsupported, "Cannot create the repaired capture"});
    }
    std::unique_ptr<AVFormatContext, OutputContextDeleter> out(rawOut);
    std::vector<int> mapping(in->nb_streams, -1);
    int videoStream = -1;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const AVStream* is = in->streams[i];
        const AVMediaType type = is->codecpar->codec_type;
        if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        AVStream* os = avformat_new_stream(out.get(), nullptr);
        if (!os || avcodec_parameters_copy(os->codecpar, is->codecpar) < 0) {
            cleanup();
            return std::unexpected(Error{ErrorCode::EncoderError, "Cannot copy a stream of the interrupted capture"});
        }
        os->codecpar->codec_tag = 0;
        os->time_base = is->time_base;
        os->sample_aspect_ratio = is->sample_aspect_ratio;
        os->avg_frame_rate = is->avg_frame_rate;
        av_dict_copy(&os->metadata, is->metadata, 0);
        mapping[i] = os->index;
        if (type == AVMEDIA_TYPE_VIDEO && videoStream < 0) {
            videoStream = static_cast<int>(i);
        }
    }
    if (videoStream < 0) {
        cleanup();
        removeJournal(capture.file);
        fs::remove(capture.file, ec);
        return std::unexpected(Error{ErrorCode::NotFound, "The interrupted capture holds no picture"});
    }
    av_dict_copy(&out->metadata, in->metadata, 0);
    if (avio_open(&out->pb, target.c_str(), AVIO_FLAG_WRITE) < 0) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot create the repaired capture next to " + source});
    }
    if (avformat_write_header(out.get(), nullptr) < 0) {
        cleanup();
        return std::unexpected(Error{ErrorCode::EncoderError, "Cannot write the repaired capture's header"});
    }

    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    RecoveredCapture result;
    result.file = capture.file;
    std::int64_t lastEnd = 0;
    bool wrote = false;
    while (packet && av_read_frame(in.get(), packet.get()) >= 0) {
        const int index = packet->stream_index;
        if (index < 0 || index >= static_cast<int>(mapping.size()) || mapping[static_cast<std::size_t>(index)] < 0) {
            av_packet_unref(packet.get());
            continue;
        }
        AVStream* is = in->streams[index];
        AVStream* os = out->streams[mapping[static_cast<std::size_t>(index)]];
        if (index == videoStream) {
            ++result.videoFrames;
            const std::int64_t stamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
            if (stamp != AV_NOPTS_VALUE) {
                lastEnd = std::max(lastEnd, av_rescale_q(stamp + std::max<std::int64_t>(packet->duration, 1), is->time_base, AVRational{1, 1000000}));
            }
        } else {
            ++result.audioPackets;
        }
        av_packet_rescale_ts(packet.get(), is->time_base, os->time_base);
        packet->stream_index = os->index;
        packet->pos = -1;
        if (av_interleaved_write_frame(out.get(), packet.get()) < 0) {
            av_packet_unref(packet.get());
            break;
        }
        wrote = true;
    }
    if (result.videoFrames == 0) {
        out.reset();
        cleanup();
        removeJournal(capture.file);
        fs::remove(capture.file, ec);
        return std::unexpected(Error{ErrorCode::NotFound, "The interrupted capture holds no picture"});
    }
    if (!wrote) { // pictures were read but the repaired file refused them: the original is the only copy, keep it
        out.reset();
        cleanup();
        return std::unexpected(Error{ErrorCode::IoError, "Cannot write the repaired capture"});
    }
    if (av_write_trailer(out.get()) < 0) {
        out.reset();
        cleanup();
        return std::unexpected(Error{ErrorCode::IoError, "Cannot finish the repaired capture"});
    }
    avio_closep(&out->pb);
    out.reset();
    in.reset();

    fs::rename(temp, capture.file, ec);
    if (ec) {
        cleanup();
        return std::unexpected(Error{ErrorCode::IoError, "Cannot replace the interrupted capture: " + ec.message()});
    }
    removeJournal(capture.file);
    result.seconds = static_cast<double>(lastEnd) / 1e6;
    return result;
}

} // namespace wabcut::capture
