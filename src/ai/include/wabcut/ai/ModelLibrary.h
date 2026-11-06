#pragma once

// The local model library (r07, docs/ai.md "Local model library"): what Settings > AI > Local models browses, sorts,
// filters, downloads and runs. Pure C++ without Qt or network: the bridge (LocalModelsController) fetches the Hugging
// Face Hub through the AI network stack and hands the JSON here.
//
//   - The Hub: search URLs (GET https://huggingface.co/api/models), a model's details with its files and their LFS
//     SHA-256 (?blobs=true) or its tree, parsed into HubModel / HubFile. Everything from the Hub is untrusted input:
//     repo ids, file paths and digests are validated, anything else is display text.
//   - Variants: a GGUF repository's quantizations (Q4_K_M, Q8_0, ...), split shards grouped, vision projectors
//     (mmproj) apart.
//   - Benchmarks: a bundled, sourced table (src/ai/data/local-benchmarks.json: base model family -> published scores
//     with the page they come from) matched to repositories by their base model, plus a repository's own model-index
//     results. Only quantizations of a family carry its scores; fine-tunes and merges have no published score.
//   - Entries, filters and sort orders of the browse list.
//   - The installed library: <store>/library/index.json lists what was downloaded (files pinned to a revision with
//     their size and SHA-256); modelInfosOf() turns an entry into ModelStore entries (registerModel) and
//     registrySpecOf() into the local assistant's registry model (provider "local-llm").

#include <wabcut/ai/AiError.h>
#include <wabcut/ai/Http.h>
#include <wabcut/ai/JsonAccess.h>
#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/ModelStore.h>
#include <wabcut/model/Error.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::library {

/// What a model is good for (the library's task filter).
enum class Task { Chat, Coding, Reasoning, Vision, Transcription, Translation };
std::string_view toString(Task task);
std::optional<Task> taskFromString(std::string_view text);

/// The browse list's orders.
enum class SortKey { Recommended, Benchmark, Downloads, Likes, Trending, Newest, Updated, Smallest };
std::string_view toString(SortKey key);
std::optional<SortKey> sortKeyFromString(std::string_view text);

// ---- Hugging Face Hub ---------------------------------------------------------------------------------------------

/// "owner/name": each part 1-96 characters of letters, digits, '-', '_' and '.', not "." or "..".
bool isValidRepoId(std::string_view repo);
/// A repository file path: relative, '/'-separated, no "." or ".." parts, no backslash, colon or control character.
bool isSafeRepoPath(std::string_view path);

struct HubQuery {
    std::string search;          ///< free text; empty = every GGUF model
    std::string author;          ///< one publisher only (a valid namespace), or empty
    SortKey sort = SortKey::Downloads; ///< Downloads, Likes, Trending, Newest and Updated sort on the Hub; the others
                                       ///< fetch by downloads and sort here
    int limit = 40;              ///< 1..100
    double minParamsB = 0.0;     ///< num_parameters=min:..,max:.. (0 = no bound)
    double maxParamsB = 0.0;
};
/// GET URL of a GGUF model search, with the fields the library shows (expand[]=...).
std::string hubSearchUrl(const HubQuery& query);
/// GET URL of one model with its files' sizes and LFS SHA-256 (?blobs=true).
std::string hubModelUrl(std::string_view repo);
/// GET URL of a repository tree at `revision` (recursive).
std::string hubTreeUrl(std::string_view repo, std::string_view revision);
/// The model's page for people.
std::string hubPageUrl(std::string_view repo);
/// The download URL of a file pinned to `revision` (path segments percent-encoded).
std::string hubFileUrl(std::string_view repo, std::string_view revision, std::string_view path);
/// The next page of a search: the Link header's rel="next" URL when it is https://huggingface.co/api/models?...
std::optional<std::string> nextPageUrl(const HttpHeaders& headers);

