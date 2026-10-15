// AI-12 local translation: OPUS-MT's unigram tokenizer (a small tokenizer.json written here, and the real pinned one
// when the model is installed), the language pairs and their pinned files, and LocalTranslator against a scripted
// wabcut-embed helper.

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/search/EmbedProtocol.h>
#include <wabcut/ai/translate/LocalTranslation.h>
#include <wabcut/ai/translate/MarianTokenizer.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::translate;

namespace {

// "▁" is U+2581. Pieces of the target vocabulary only have -100, as in the OPUS-MT exports.
const char* kTokenizer = R"({
  "version": "1.0",
  "added_tokens": [{"id": 0, "special": true, "content": "</s>"}, {"id": 1, "special": true, "content": "<unk>"},
                   {"id": 15, "special": true, "content": "<pad>"}],
  "normalizer": {"type": "Precompiled", "precompiled_charsmap": null},
  "decoder": {"type": "Metaspace", "replacement": "▁", "add_prefix_space": true},
  "model": {"type": "Unigram", "unk_id": 2, "vocab": [
    ["</s>", 0.0], ["<unk>", 0.0], ["▁hello", -1.0], ["▁hel", -2.0], ["lo", -2.0], ["▁world", -1.5],
    ["!", -2.0], [">>tha<<", -100.0], ["▁สวัสดี", -1.0], ["ครับ", -1.5],
    ["▁", -3.0], ["▁helloworld", -100.0], ["wor", -2.0], ["ld", -2.0], ["▁ทํา", -1.0], ["<pad>", 0.0]]}
})";

std::string pieces(const MarianTokenizer& t, const std::vector<std::int64_t>& ids) {
    std::string out;
    for (const std::int64_t id : ids) {
        out += (out.empty() ? "" : "|") + t.piece(id);
    }
    return out;
}

} // namespace

TEST_CASE("The OPUS-MT tokenizer splits words into the best pieces and reads them back", "[ai][global][translate][tokenizer]") {
    auto tokenizer = MarianTokenizer::fromJson(kTokenizer);
    REQUIRE(tokenizer);
    const MarianTokenizer& t = *tokenizer;
    CHECK(t.eos() == 0);
    CHECK(t.unk() == 1); // <unk> itself, whatever the file's unk_id says
    CHECK(t.pad() == 15);

    const auto ids = t.encode("hello  world!");
    CHECK(pieces(t, ids) == "▁hello|▁world|!|</s>");
    CHECK(t.decode(ids) == "hello world!");

    // A multilingual model's target token goes first; unknown characters become one <unk>.
    const auto thai = t.encode("สวัสดีครับ éé", ">>tha<<");
    CHECK(pieces(t, thai) == ">>tha<<|▁สวัสดี|ครับ|▁|<unk>|</s>");
    CHECK(t.decode(thai) == "สวัสดีครับ");

    // Target-only pieces lose against source pieces; special and target tokens never come out of decode.
    CHECK(pieces(t, t.encode("helloworld")) == "▁hello|wor|ld|</s>");
    const std::vector<std::int64_t> generated{7, 2, 5, 15, 0};
    CHECK(t.decode(generated) == "hello world");

    // The vocabulary is NFKC's: Thai SARA AM as NIKHAHIT + AA, full-width letters as ASCII; read back composed.
    const auto am = t.encode("ทำ");
    CHECK(am == t.encode("ทํา"));
    CHECK(pieces(t, am) == "▁ทํา|</s>");
    CHECK(t.decode(am) == "ทำ");
    CHECK(t.encode("ｈｅｌｌｏ") == t.encode("hello"));

    CHECK_FALSE(MarianTokenizer::fromJson(R"({"model":{"type":"BPE","vocab":{}}})"));
    CHECK_FALSE(MarianTokenizer::fromJson("not json"));
}

