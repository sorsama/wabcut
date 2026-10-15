#include <wabcut/ai/audio/AudioModels.h>

#include <wabcut/ai/ChildProcess.h>
#include <wabcut/ai/Sha256.h>
#include <wabcut/model/MediaLocator.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>

namespace wabcut::ai::audio {

namespace {

/// Bumped when the signal processing around DeepFilterNet3 changes what a take sounds like (DeepFilter.h, Plans.h).
constexpr int kSpeechProcessingVersion = 1;
/// Bumped when the stem pipeline changes (segmenting, overlap, normalisation).
constexpr int kStemProcessingVersion = 1;

constexpr std::array<ArchiveMember, 4> kSpeechMembers{{
    {"enc.onnx", 1954042, "7c5399d3da8a50ebef1c1a0ae421b33376aa5e45d0e92df16da7e83c9c131916"},
    {"erb_dec.onnx", 3292397, "ab669a1d10afe20911728b33053a452071042317a90581092b325da7b2f9d895"},
    {"df_dec.onnx", 3340803, "23114ce3b0f6464b763ee62f7bb8aab6b2a129a21eabd5bcfe59413db05f278a"},
    {"config.ini", 2067, "415eb925d44990d938fb739f514aa3662c1ec0ea836cff044fa1291b82cb4290"},
}};

const ModelInfo& required(std::string_view id) {
    const ModelInfo* m = findModel(id);
    if (m == nullptr) {
        std::abort(); // the manifest is compiled in; a missing entry is a build error
    }
    return *m;
}

std::expected<std::string, Error> fileSha256(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot read " + pathToUtf8(file)));
    }
    Sha256 sha;
    std::array<char, 1 << 16> buffer{};
    while (in) {
        in.read(buffer.data(), buffer.size());
        const auto n = static_cast<std::size_t>(in.gcount());
        sha.update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), n));
    }
    return sha.finishHex();
}

std::string randomSuffix() {
    std::random_device device;
    std::mt19937_64 rng((static_cast<std::uint64_t>(device()) << 32) ^ device());
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 12; ++i) {
        out += kHex[rng() % 16];
    }
    return out;
}

std::filesystem::path markerOf(const std::filesystem::path& directory) {
    return directory / ".verified";
}

std::string gb(std::uint64_t bytes) {
    return llm::formatBytes(bytes);
}

} // namespace

const ModelInfo& speechModel() {
    static const ModelInfo& m = required("deepfilternet3");
    return m;
}

std::span<const ArchiveMember> speechModelMembers() {
    return kSpeechMembers;
}

std::string speechModelVersion() {
    return "deepfilternet3@" + speechModel().sha256.substr(0, 12) + "/dsp" + std::to_string(kSpeechProcessingVersion);
}

std::filesystem::path speechModelDirectory(const ModelStore& store) {
    return store.root() / "deepfilternet3-84d57ec";
}

bool speechModelInstalled(const ModelStore& store) {
    const std::filesystem::path dir = speechModelDirectory(store);
    std::error_code ec;
    for (const ArchiveMember& m : kSpeechMembers) {
        if (std::filesystem::file_size(dir / std::string(m.name), ec) != m.size || ec) {
            return false;
        }
    }
    std::ifstream marker(markerOf(dir));
    std::string text;
    std::getline(marker, text);
    return text == speechModel().sha256;
}

std::filesystem::path defaultExtractor() {
#ifdef _WIN32
    const char* root = std::getenv("SystemRoot");
    return std::filesystem::path(root != nullptr && *root != '\0' ? root : "C:\\Windows") / "System32" / "tar.exe";
#else
    return "/usr/bin/tar";
#endif
}