struct HubFile {
    std::string path;     ///< in the repository
    std::uint64_t size = 0;
    std::string sha256;   ///< the LFS object id (64 lowercase hex), empty for files not in LFS
    bool operator==(const HubFile&) const = default;
};

/// One result of a repository's model-index (its own evaluation results).
struct EvalResult {
    std::string metric;  ///< "MMLU-Pro", ...
    std::string dataset;
    double value = 0.0;
};

struct HubModel {
    std::string id;                 ///< "unsloth/Qwen3-8B-GGUF"
    std::string author;
    std::string revision;           ///< commit SHA (model details only)
    std::int64_t downloads = 0;     ///< last 30 days
    std::int64_t likes = 0;
    double trending = 0.0;
    std::string createdAt;          ///< ISO 8601
    std::string lastModified;
    std::vector<std::string> tags;
    std::string pipelineTag;
    std::string license;            ///< SPDX id ("Apache-2.0", "MIT", ...) or the Hub's own id ("llama3.1", "other")
    std::string licenseName;        ///< for people ("Apache License 2.0", "Llama 3.1 Community License", ...)
    bool gated = false;
    std::vector<std::string> baseModels; ///< base_model ids
    std::string baseRelation;       ///< "quantized", "finetune", "merge", "adapter" or empty
    std::vector<std::string> languages; ///< ISO 639-1 codes from the card
    double paramsB = 0.0;           ///< total parameters (gguf.total, else from the name), 0 = unknown
    double activeParamsB = 0.0;     ///< mixture of experts: active parameters from the name ("35B-A3B"), 0 = n/a
    std::string architecture;       ///< gguf.architecture
    std::int64_t contextLength = 0; ///< gguf.context_length
    std::uint64_t ggufFileSize = 0; ///< gguf.totalFileSize (the size of one quantization, as the Hub reports it)
    std::vector<HubFile> files;     ///< model details / tree only
    std::vector<EvalResult> evals;  ///< model-index
};

/// A search answer (a JSON array). Entries with an invalid id are skipped. Errors: ProviderError (not the expected JSON).
std::expected<std::vector<HubModel>, AiError> parseHubSearch(std::string_view json);
/// One model's details (?blobs=true: siblings with sizes and LFS SHA-256). Errors: ProviderError (not the expected JSON).
std::expected<HubModel, AiError> parseHubModel(std::string_view json);
/// A tree listing (files only; directories skipped; unsafe paths skipped). Errors: ProviderError (not the expected JSON).
std::expected<std::vector<HubFile>, AiError> parseHubTree(std::string_view json);

/// Parameter count from a model name: "Qwen3-8B" -> 8, "Qwen3-1.7B" -> 1.7, "gemma-4-E4B" -> 4, "SmolLM2-360M" -> 0.36,
/// "Mixtral-8x7B" -> 56; active parameters of a mixture of experts from "35B-A3B" -> 3. 0 when none.
double paramsFromName(std::string_view name, double* activeParamsB = nullptr);
/// The Hub's licence id as SPDX where there is one ("apache-2.0" -> "Apache-2.0"); others stay as they are.
std::string normalizeLicense(std::string_view hubLicense);
/// A readable name for a licence id ("llama3.1" -> "Llama 3.1 Community License").
std::string licenseDisplayName(std::string_view license, std::string_view cardName = {});
/// On the allow-list of ModelStore (isAllowedModelLicense): downloads without an extra question.
bool isPermissive(std::string_view license);

// ---- Variants -------------------------------------------------------------------------------------------------------