TEST_CASE("The local pairs name pinned, allow-listed files: English and Thai both ways first",
          "[ai][global][translate][models]") {
    const LocalPair* enTh = findLocalPair("en", "th-TH");
    REQUIRE(enTh != nullptr);
    CHECK(enTh->model == "opus-mt-en-mul");
    CHECK(enTh->targetToken == ">>tha<<");
    const LocalPair* thEn = findLocalPair("th", "en");
    REQUIRE(thEn != nullptr);
    CHECK(thEn->model == "opus-mt-th-en");
    CHECK(thEn->targetToken.empty());
    CHECK(findLocalPair("th", "ja") == nullptr);
    for (const LocalPair& pair : localPairs()) {
        INFO(pair.source << " -> " << pair.target);
        const auto files = localModelFiles(pair.model);
        REQUIRE(files.size() == 4);
        for (const ModelInfo* m : files) {
            CHECK(m->kind == ModelKind::Translation);
            CHECK(isAllowedModelLicense(m->license));
            CHECK(m->license.find("NC") == std::string::npos);
            CHECK(m->sha256.size() == 64);
            CHECK(m->url.starts_with("https://huggingface.co/Xenova/opus-mt-"));
            CHECK(m->url.find("/resolve/") != std::string::npos);
            CHECK(m->fileName.starts_with(std::string(pair.model) + "/"));
            CHECK(m->licenseCard.find("OPUS-MT") != std::string::npos);
            if (m->license == "CC-BY-4.0") {
                CHECK(m->licenseCard.find("Creative Commons Attribution 4.0") != std::string::npos); // the attribution
            }
        }
    }
    const ModelStore empty(std::filesystem::temp_directory_path() / "wabcut-no-models-here");
    CHECK_FALSE(isLocalModelInstalled(empty, "opus-mt-en-mul"));
    CHECK(localModelDownloadBytes(empty, "opus-mt-en-mul") > 100'000'000);
}

#ifdef WABCUT_FAKE_EMBED
TEST_CASE("LocalTranslator loads the model once per helper and translates in batches", "[ai][global][translate][helper]") {
    LocalTranslatorOptions options;
    options.executable = WABCUT_FAKE_EMBED;
    options.modelDirectory = std::filesystem::temp_directory_path() / "opus-mt-en-mul";
    options.targetToken = ">>tha<<";
    options.batch = 2;
    LocalTranslator translator(options);
    TranslationRequest request;
    request.sourceLanguage = "en";
    request.targetLanguage = "th";
    request.segments = {{"0", "Hello"}, {"1", "world"}, {"2", "again"}};
    auto result = translator.translate(request, {});
    REQUIRE(result);
    REQUIRE(result->segments.size() == 3);
    CHECK(result->segments[0].id == "0");
    CHECK(result->segments[0].text == ">>tha<< Hello"); // the fake helper echoes the target token
    CHECK(result->segments[2].text == ">>tha<< again");
    CHECK(translator.device() == "FakeCPU");
    CHECK(translator.tokens() > 0);

    // A GPU provider that takes the helper down while loading: the translator starts it again on the CPU.
    LocalTranslatorOptions gpu = options;
    gpu.device = "auto";
    gpu.arguments = {"--mode", "crash-gpu-translation"};
    LocalTranslator fallback(gpu);
    auto again = fallback.translate(request, {});
    INFO((again ? std::string() : again.error().message));
    REQUIRE(again);
    CHECK(again->segments.size() == 3);
    CHECK(fallback.device() == "FakeCPU");
}
#endif

TEST_CASE("The real OPUS-MT tokenizer round-trips English (when the model is installed)", "[ai][global][translate][tokenizer][real]") {
    const ModelStore store(ModelStore::defaultRoot());
    if (!isLocalModelInstalled(store, "opus-mt-en-mul")) {
        SKIP("opus-mt-en-mul is not installed");
    }
    auto t = MarianTokenizer::load(localModelDirectory(store, "opus-mt-en-mul") / "tokenizer.json");
    REQUIRE(t);
    const std::string text = "Welcome back to the channel, today we cook green curry.";
    const auto ids = t->encode(text, ">>tha<<");
    CHECK(ids.front() == *t->idOf(">>tha<<"));
    CHECK(ids.back() == t->eos());
    CHECK(t->decode(ids) == text);
}
