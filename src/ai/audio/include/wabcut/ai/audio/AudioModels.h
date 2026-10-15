#pragma once

// The local audio models (A.4; docs/ai.md "Enhance Speech" and "Stem separation") and their files in the model
// store: DeepFilterNet3 (an archive of three ONNX graphs, extracted with the system's tar and verified graph by graph)
// and HTDemucs (one ONNX graph), both on the licence allow-list (MIT), downloaded on first use. Also the memory
// budget stem separation needs: it refuses up front on a computer that cannot hold one segment's inference.

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/llm/SystemProbe.h>
#include <wabcut/model/Error.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::audio {

// ---- Enhance Speech: DeepFilterNet3 ----

/// The manifest entry of DeepFilterNet3's ONNX archive ("deepfilternet3").
const ModelInfo& speechModel();

/// A file the archive must contain, with its size and SHA-256 (checked after extraction).
struct ArchiveMember {
    std::string_view name;
    std::uint64_t size = 0;
    std::string_view sha256;
};
/// enc.onnx, erb_dec.onnx, df_dec.onnx and config.ini.
std::span<const ArchiveMember> speechModelMembers();

/// What an enhanced take was rendered with: the model's digest and the version of the signal processing around it.
/// A take rendered with another version is stale.
std::string speechModelVersion();

/// Where the extracted graphs live: <store root>/deepfilternet3-84d57ec.
std::filesystem::path speechModelDirectory(const ModelStore& store);
/// The graphs are extracted and were verified.
bool speechModelInstalled(const ModelStore& store);

/// The system's tar (bsdtar on Windows: %SystemRoot%\System32\tar.exe; /usr/bin/tar elsewhere), absolute.
std::filesystem::path defaultExtractor();

/// Makes DeepFilterNet3 ready: downloads the archive (with `transport`; none = it must be in the store already),
/// verifies it, extracts it with `extractor` (absolute) and checks every graph's size and digest; returns the
/// directory. Errors: NotFound (not downloaded, no transport), IoError, InvalidArgument (a digest mismatch; nothing is
/// installed), Cancelled.
std::expected<std::filesystem::path, Error> installSpeechModel(ModelStore& store, IDownloadTransport* transport,
                                                              const std::filesystem::path& extractor,
                                                              const std::function<void(const DownloadProgress&)>& progress = {},
                                                              std::stop_token stop = {});

// ---- Stem separation: HTDemucs ----

/// The manifest entry ("htdemucs").
const ModelInfo& stemModel();

/// How the model is fed: 44.1 kHz stereo segments of 7.8 s with a quarter overlap, four stems in this order.
struct StemModelSpec {
    int sampleRate = 44100;
    int segment = 343980;
    int overlap = 343980 / 4;
    std::vector<std::string> stems{"drums", "bass", "other", "vocals"};
};
const StemModelSpec& stemModelSpec();
/// The model's digest and the separation version (what the stems' provenance records).
std::string stemModelVersion();

/// What one segment needs in RAM, ONNX Runtime on the CPU (measured ~1.1 GB; budgeted with headroom).
inline constexpr std::uint64_t kStemRamBudget = std::uint64_t{2560} << 20; // 2.5 GiB

/// Where stem separation runs on this computer: the CPU, unless `requested` is "gpu" (the export's inverse STFT, a
/// transposed convolution with a 4096-tap kernel, runs DirectML out of memory even with 10 GB free on an RTX 3060, so
/// "auto" does not try the GPU and lose the time of a failed attempt). Errors: Unsupported when the computer has less
/// than kStemRamBudget of RAM available ("stem separation needs 2.7 GB of free memory; 1.2 GB is available").
std::expected<std::string, Error> stemDevice(const llm::SystemProbe& probe, std::string_view requested = "auto");

} // namespace wabcut::ai::audio