/// One downloadable quantization of a GGUF repository: its file, or every shard of a split file in order.
struct Variant {
    std::string quant;          ///< "Q4_K_M", "UD-Q4_K_XL", "Q8_0", "BF16", ...; the file name when none is recognisable
    std::vector<HubFile> files; ///< the first shard first
    std::uint64_t bytes = 0;
    double bits = 0.0;          ///< about how many bits a weight takes (sorting and the size estimate), 0 = unknown
};
/// "Qwen3-8B-Q4_K_M.gguf" -> "Q4_K_M", "Qwen3.8-27B-UD-Q4_K_XL.gguf" -> "UD-Q4_K_XL", "x-BF16-00001-of-00002.gguf" ->
/// "BF16"; "" when the name has none.
std::string quantOf(std::string_view fileName);
/// Approximate bits per weight of a quantization ("Q4_K_M" -> 4.8); 0 when unknown.
double bitsOf(std::string_view quant);
/// The repository's quantizations, smallest first: .gguf files that are not projectors (mmproj*), importance matrices
/// (imatrix*) or speculative-decoding drafts (mtp*); split files grouped (an incomplete set is left out); files
/// without a SHA-256 are left out (nothing unverifiable is offered).
std::vector<Variant> variantsOf(std::span<const HubFile> files);
/// The vision projectors (mmproj*.gguf with a SHA-256).
std::vector<HubFile> projectorsOf(std::span<const HubFile> files);
/// The quantization to offer first: `preferred` when present, else Q4_K_M, then other 4-bit kinds, then Q5_K_M, then
/// the one nearest 4.8 bits. Null when there is none.
const Variant* defaultVariant(std::span<const Variant> variants, std::string_view preferred = {});
/// F16, then BF16, then F32, then the first. Null when there is none.
const HubFile* defaultProjector(std::span<const HubFile> projectors);

// ---- Benchmarks -----------------------------------------------------------------------------------------------------

struct Metric {
    std::string id;          ///< "mmlu_pro"
    std::string name;        ///< "MMLU-Pro"
    std::string description; ///< what it measures, one line
    bool core = false;       ///< part of the composite score
};
struct Score {
    std::string metric;      ///< Metric::id
    double value = 0.0;      ///< percent, 0..100
    std::string mode;        ///< "thinking", "non-thinking", "instruct", ...
    std::string note;
    std::string source;      ///< where this score comes from, when not the family's source
};
struct BenchmarkFamily {
    std::string id;          ///< the base model's repository id, "Qwen/Qwen3-8B"
    std::string name;        ///< "Qwen3 8B"
    std::string publisher;
    double paramsB = 0.0;
    double activeParamsB = 0.0;
    std::string license;
    bool vision = false;
    std::vector<std::string> tasks;     ///< Task names
    std::vector<std::string> languages; ///< ISO 639-1 codes, or "multilingual"
    std::vector<std::string> aliases;   ///< other repository ids of the same weights (e.g. an unquantized QAT checkpoint)
    std::vector<Score> scores;
    std::string source;      ///< the page the scores were copied from
    std::string released;    ///< YYYY-MM-DD
    std::string checked;     ///< when the source was read, YYYY-MM-DD
    std::string note;
};
struct BenchmarkTable {
    std::string updated;
    std::vector<Metric> metrics;
    std::vector<BenchmarkFamily> families;

    const Metric* metric(std::string_view id) const;
    const BenchmarkFamily* family(std::string_view repoId) const; ///< by id or alias, case-insensitive
};
/// Parses a benchmark table (schemaVersion 1). Errors: ProviderError naming the entry (unknown metric, score out of
/// 0..100, a family without a source).
std::expected<BenchmarkTable, AiError> parseBenchmarks(std::string_view json);
/// The table compiled into the library (src/ai/data/local-benchmarks.json); empty if it fails to parse (a test
/// checks that it parses).
const BenchmarkTable& builtinBenchmarks();
/// The family whose published scores apply to `model`: a base model (or an alias) that is a family, when the
/// repository is that model or a quantization of it; without base model data, a repository name that is exactly a
/// family's name plus GGUF / quantization noise. Fine-tunes, merges and adapters get none. Null when nothing applies.
const BenchmarkFamily* matchFamily(const HubModel& model, const BenchmarkTable& table);
/// The family of a repository id alone (the curated picks, installed models): the family itself, an alias, or a
/// quantization repository named after it ("unsloth/Qwen3-8B-GGUF" -> Qwen/Qwen3-8B). Null when none.
const BenchmarkFamily* familyForRepo(std::string_view repo, const BenchmarkTable& table);
/// The mean of the family's core metrics that have a score; nullopt when it has none.
std::optional<double> compositeScore(const BenchmarkFamily& family, const BenchmarkTable& table);
/// The same for a repository's own model-index results (core metrics only, by name).
std::optional<double> compositeScore(std::span<const EvalResult> evals, const BenchmarkTable& table);