std::expected<std::filesystem::path, Error> installSpeechModel(ModelStore& store, IDownloadTransport* transport,
                                                              const std::filesystem::path& extractor,
                                                              const std::function<void(const DownloadProgress&)>& progress,
                                                              std::stop_token stop) {
    const std::filesystem::path target = speechModelDirectory(store);
    if (speechModelInstalled(store)) {
        return target;
    }
    if (!extractor.is_absolute()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the archive extractor must be an absolute path"));
    }
    auto archive = store.ensure(speechModel(), transport, progress, stop);
    if (!archive) {
        return std::unexpected(archive.error());
    }
    if (auto verified = store.verify(speechModel()); !verified) {
        return std::unexpected(verified.error());
    }
    // Extract into a fresh directory next to the target, check every graph, then move it into place.
    const std::filesystem::path scratch = store.root() / ("deepfilternet3-" + randomSuffix() + ".part");
    std::error_code ec;
    std::filesystem::create_directories(scratch, ec);
    if (ec) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot create " + pathToUtf8(scratch)));
    }
    const auto cleanup = [&] {
        std::error_code ignored;
        std::filesystem::remove_all(scratch, ignored);
    };
    ChildProcessOptions options;
    options.executable = extractor;
    options.lowPriority = true;
    options.arguments = {"-xzf", pathToUtf8(*archive), "-C", pathToUtf8(scratch)};
    auto process = ChildProcess::spawn(options);
    if (!process) {
        cleanup();
        return std::unexpected(Error(process.error().code, "cannot run " + pathToUtf8(extractor) + ": " + process.error().message));
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    while (!(*process)->wait(std::chrono::milliseconds(50))) {
        if (stop.stop_requested() || std::chrono::steady_clock::now() > deadline) {
            (*process)->kill();
            cleanup();
            return std::unexpected(Error(stop.stop_requested() ? ErrorCode::Cancelled : ErrorCode::IoError,
                                         stop.stop_requested() ? "cancelled" : "extracting DeepFilterNet3 timed out"));
        }
    }
    if (const int exit = (*process)->exitCode().value_or(-1); exit != 0) {
        cleanup();
        return std::unexpected(Error(ErrorCode::IoError, "extracting DeepFilterNet3 failed (tar exit code " + std::to_string(exit) + ")"));
    }
    // The archive keeps the graphs under tmp/export/; take each by name wherever it is, once.
    const std::filesystem::path staged = scratch / "graphs";
    std::filesystem::create_directories(staged, ec);
    for (const ArchiveMember& member : kSpeechMembers) {
        std::filesystem::path found;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(scratch, ec)) {
            if (entry.is_regular_file() && entry.path().filename() == std::string(member.name) &&
                entry.path().parent_path() != staged) {
                found = entry.path();
                break;
            }
        }
        if (found.empty()) {
            cleanup();
            return std::unexpected(Error(ErrorCode::InvalidArgument, "the DeepFilterNet3 archive has no " + std::string(member.name)));
        }
        const auto digest = fileSha256(found);
        if (!digest || *digest != member.sha256 || std::filesystem::file_size(found, ec) != member.size) {
            cleanup();
            return std::unexpected(Error(ErrorCode::InvalidArgument, std::string(member.name) + " does not match its pinned SHA-256"));
        }
        std::filesystem::rename(found, staged / std::string(member.name), ec);
        if (ec) {
            cleanup();
            return std::unexpected(Error(ErrorCode::IoError, "cannot move " + std::string(member.name)));
        }
    }
    {
        std::ofstream marker(markerOf(staged), std::ios::trunc);
        marker << speechModel().sha256 << '\n';
        if (!marker) {
            cleanup();
            return std::unexpected(Error(ErrorCode::IoError, "cannot write the DeepFilterNet3 marker"));
        }
    }
    std::filesystem::remove_all(target, ec);
    std::filesystem::rename(staged, target, ec);
    cleanup();
    if (ec || !speechModelInstalled(store)) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot install DeepFilterNet3 into " + pathToUtf8(target)));
    }
    return target;
}

const ModelInfo& stemModel() {
    static const ModelInfo& m = required("htdemucs");
    return m;
}

const StemModelSpec& stemModelSpec() {
    static const StemModelSpec spec;
    return spec;
}

std::string stemModelVersion() {
    return "htdemucs@" + stemModel().sha256.substr(0, 12) + "/sep" + std::to_string(kStemProcessingVersion);
}

std::expected<std::string, Error> stemDevice(const llm::SystemProbe& probe, std::string_view requested) {
    if (probe.ramAvailable != 0 && probe.ramAvailable < kStemRamBudget) {
        return std::unexpected(Error(ErrorCode::Unsupported, "stem separation needs " + gb(kStemRamBudget) +
                                                                 " of free memory; " + gb(probe.ramAvailable) + " is available"));
    }
    return std::string(requested == "gpu" ? "gpu" : "cpu");
}

} // namespace wabcut::ai::audio
