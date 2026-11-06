// r07 local model library (Local-AI engineer): Hugging Face search / model / tree parsing from recorded answers
// (tests/ai/fixtures/hf, read 2026-11-03, chat templates removed), quantization variants, the bundled benchmark table
// and its matching, filters and sort orders, the installed library index, the download queue over a fake transport
// (progress, Range resume, pause, cancel, SHA-256 mismatch, disk space, a failed write) and the launch overrides.
// Nothing here touches the network.

#include "AiTestUtil.h"

#include <wabcut/ai/ModelDownloads.h>
#include <wabcut/ai/ModelLibrary.h>
#include <wabcut/ai/Sha256.h>
#include <wabcut/ai/llm/LocalRuntime.h>
#include <wabcut/ai/llm/Tiers.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

using namespace wabcut;
using namespace wabcut::ai;
namespace lib = wabcut::ai::library;

namespace {

constexpr std::uint64_t GiB = std::uint64_t{1} << 30;

std::string fixture(const std::string& name) {
    std::ifstream in(std::filesystem::path(WABCUT_HF_FIXTURES) / name, std::ios::binary);
    REQUIRE(in);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

const lib::HubModel* byId(const std::vector<lib::HubModel>& models, std::string_view id) {
    for (const auto& m : models) {
        if (m.id == id)
            return &m;
    }
    return nullptr;
}

std::vector<lib::HubModel> searchFixture() {
    auto parsed = lib::parseHubSearch(fixture("search-qwen3-8b.json"));
    REQUIRE(parsed.has_value());
    return *parsed;
}

lib::HubModel modelFixture() {
    auto parsed = lib::parseHubModel(fixture("model-qwen3-8b-gguf.json"));
    REQUIRE(parsed.has_value());
    return *parsed;
}

llm::SystemProbe desktop(std::uint64_t ramFree, std::uint64_t gpuFree) {
    llm::SystemProbe p;
    p.os = "windows";
    p.arch = "x64";
    p.cpuThreads = 16;
    p.ramTotal = 32 * GiB;
    p.ramAvailable = ramFree;
    p.vulkanLoader = true;
    p.gpus.push_back({"NVIDIA GeForce RTX 3060", llm::GpuVendor::Nvidia, 12 * GiB, gpuFree, false});
    return p;
}

std::string randomBody(std::size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::string s(n, '\0');
    for (char& c : s)
        c = static_cast<char>(rng() & 0xFF);
    return s;
}

ModelInfo fileModel(const std::string& id, const std::string& body, std::string license = "MIT") {
    ModelInfo m;
    m.id = id;
    m.kind = ModelKind::Llm;
    std::string file = id;
    std::replace(file.begin(), file.end(), ':', '-');
    m.fileName = "library/test/repo/" + file + ".gguf";
    m.url = "https://huggingface.co/test/repo/resolve/0123456789abcdef0123456789abcdef01234567/" + id + ".gguf";
    m.size = body.size();
    m.sha256 = Sha256::hex(body);
    m.license = std::move(license);
    return m;
}

/// Serves bodies by URL in 1000-byte pieces, honouring Range. `holdAt`: after that many bytes of one response it waits
/// until the stop token fires (a slow network the test pauses or cancels). `corrupt` flips a byte. Never the network.
class FakeDownloads final : public IDownloadTransport {
public:
    std::map<std::string, std::string> bodies;
    std::atomic<std::uint64_t> holdAt{0}; ///< 0 = never hold
    std::atomic<bool> corrupt{false};
    std::atomic<int> holding{0};

    std::vector<std::pair<std::string, std::uint64_t>> requests() {
        std::lock_guard lock(m_mutex);
        return m_requests;
    }

    std::expected<Response, Error> get(const std::string& url, std::uint64_t offset,
                                       const std::function<bool(const Response&)>& onResponse,
                                       const std::function<bool(std::span<const std::byte>)>& onData,
                                       std::stop_token stop) override {
        {
            std::lock_guard lock(m_mutex);
            m_requests.emplace_back(url, offset);
        }
        const auto it = bodies.find(url);
        if (it == bodies.end()) {
            Response notFound{404, std::nullopt};
            onResponse(notFound);
            return notFound;
        }
        std::string data = it->second.substr(static_cast<std::size_t>(offset));
        if (corrupt && !data.empty())
            data[data.size() / 2] ^= 0x5A;
        Response r{offset == 0 ? 200 : 206, it->second.size()};
        if (!onResponse(r))
            return std::unexpected(Error{ErrorCode::Cancelled, "stopped"});
        std::uint64_t served = 0;
        for (std::size_t i = 0; i < data.size();) {
            if (holdAt > 0 && served >= holdAt) {
                ++holding;
                while (!stop.stop_requested())
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                --holding;
            }
            if (stop.stop_requested())
                return std::unexpected(Error{ErrorCode::Cancelled, "cancelled"});
            const std::size_t n = std::min<std::size_t>(1000, data.size() - i);
            if (!onData(std::as_bytes(std::span(data.data() + i, n))))
                return std::unexpected(Error{ErrorCode::Cancelled, "stopped"});
            served += n;
            i += n;
        }
        return r;
    }

private:
    std::mutex m_mutex;
    std::vector<std::pair<std::string, std::uint64_t>> m_requests;
};

template <class Pred>
bool waitUntil(Pred pred, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

} // namespace

TEST_CASE("Model library parses a Hugging Face GGUF search", "[ai][library]") {
    const auto models = searchFixture();
    REQUIRE(models.size() == 8);
    const lib::HubModel* qwen = byId(models, "Qwen/Qwen3-8B-GGUF");
    REQUIRE(qwen != nullptr);
    CHECK(qwen->author == "Qwen");
    CHECK(qwen->downloads > 0);
    CHECK(qwen->likes > 0);
    CHECK(qwen->license == "Apache-2.0");
    CHECK(qwen->licenseName == "Apache License 2.0");
    CHECK(qwen->pipelineTag == "text-generation");
    CHECK(qwen->baseRelation == "quantized");
    REQUIRE_FALSE(qwen->baseModels.empty());
    CHECK(qwen->baseModels.front() == "Qwen/Qwen3-8B");
    CHECK(qwen->paramsB == Catch::Approx(8.19).margin(0.01)); // gguf.total
    CHECK(qwen->ggufFileSize == 5027783488u);
    CHECK(qwen->createdAt.starts_with("20"));
    CHECK(qwen->files.empty()); // a search has no file list

    const lib::HubModel* finetune = byId(models, "AnkitAI/Parable-Qwen3-8B-Claude-Fable-5-GGUF");
    REQUIRE(finetune != nullptr);
    CHECK(finetune->baseRelation == "finetune");

    // Garbage and wrong shapes are errors or skipped entries, never exceptions.
    CHECK_FALSE(lib::parseHubSearch("{}").has_value());
    CHECK_FALSE(lib::parseHubSearch("not json").has_value());
    const auto skipped = lib::parseHubSearch(R"([{"id": "../evil"}, {"id": 5}, {"id": "a/b", "downloads": "many"}])");
    REQUIRE(skipped.has_value());
    REQUIRE(skipped->size() == 1);
    CHECK(skipped->front().downloads == 0);
}

TEST_CASE("Model library keeps only GGUF chat models runnable", "[ai][library]") {
    const auto models = searchFixture();
    auto runnable = [&](std::string_view id) {
        const lib::HubModel* m = byId(models, id);
        REQUIRE(m != nullptr);
        return lib::isRunnableLlm(*m);
    };
    CHECK(runnable("Qwen/Qwen3-8B-GGUF"));
    CHECK(runnable("Qwen/Qwen3-VL-8B-Instruct-GGUF"));
    CHECK(runnable("unsloth/DeepSeek-R1-0528-Qwen3-8B-GGUF"));
    CHECK(runnable("mradermacher/Qwen3-VL-8B-Instruct-abliterated-GGUF")); // no pipeline, but conversational
    CHECK_FALSE(runnable("Serveurperso/ACE-Step-1.5-GGUF"));               // music generation
    CHECK_FALSE(runnable("scragnog/ace-step-1.5-gguf-merge-models"));       // text-to-audio
}

TEST_CASE("Model library matches published benchmarks to quantizations only", "[ai][library][benchmarks]") {
    const lib::BenchmarkTable& table = lib::builtinBenchmarks();
    const auto models = searchFixture();
    auto family = [&](std::string_view id) -> std::string {
        const lib::HubModel* m = byId(models, id);
        REQUIRE(m != nullptr);
        const lib::BenchmarkFamily* f = lib::matchFamily(*m, table);
        return f ? f->id : std::string();
    };
    CHECK(family("Qwen/Qwen3-8B-GGUF") == "Qwen/Qwen3-8B");
    CHECK(family("unsloth/Qwen3-VL-8B-Instruct-GGUF") == "Qwen/Qwen3-VL-8B-Instruct");
    CHECK(family("AnkitAI/Parable-Qwen3-8B-Claude-Fable-5-GGUF").empty());       // a fine-tune of Qwen3 8B
    CHECK(family("mradermacher/Qwen3-VL-8B-Instruct-abliterated-GGUF").empty()); // a quantization of an altered model

    // By repository name alone (the curated picks): quantization repositories named after the family.
    CHECK(lib::familyForRepo("unsloth/Qwen3-4B-Instruct-2507-GGUF", table)->id == "Qwen/Qwen3-4B-Instruct-2507");
    CHECK(lib::familyForRepo("unsloth/Phi-4-mini-instruct-GGUF", table)->id == "microsoft/Phi-4-mini-instruct");
    CHECK(lib::familyForRepo("someone/Qwen3-8B-Q4_K_M-GGUF", table)->id == "Qwen/Qwen3-8B");
    CHECK(lib::familyForRepo("someone/Qwen3-8B-abliterated-GGUF", table) == nullptr);

    // The composite is the mean of the core metrics a family reports.
    const lib::BenchmarkFamily* qwen = table.family("qwen/qwen3-8b");
    REQUIRE(qwen != nullptr);
    double sum = 0.0;
    int n = 0;
    for (const lib::Score& s : qwen->scores) {
        if (table.metric(s.metric)->core) {
            sum += s.value;
            ++n;
        }
    }
    REQUIRE(n > 0);
    CHECK(*lib::compositeScore(*qwen, table) == Catch::Approx(sum / n));

    // A repository's own model-index results count when no family applies (fractions read as percent).
    const std::vector<lib::EvalResult> evals{{"MMLU-Pro", "TIGER-Lab/MMLU-Pro", 61.0}, {"IFEval", "IFEval", 80.0}, {"BLEU", "x", 30.0}};
    CHECK(*lib::compositeScore(evals, table) == Catch::Approx(70.5));
    auto withIndex = lib::parseHubModel(R"({"id": "me/model-GGUF", "tags": ["gguf"], "model-index": [{"name": "m",
        "results": [{"dataset": {"name": "MMLU-Pro"}, "metrics": [{"type": "acc", "name": "MMLU-Pro", "value": 0.5}]}]}]})");
    REQUIRE(withIndex.has_value());
    REQUIRE(withIndex->evals.size() == 1);
    CHECK(withIndex->evals.front().value == Catch::Approx(50.0));
    const lib::Entry e = lib::makeEntry(*withIndex, table);
    CHECK(e.ownEvals);
    CHECK(*e.score == Catch::Approx(50.0));
}