// ---- Entries, filters and sorting ----------------------------------------------------------------------------------

enum class Publisher { Official, Quantizer, Community };
std::string_view toString(Publisher publisher);
/// Well-known GGUF publishers (unsloth, bartowski, lmstudio-community, ggml-org, ...).
bool isKnownQuantizer(std::string_view author);

/// How a model would run here (filled by the caller from llm::estimateFit).
enum class Fit { Unknown = -1, No = 0, Cpu = 1, Gpu = 2 };

struct Entry {
    HubModel hub;
    std::vector<Task> tasks;
    bool vision = false;
    const BenchmarkFamily* family = nullptr;
    std::optional<double> score;        ///< composite benchmark score
    bool ownEvals = false;              ///< the score comes from the repository's own model-index
    Publisher publisher = Publisher::Community;
    bool permissive = false;
    std::uint64_t sizeBytes = 0;        ///< the default quantization's size; 0 = unknown
    /// sizeBytes is an estimate: a 4-bit (Q4_K_M) download from the parameter count. A search does not list files,
    /// and the Hub's gguf.totalFileSize is the size of one file of the repository, often a 16-bit one.
    bool sizeEstimated = false;
    std::string quant;                  ///< the default quantization, when known
    std::vector<std::string> quants;    ///< offered quantizations (details only)
    std::vector<std::string> languages; ///< card languages and the family's
    Fit fit = Fit::Unknown;
    bool installed = false;
    bool curated = false;
};

/// Whether llama-server can chat with it: a GGUF repository whose pipeline is text generation, image-text-to-text or
/// any-to-any (or none, for a conversational model), and not speech, image, embedding or other generation models.
bool isRunnableLlm(const HubModel& model);
/// What the model is good for: chat always; coding, reasoning, vision and translation from its tags, name and family.
std::vector<Task> tasksOf(const HubModel& model, const BenchmarkFamily* family);
/// An entry with everything derivable from the Hub data and the benchmark table (fit, installed and curated are the
/// caller's).
Entry makeEntry(HubModel model, const BenchmarkTable& table);

struct Filter {
    std::optional<Task> task;
    bool fitsOnly = false;          ///< Fit::Gpu or Fit::Cpu (unknown fits pass)
    std::string size = "any";       ///< download size: "any", "small" (< 3 GB), "medium" (3-8), "large" (8-20), "huge" (> 20)
    std::string params = "any";     ///< "any", "tiny" (< 3B), "small" (3-9B), "medium" (9-20B), "large" (20-40B), "huge" (> 40B)
    std::string quant;              ///< offered quantization ("" any; entries whose quantizations are unknown pass)
    bool permissiveOnly = true;     ///< licences on the allow-list only
    std::string language;           ///< ISO 639-1 ("" any); "multilingual" families pass
    std::string publisher = "any";  ///< "any", "official", "trusted" (official or a well-known quantizer)
    bool installedOnly = false;
};
bool matches(const Entry& entry, const Filter& filter);
/// Stable sort. Recommended: fit (GPU, CPU, unknown, no), curated first, score (none last), downloads. Benchmark: score
/// (none last), then downloads. Smallest: size (unknown last).
void sortEntries(std::vector<Entry>& entries, SortKey key);

// ---- The installed library -----------------------------------------------------------------------------------------

struct InstalledFile {
    std::string storeName;  ///< ModelInfo::fileName: "library/<owner>/<name>/<path>"
    std::string url;        ///< pinned to the revision
    std::uint64_t size = 0;
    std::string sha256;
    bool operator==(const InstalledFile&) const = default;
};
struct InstalledModel {
    std::string id;             ///< libraryModelId(): "hf:owner/name:QUANT"
    std::string repo;
    std::string revision;
    std::string quant;
    std::string name;           ///< "Qwen3 8B" (the family's) or the repository's name
    std::string publisher;      ///< the repository's owner
    std::string license;
    std::string licenseName;
    bool licenseAccepted = false;
    double paramsB = 0.0;
    std::int64_t contextLength = 0;
    bool vision = false;
    std::vector<std::string> tasks;
    std::string family;         ///< BenchmarkFamily::id, or empty
    std::vector<InstalledFile> files;       ///< the weights, the first shard first
    std::optional<InstalledFile> projector; ///< the vision projector
    std::string installedAt;    ///< YYYY-MM-DD
    // Advanced launch settings (llm::LaunchOverrides): 0 / -1 = automatic.
    int context = 0;
    int gpuLayers = -1;
    int threads = 0;
};

/// "hf:<repo>:<quant>".
std::string libraryModelId(std::string_view repo, std::string_view quant);
/// "library/<owner>/<name>/<path>" (a valid repo id and a safe path; otherwise empty).
std::string storeNameFor(std::string_view repo, std::string_view path);
/// What downloading `variant` (and `projector`) of `model` installs. `model` needs its revision; errors:
/// InvalidRequest (no revision, an invalid repo, a file without SHA-256, a licence that is neither permissive nor
/// accepted).
std::expected<InstalledModel, AiError> planInstall(const HubModel& model, const Variant& variant, const HubFile* projector,
                                                   bool licenseAccepted, const BenchmarkTable& table);
/// ModelStore entries: the first shard (id), the other shards (id + "#2", "#3", ...), the projector (id + "#mmproj").
std::vector<ModelInfo> modelInfosOf(const InstalledModel& model);
/// The local assistant's registry model (provider "local-llm", api "managed-llama"): chat with constrained tool calls
/// (llama-server's grammar) and structured output, images when it has a projector.
ModelSpec registrySpecOf(const InstalledModel& model);
/// The total download size.
std::uint64_t bytesOf(const InstalledModel& model);

Json toJson(const InstalledModel& model);
/// Validates an entry read from disk (ids, paths, digests); InvalidRequest names what is wrong.
std::expected<InstalledModel, AiError> installedFromJson(const Json& j);

/// <store>/library/index.json: what the library downloaded. Not thread-safe (one owner on the GUI thread).
class LibraryIndex {
public:
    explicit LibraryIndex(std::filesystem::path storeRoot);
    static std::filesystem::path fileFor(const std::filesystem::path& storeRoot);

    const std::filesystem::path& root() const { return m_root; }
    /// Reads the index: a missing file is an empty library; invalid entries are skipped. Errors: IoError (unreadable),
    /// InvalidArgument (not a library index; the file is left as it is).
    std::expected<void, Error> load();
    /// Writes it atomically (a temporary file renamed over the old one).
    std::expected<void, Error> save() const;

    const std::vector<InstalledModel>& models() const { return m_models; }
    const InstalledModel* find(std::string_view id) const;
    InstalledModel* find(std::string_view id);
    /// Adds or replaces the entry with that id.
    void upsert(InstalledModel model);
    /// Removes the entry (not its files). False when there was none.
    bool remove(std::string_view id);
    /// Store names of `id`'s files that no other entry uses (what deleting it may remove).
    std::vector<std::string> exclusiveFiles(std::string_view id) const;

private:
    std::filesystem::path m_root;
    std::vector<InstalledModel> m_models;
};

} // namespace wabcut::ai::library