TEST_CASE("The bundled benchmark table parses and every score has a source", "[ai][library][benchmarks]") {
    const lib::BenchmarkTable& table = lib::builtinBenchmarks();
    REQUIRE(table.families.size() >= 20);
    REQUIRE_FALSE(table.updated.empty());
    for (const lib::BenchmarkFamily& f : table.families) {
        INFO(f.id);
        CHECK(f.source.starts_with("https://"));
        CHECK_FALSE(f.name.empty());
        for (const lib::Score& s : f.scores) {
            CHECK(table.metric(s.metric) != nullptr);
            CHECK(s.value >= 0.0);
            CHECK(s.value <= 100.0);
        }
    }
    // The curated picks (ModelStore's tiers and Phi-4 mini) all have published scores.
    for (const char* id : {"Qwen/Qwen3-1.7B", "Qwen/Qwen3-4B-Instruct-2507", "Qwen/Qwen3-8B", "microsoft/Phi-4-mini-instruct"}) {
        INFO(id);
        const lib::BenchmarkFamily* f = table.family(id);
        REQUIRE(f != nullptr);
        CHECK(lib::compositeScore(*f, table).has_value());
    }
    // Validation: unknown metrics, scores out of range and sourceless families are refused.
    const std::string head = R"({"schemaVersion": 1, "metrics": [{"id": "x", "core": true}], "families": [)";
    CHECK(lib::parseBenchmarks(head + R"({"id": "a/b", "source": "https://e", "scores": {"x": 50}}]})").has_value());
    CHECK_FALSE(lib::parseBenchmarks(head + R"({"id": "a/b", "source": "https://e", "scores": {"y": 50}}]})").has_value());
    CHECK_FALSE(lib::parseBenchmarks(head + R"({"id": "a/b", "source": "https://e", "scores": {"x": 150}}]})").has_value());
    CHECK_FALSE(lib::parseBenchmarks(head + R"({"id": "a/b", "scores": {"x": 50}}]})").has_value());
    CHECK_FALSE(lib::parseBenchmarks(head + R"({"id": "../b", "source": "https://e"}]})").has_value());
}

TEST_CASE("Model library parses quantization names and parameter counts", "[ai][library]") {
    CHECK(lib::quantOf("Qwen3-8B-Q4_K_M.gguf") == "Q4_K_M");
    CHECK(lib::quantOf("Qwen3.8-27B-UD-Q4_K_XL.gguf") == "UD-Q4_K_XL");
    CHECK(lib::quantOf("Qwen3.8-27B-UD-IQ2_XXS.gguf") == "UD-IQ2_XXS");
    CHECK(lib::quantOf("BF16/Qwen3.8-27B-BF16-00001-of-00002.gguf") == "BF16");
    CHECK(lib::quantOf("gemma-4-12b-it-q4_0.gguf") == "Q4_0");
    CHECK(lib::quantOf("gpt-oss-20b-MXFP4.gguf") == "MXFP4");
    CHECK(lib::quantOf("Phi-4-mini-instruct.gguf").empty());
    CHECK(lib::bitsOf("Q4_K_M") == Catch::Approx(4.8));
    CHECK(lib::bitsOf("UD-Q8_K_XL") == Catch::Approx(8.5));
    CHECK(lib::bitsOf("BF16") == Catch::Approx(16.0));
    CHECK(lib::bitsOf("weird") == 0.0);

    double active = 0.0;
    CHECK(lib::paramsFromName("Qwen/Qwen3-1.7B") == Catch::Approx(1.7));
    CHECK(lib::paramsFromName("unsloth/Qwen3.8-27B-GGUF") == Catch::Approx(27.0));
    CHECK(lib::paramsFromName("google/gemma-4-E4B-it") == Catch::Approx(4.0));
    CHECK(lib::paramsFromName("HuggingFaceTB/SmolLM2-360M-Instruct") == Catch::Approx(0.36));
    CHECK(lib::paramsFromName("mistralai/Mixtral-8x7B") == Catch::Approx(56.0));
    CHECK(lib::paramsFromName("ornith-ai/Ornith-1.5-35B-A3B-GGUF", &active) == Catch::Approx(35.0));
    CHECK(active == Catch::Approx(3.0));
    CHECK(lib::paramsFromName("microsoft/Phi-4-mini-instruct") == 0.0);
}

TEST_CASE("Model library reads a model's files and groups quantizations", "[ai][library]") {
    const lib::HubModel model = modelFixture();
    CHECK(model.revision == "7c41481f57cb95916b40956ab2f0b139b296d974"); // the revision ModelStore pins for qwen3-8b
    const auto variants = lib::variantsOf(model.files);
    REQUIRE(variants.size() == 5);
    CHECK(variants.front().quant == "Q4_K_M"); // smallest first
    CHECK(variants.back().quant == "Q8_0");
    const lib::Variant* chosen = lib::defaultVariant(variants);
    REQUIRE(chosen != nullptr);
    CHECK(chosen->quant == "Q4_K_M");
    CHECK(chosen->bytes == 5027783488u);
    CHECK(chosen->files.front().sha256 == "d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785");
    CHECK(lib::defaultVariant(variants, "Q6_K")->quant == "Q6_K");
    CHECK(lib::projectorsOf(model.files).empty());

    // A tree with split full-precision files, projectors, an importance matrix and speculative-decoding drafts.
    auto tree = lib::parseHubTree(fixture("tree-qwen3.8-27b-gguf.json"));
    REQUIRE(tree.has_value());
    const auto treeVariants = lib::variantsOf(*tree);
    const lib::Variant* bf16 = nullptr;
    for (const auto& v : treeVariants) {
        INFO(v.quant);
        CHECK(v.quant.find("MTP") == std::string::npos);
        CHECK(v.files.front().path.find("imatrix") == std::string::npos);
        CHECK(v.files.front().path.find("mmproj") == std::string::npos);
        if (v.quant == "BF16")
            bf16 = &v;
    }
    REQUIRE(bf16 != nullptr);
    REQUIRE(bf16->files.size() == 2);
    CHECK(bf16->files[0].path.find("00001-of-00002") != std::string::npos);
    CHECK(bf16->bytes == bf16->files[0].size + bf16->files[1].size);
    CHECK(lib::defaultVariant(treeVariants)->quant == "UD-Q4_K_XL"); // no plain Q4_K_M here
    const auto projectors = lib::projectorsOf(*tree);
    REQUIRE(projectors.size() == 2);
    CHECK(lib::defaultProjector(projectors)->path == "mmproj-F16.gguf");

    // An incomplete split set and files without a SHA-256 are never offered.
    const std::string sha(64, 'a');
    const std::vector<lib::HubFile> partial{{"x-Q4_K_M-00001-of-00003.gguf", 10, sha}, {"x-Q4_K_M-00002-of-00003.gguf", 10, sha},
                                            {"y-Q8_0.gguf", 10, ""}};
    CHECK(lib::variantsOf(partial).empty());
}

TEST_CASE("Model library builds search and file URLs", "[ai][library]") {
    lib::HubQuery q;
    q.search = "qwen 3&x";
    q.sort = lib::SortKey::Trending;
    q.limit = 500;
    const std::string url = lib::hubSearchUrl(q);
    CHECK(url.starts_with("https://huggingface.co/api/models?filter=gguf"));
    CHECK(url.find("search=qwen%203%26x") != std::string::npos);
    CHECK(url.find("sort=trendingScore") != std::string::npos);
    CHECK(url.find("limit=100") != std::string::npos);
    CHECK(url.find("expand%5B%5D=gguf") != std::string::npos);
    q.sort = lib::SortKey::Benchmark; // sorted here: fetched by downloads
    CHECK(lib::hubSearchUrl(q).find("sort=downloads") != std::string::npos);

    CHECK(lib::hubFileUrl("unsloth/Qwen3.8-27B-GGUF", "4ca720788d1e01f1bff70c033e0d0028fd02e502", "BF16/a b.gguf") ==
          "https://huggingface.co/unsloth/Qwen3.8-27B-GGUF/resolve/4ca720788d1e01f1bff70c033e0d0028fd02e502/BF16/a%20b.gguf");
    CHECK(lib::hubModelUrl("Qwen/Qwen3-8B-GGUF") == "https://huggingface.co/api/models/Qwen/Qwen3-8B-GGUF?blobs=true");

    const HttpHeaders next{{"Link", "<https://huggingface.co/api/models?filter=gguf&cursor=abc>; rel=\"next\""}};
    CHECK(lib::nextPageUrl(next) == std::optional<std::string>("https://huggingface.co/api/models?filter=gguf&cursor=abc"));
    const HttpHeaders elsewhere{{"Link", "<https://evil.example/api/models?x=1>; rel=\"next\""}};
    CHECK_FALSE(lib::nextPageUrl(elsewhere).has_value());
    CHECK_FALSE(lib::nextPageUrl({}).has_value());

    CHECK(lib::isValidRepoId("unsloth/Qwen3.8-27B-GGUF"));
    for (const char* bad : {"", "noslash", "a/b/c", "../x", "a/..", "a/b.", "a b/c", "a/b\\c", "-a/b"})
        CHECK_FALSE(lib::isValidRepoId(bad));
    CHECK(lib::isSafeRepoPath("BF16/model-00001-of-00002.gguf"));
    for (const char* bad : {"", "/abs.gguf", "a/../b.gguf", "a\\b.gguf", "c:x.gguf", "a/./b.gguf", "a/"})
        CHECK_FALSE(lib::isSafeRepoPath(bad));
}

TEST_CASE("Model library filters and sorts entries", "[ai][library]") {
    const lib::BenchmarkTable& table = lib::builtinBenchmarks();
    std::vector<lib::Entry> entries;
    for (lib::HubModel m : searchFixture()) {
        if (lib::isRunnableLlm(m))
            entries.push_back(lib::makeEntry(std::move(m), table));
    }
    REQUIRE(entries.size() == 6);
    auto find = [&](std::string_view id) -> lib::Entry& {
        for (auto& e : entries) {
            if (e.hub.id == id)
                return e;
        }
        FAIL("no entry " << id);
        return entries.front();
    };
    lib::Entry& official = find("Qwen/Qwen3-8B-GGUF");
    CHECK(official.publisher == lib::Publisher::Official);
    CHECK(official.score.has_value());
    CHECK(official.permissive);
    // A search lists no files: the size is a 4-bit estimate from the parameter count (8.19B -> about 4.9 GB), not the
    // Hub's one-file size (16 GB for some of these repositories, their 16-bit file).
    CHECK(official.sizeEstimated);
    CHECK(official.sizeBytes == static_cast<std::uint64_t>(official.hub.paramsB * 1e9 * 4.8 / 8.0));
    CHECK(find("unsloth/Qwen3-VL-8B-Instruct-GGUF").publisher == lib::Publisher::Quantizer);
    CHECK(find("unsloth/Qwen3-VL-8B-Instruct-GGUF").vision);
    CHECK(find("AnkitAI/Parable-Qwen3-8B-Claude-Fable-5-GGUF").publisher == lib::Publisher::Community);
    const auto& tasks = find("AnkitAI/Parable-Qwen3-8B-Claude-Fable-5-GGUF").tasks;
    CHECK(std::find(tasks.begin(), tasks.end(), lib::Task::Coding) != tasks.end());
    CHECK(std::find(tasks.begin(), tasks.end(), lib::Task::Reasoning) != tasks.end());

    auto count = [&](const lib::Filter& f) {
        return std::count_if(entries.begin(), entries.end(), [&](const lib::Entry& e) { return lib::matches(e, f); });
    };
    lib::Filter all;
    all.permissiveOnly = false;
    CHECK(count(all) == 6);
    lib::Filter vision = all;
    vision.task = lib::Task::Vision;
    CHECK(count(vision) == 2); // the abliterated quantization says nothing about images
    lib::Filter officialOnly = all;
    officialOnly.publisher = "official";
    CHECK(count(officialOnly) == 2); // Qwen's own two
    lib::Filter trusted = all;
    trusted.publisher = "trusted";
    CHECK(count(trusted) == 5); // and unsloth's, mradermacher's
    lib::Filter medium = all;
    medium.size = "medium"; // 3-8 GB: every 8B model at 4 bits
    CHECK(count(medium) == 6);
    lib::Filter large = all;
    large.size = "large";
    CHECK(count(large) == 0);
    lib::Filter params = all;
    params.params = "small"; // 3-9B: every one of them
    CHECK(count(params) == 6);
    lib::Filter installed = all;
    installed.installedOnly = true;
    CHECK(count(installed) == 0);
    official.installed = true;
    CHECK(count(installed) == 1);
    lib::Filter fits = all;
    fits.fitsOnly = true;
    find("unsloth/DeepSeek-R1-0528-Qwen3-8B-GGUF").fit = lib::Fit::No;
    CHECK(count(fits) == 5);

    lib::sortEntries(entries, lib::SortKey::Benchmark);
    CHECK(entries.front().score.has_value());
    CHECK_FALSE(entries.back().score.has_value()); // no published score last
    lib::sortEntries(entries, lib::SortKey::Smallest);
    for (std::size_t k = 1; k < entries.size(); ++k)
        CHECK(entries[k - 1].sizeBytes <= entries[k].sizeBytes);
    lib::sortEntries(entries, lib::SortKey::Downloads);
    for (std::size_t k = 1; k < entries.size(); ++k)
        CHECK(entries[k - 1].hub.downloads >= entries[k].hub.downloads);
    lib::sortEntries(entries, lib::SortKey::Newest);
    for (std::size_t k = 1; k < entries.size(); ++k)
        CHECK(entries[k - 1].hub.createdAt >= entries[k].hub.createdAt);
    // Recommended: what runs here first (GPU before CPU before unknown before no), then curated, then the score.
    find("Qwen/Qwen3-VL-8B-Instruct-GGUF").fit = lib::Fit::Cpu;
    find("Qwen/Qwen3-8B-GGUF").fit = lib::Fit::Gpu;
    lib::sortEntries(entries, lib::SortKey::Recommended);
    CHECK(entries[0].hub.id == "Qwen/Qwen3-8B-GGUF");
    CHECK(entries[1].hub.id == "Qwen/Qwen3-VL-8B-Instruct-GGUF");
    CHECK(entries.back().hub.id == "unsloth/DeepSeek-R1-0528-Qwen3-8B-GGUF");
}

TEST_CASE("Model library plans an install and round-trips the index", "[ai][library]") {
    const lib::BenchmarkTable& table = lib::builtinBenchmarks();
    const lib::HubModel model = modelFixture();
    const auto variants = lib::variantsOf(model.files);
    auto plan = lib::planInstall(model, *lib::defaultVariant(variants), nullptr, false, table);
    REQUIRE(plan.has_value());
    CHECK(plan->id == "hf:Qwen/Qwen3-8B-GGUF:Q4_K_M");
    CHECK(plan->name == "Qwen3 8B");
    CHECK(plan->family == "Qwen/Qwen3-8B");
    CHECK_FALSE(plan->licenseAccepted); // Apache-2.0 needs no acceptance
    REQUIRE(plan->files.size() == 1);
    CHECK(plan->files[0].url ==
          "https://huggingface.co/Qwen/Qwen3-8B-GGUF/resolve/7c41481f57cb95916b40956ab2f0b139b296d974/Qwen3-8B-Q4_K_M.gguf");
    CHECK(plan->files[0].storeName == "library/Qwen/Qwen3-8B-GGUF/Qwen3-8B-Q4_K_M.gguf");
    CHECK(lib::bytesOf(*plan) == 5027783488u);

    const auto infos = lib::modelInfosOf(*plan);
    REQUIRE(infos.size() == 1);
    CHECK(infos[0].kind == ModelKind::Llm);
    CHECK(infos[0].sha256 == plan->files[0].sha256);
    CHECK(infos[0].paramsB == Catch::Approx(8.19).margin(0.01));
    const ModelSpec spec = lib::registrySpecOf(*plan);
    CHECK(spec.provider == "local-llm");
    CHECK(spec.api == "managed-llama");
    CHECK(spec.caps.has(Capability::Chat));
    CHECK(spec.caps.constrainedTools);
    CHECK_FALSE(spec.caps.vision);

    // A split model with a projector: parts and the projector become ModelStore entries of their own.
    lib::InstalledModel split = *plan;
    split.id = lib::libraryModelId(split.repo, "BF16");
    split.quant = "BF16";
    split.files.push_back({"library/Qwen/Qwen3-8B-GGUF/x-00002-of-00002.gguf", plan->files[0].url, 5, std::string(64, 'b')});
    split.projector = lib::InstalledFile{"library/Qwen/Qwen3-8B-GGUF/mmproj-F16.gguf", plan->files[0].url, 7, std::string(64, 'c')};
    const auto splitInfos = lib::modelInfosOf(split);
    REQUIRE(splitInfos.size() == 3);
    CHECK(splitInfos[0].parts == std::vector<std::string>{split.id + "#2"});
    CHECK(splitInfos[0].projector == split.id + "#mmproj");
    CHECK(lib::registrySpecOf(split).caps.vision);

    // The index: saved, read back, entries with a path outside their folder or a bad digest skipped.
    const auto root = test::freshDir("library-index");
    lib::LibraryIndex index(root);
    REQUIRE(index.load().has_value()); // no file yet: an empty library
    index.upsert(*plan);
    index.upsert(split);
    REQUIRE(index.save().has_value());
    lib::LibraryIndex again(root);
    REQUIRE(again.load().has_value());
    REQUIRE(again.models().size() == 2);
    CHECK(again.find(plan->id)->files == plan->files);
    CHECK(again.find(split.id)->projector == split.projector);
    // The two entries share nothing but the projector is the split one's alone.
    const auto exclusive = again.exclusiveFiles(split.id);
    CHECK(std::find(exclusive.begin(), exclusive.end(), split.projector->storeName) != exclusive.end());

    Json tampered = lib::toJson(*plan);
    tampered["files"][0]["storeName"] = "library/Qwen/Qwen3-8B-GGUF/../../../evil.gguf";
    CHECK_FALSE(lib::installedFromJson(tampered).has_value());
    tampered = lib::toJson(*plan);
    tampered["files"][0]["sha256"] = "nope";
    CHECK_FALSE(lib::installedFromJson(tampered).has_value());
    tampered = lib::toJson(*plan);
    tampered["files"][0]["url"] = "https://evil.example/Qwen/Qwen3-8B-GGUF/resolve/x/y.gguf";
    CHECK_FALSE(lib::installedFromJson(tampered).has_value());
    {
        std::ofstream out(lib::LibraryIndex::fileFor(root), std::ios::binary | std::ios::trunc);
        out << "[]";
    }
    lib::LibraryIndex damaged(root);
    CHECK_FALSE(damaged.load().has_value());
}

TEST_CASE("Model library refuses unaccepted licences", "[ai][library]") {
    const lib::BenchmarkTable& table = lib::builtinBenchmarks();
    lib::HubModel model = modelFixture();
    model.license = "llama3.1";
    model.licenseName = lib::licenseDisplayName("llama3.1");
    CHECK(model.licenseName == "Llama 3.1 Community License");
    CHECK_FALSE(lib::isPermissive(model.license));
    const auto variants = lib::variantsOf(model.files);
    const auto refused = lib::planInstall(model, variants.front(), nullptr, false, table);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message.find("Llama 3.1 Community License") != std::string::npos);
    const auto accepted = lib::planInstall(model, variants.front(), nullptr, true, table);
    REQUIRE(accepted.has_value());
    CHECK(accepted->licenseAccepted);
    CHECK(lib::modelInfosOf(*accepted).front().licenseAccepted);

    // The store refuses a local model outside MIT / Apache-2.0 unless the user accepted its licence.
    const std::string body = randomBody(3000, 7);
    ModelInfo m = fileModel("licensed", body, "llama3.1");
    FakeDownloads net;
    net.bodies[m.url] = body;
    ModelStore store(test::freshDir("licence-store"));
    auto blocked = store.ensure(m, &net);
    REQUIRE_FALSE(blocked.has_value());
    CHECK(net.requests().empty());
    m.licenseAccepted = true;
    CHECK(store.ensure(m, &net).has_value());
}

TEST_CASE("Registered library models are found by findModel", "[ai][library]") {
    ModelInfo m = fileModel("hf:test/repo:Q4_K_M", "abc");
    CHECK(registerModel(m));
    const ModelInfo* found = findModel("hf:test/repo:Q4_K_M");
    REQUIRE(found != nullptr);
    CHECK(found->sha256 == m.sha256);
    const std::vector<ModelInfo> registered = registeredModels();
    CHECK(std::any_of(registered.begin(), registered.end(), [](const ModelInfo& r) { return r.id == "hf:test/repo:Q4_K_M"; }));
    unregisterModel("hf:test/repo:Q4_K_M");
    CHECK(findModel("hf:test/repo:Q4_K_M") == nullptr);
    CHECK(registerModel(m)); // registered again after a fresh download
    CHECK(findModel("hf:test/repo:Q4_K_M") != nullptr);

    ModelInfo builtin = m;
    builtin.id = "base";
    CHECK_FALSE(registerModel(builtin)); // built-in ids stay built-in
    ModelInfo escape = m;
    escape.id = "escape";
    escape.fileName = "../outside.gguf";
    CHECK_FALSE(registerModel(escape));
    escape.fileName = "C:/outside.gguf";
    CHECK_FALSE(registerModel(escape));
}

TEST_CASE("Download queue downloads verifies and reports progress", "[ai][library][downloads]") {
    const std::string a = randomBody(40'000, 1);
    const std::string b = randomBody(25'000, 2);
    ModelInfo fa = fileModel("hf:test/repo:A", a);
    ModelInfo fb = fileModel("hf:test/repo:A#2", b);
    FakeDownloads net;
    net.bodies[fa.url] = a;
    fb.url += "2";
    net.bodies[fb.url] = b;
    const auto root = test::freshDir("queue-basic");

    std::mutex mutex;
    std::vector<DownloadState> finished;
    std::atomic<int> changes{0};
    DownloadQueue::Options options;
    options.onChanged = [&] { ++changes; };
    options.notifyInterval = std::chrono::milliseconds(0);
    options.onFinished = [&](const DownloadState& s) {
        std::lock_guard lock(mutex);
        finished.push_back(s);
    };
    DownloadQueue queue(net, options);
    REQUIRE(queue.enqueue({"model-a", "Model A", root, {fa, fb}}));
    CHECK_FALSE(queue.enqueue({"model-a", "Model A", root, {fa, fb}})); // already queued
    REQUIRE(queue.waitIdle(std::chrono::seconds(10)));
    const auto state = queue.state("model-a");
    REQUIRE(state.has_value());
    CHECK(state->status == DownloadStatus::Done);
    CHECK(state->received == a.size() + b.size());
    CHECK(state->total == a.size() + b.size());
    CHECK(state->fileCount == 2);
    CHECK(changes > 2);
    {
        std::lock_guard lock(mutex);
        REQUIRE(finished.size() == 1);
        CHECK(finished.front().status == DownloadStatus::Done);
    }
    ModelStore store(root);
    CHECK(store.isInstalled(fa));
    CHECK(store.isInstalled(fb));

    // Already there: nothing is fetched again.
    const auto before = net.requests().size();
    REQUIRE(queue.enqueue({"model-a", "Model A", root, {fa, fb}}));
    REQUIRE(queue.waitIdle(std::chrono::seconds(10)));
    CHECK(net.requests().size() == before);
    queue.clearFinished();
    CHECK(queue.snapshot().empty());
}

TEST_CASE("Download queue resumes a paused download with a range request", "[ai][library][downloads]") {
    const std::string body = randomBody(60'000, 3);
    ModelInfo f = fileModel("hf:test/repo:R", body);
    FakeDownloads net;
    net.bodies[f.url] = body;
    net.holdAt = 20'000;
    const auto root = test::freshDir("queue-resume");
    DownloadQueue queue(net, {});
    REQUIRE(queue.enqueue({"r", "R", root, {f}}));
    REQUIRE(waitUntil([&] { return net.holding.load() > 0; }));
    REQUIRE(queue.pause("r"));
    ModelStore store(root);
    REQUIRE(waitUntil([&] { return net.holding.load() == 0 && !queue.busy() && store.partialBytes(f) == 20'000; }));
    CHECK(queue.state("r")->status == DownloadStatus::Paused);
    CHECK_FALSE(store.isInstalled(f));

    net.holdAt = 0;
    REQUIRE(queue.resume("r"));
    REQUIRE(queue.waitIdle(std::chrono::seconds(10)));
    CHECK(queue.state("r")->status == DownloadStatus::Done);
    CHECK(store.isInstalled(f));
    const auto requests = net.requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].second == 0);
    CHECK(requests[1].second == 20'000); // "Range: bytes=20000-"
}

TEST_CASE("Download queue cancels and deletes partial files", "[ai][library][downloads]") {
    const std::string body = randomBody(50'000, 4);
    ModelInfo f = fileModel("hf:test/repo:C", body);
    FakeDownloads net;
    net.bodies[f.url] = body;
    net.holdAt = 10'000;
    const auto root = test::freshDir("queue-cancel");
    DownloadQueue queue(net, {});
    REQUIRE(queue.enqueue({"c", "C", root, {f}}));
    REQUIRE(waitUntil([&] { return net.holding.load() > 0; }));
    REQUIRE(queue.cancel("c"));
    CHECK_FALSE(queue.state("c").has_value());
    ModelStore store(root);
    REQUIRE(waitUntil([&] { return net.holding.load() == 0 && store.partialBytes(f) == 0; }));
    CHECK_FALSE(store.isInstalled(f));

    // A paused request's partial file goes too.
    REQUIRE(queue.enqueue({"c", "C", root, {f}}));
    REQUIRE(waitUntil([&] { return net.holding.load() > 0; }));
    REQUIRE(queue.pause("c"));
    REQUIRE(waitUntil([&] { return net.holding.load() == 0 && !queue.busy() && store.partialBytes(f) == 10'000; }));
    REQUIRE(queue.cancel("c"));
    REQUIRE(waitUntil([&] { return store.partialBytes(f) == 0; }));
    CHECK_FALSE(queue.cancel("c"));
}

TEST_CASE("Download queue reports a SHA-256 mismatch and a failed write", "[ai][library][downloads]") {
    const std::string body = randomBody(30'000, 5);
    ModelInfo f = fileModel("hf:test/repo:S", body);
    FakeDownloads net;
    net.bodies[f.url] = body;
    net.corrupt = true;
    const auto root = test::freshDir("queue-sha");
    DownloadQueue queue(net, {});
    REQUIRE(queue.enqueue({"s", "S", root, {f}}));
    REQUIRE(queue.waitIdle(std::chrono::seconds(10)));
    auto state = queue.state("s");
    REQUIRE(state.has_value());
    CHECK(state->status == DownloadStatus::Failed);
    CHECK(state->error.find("verification") != std::string::npos);
    ModelStore store(root);
    CHECK_FALSE(store.isInstalled(f));
    CHECK(store.partialBytes(f) == 0); // a damaged download is deleted

    // Retry after the network healed.
    net.corrupt = false;
    REQUIRE(queue.resume("s"));
    REQUIRE(queue.waitIdle(std::chrono::seconds(10)));
    CHECK(queue.state("s")->status == DownloadStatus::Done);

    // A store that cannot be written (the partial file's place is taken by a folder): Failed, with why.
    ModelInfo g = fileModel("hf:test/repo:W", body);
    g.url += "w";
    net.bodies[g.url] = body;
    std::filesystem::path part = store.pathFor(g);
    part += ".part";
    std::filesystem::create_directories(part);
    std::ofstream(part / "keep") << "x"; // not empty: it cannot be removed either
    REQUIRE(queue.enqueue({"w", "W", root, {g}}));
    REQUIRE(queue.waitIdle(std::chrono::seconds(10)));
    state = queue.state("w");
    REQUIRE(state.has_value());
    CHECK(state->status == DownloadStatus::Failed);
    CHECK_FALSE(state->error.empty());
}

TEST_CASE("Download queue refuses to start without enough disk space", "[ai][library][downloads]") {
    const std::string body = randomBody(30'000, 6);
    ModelInfo f = fileModel("hf:test/repo:D", body);
    FakeDownloads net;
    net.bodies[f.url] = body;
    DownloadQueue::Options options;
    options.freeSpace = [](const std::filesystem::path&) { return std::optional<std::uint64_t>(1000); };
    options.spareBytes = 0;
    DownloadQueue queue(net, options);
    REQUIRE(queue.enqueue({"d", "D", test::freshDir("queue-disk"), {f}}));
    REQUIRE(queue.waitIdle(std::chrono::seconds(10)));
    const auto state = queue.state("d");
    REQUIRE(state.has_value());
    CHECK(state->status == DownloadStatus::Failed);
    CHECK(state->error.find("Not enough free space") != std::string::npos);
    CHECK(net.requests().empty());
}

TEST_CASE("Launch overrides and the parameter count shape the llama-server plan", "[ai][library][llm]") {
    ModelInfo m;
    m.id = "hf:test/plan:Q4_K_M";
    m.kind = ModelKind::Llm;
    m.size = 5 * GiB;
    m.license = "Apache-2.0";
    m.paramsB = 8.0;
    const llm::SystemProbe probe = desktop(24 * GiB, 11 * GiB);

    auto plan = llm::planLaunch(m, probe);
    REQUIRE(plan.has_value());
    CHECK(plan->gpu);
    CHECK(plan->context == 8192);
    CHECK(plan->gpuLayers == -1);

    llm::setLaunchOverrides(m.id, {4096, 20, 3});
    plan = llm::planLaunch(m, probe);
    REQUIRE(plan.has_value());
    CHECK(plan->context == 4096);
    CHECK(plan->gpuLayers == 20);
    CHECK(plan->threads == 3);

    llm::setLaunchOverrides(m.id, {0, 0, 0}); // CPU only
    plan = llm::planLaunch(m, probe);
    REQUIRE(plan.has_value());
    CHECK_FALSE(plan->gpu);
    CHECK(plan->runtimeId == "llama-server-win-cpu-x64");

    llm::setLaunchOverrides(m.id, {999999, 5000, 9999}); // clamped
    CHECK(llm::launchOverrides(m.id) == llm::LaunchOverrides{131072, 999, 256});
    llm::setLaunchOverrides(m.id, {});
    CHECK(llm::launchOverrides(m.id) == llm::LaunchOverrides{});

    // A licence outside the allow-list runs only when the user accepted it.
    m.license = "llama3.1";
    CHECK_FALSE(llm::planLaunch(m, probe).has_value());
    m.licenseAccepted = true;
    CHECK(llm::planLaunch(m, probe).has_value());

    // The shape grows with the parameter count; the estimate tells a model that does not fit.
    CHECK(llm::shapeFor(1.7).layers < llm::shapeFor(27.0).layers);
    CHECK(llm::kvCacheBytes(llm::shapeFor(27.0)) > llm::kvCacheBytes(llm::shapeFor(4.0)));
    CHECK(llm::shapeFor(0.0).layers == 32);
    const llm::Admission fits = llm::estimateFit(5 * GiB, 8.0, probe);
    CHECK(fits.fits);
    CHECK(fits.gpu);
    const llm::Admission huge = llm::estimateFit(60 * GiB, 70.0, probe);
    CHECK_FALSE(huge.fits);
    CHECK_FALSE(huge.reason.empty());
}
