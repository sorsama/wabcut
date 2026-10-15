// Smart search (AI-8) building blocks: vectors and their encoding, the picture resampling SigLIP 2 expects (checked
// against Pillow), the Gemma BPE tokenizer (a hand-made tokenizer.json; the real one when WABCUT_TEST_SIGLIP_DIR names
// the downloaded model), the wabcut-embed protocol, shot-aware keyframes and the transcript query (Thai included).

#include "SearchTestUtil.h"

#include <wabcut/ai/Sha256.h>
#include <wabcut/ai/search/EmbedProtocol.h>
#include <wabcut/ai/search/Keyframes.h>
#include <wabcut/ai/search/TextQuery.h>
#include <wabcut/ai/search/Tokenizer.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>

using namespace wabcut;
using namespace wabcut::ai::search;
using namespace wabcut::ai::search::test;

// --- Vectors -----------------------------------------------------------------------------------------------------------

TEST_CASE("Vectors are normalised and travel exactly as base64 float32", "[ai][search]") {
    Vector v{3.0f, 4.0f};
    normalize(v);
    CHECK(v[0] == Catch::Approx(0.6f));
    CHECK(v[1] == Catch::Approx(0.8f));
    Vector zero{0.0f, 0.0f};
    normalize(zero);
    CHECK(zero == Vector{0.0f, 0.0f});
    CHECK(dot(v, v) == Catch::Approx(1.0f));

    const Vector odd{0.1f, -1.0e-30f, 3.4e38f, -0.0f, 1.0f / 3.0f};
    const auto decoded = decodeVector(encodeVector(odd));
    REQUIRE(decoded);
    CHECK(*decoded == odd); // bit-exact
    CHECK(decodeVector(encodeVector(odd), 5));
    CHECK_FALSE(decodeVector(encodeVector(odd), 4));         // wrong dimension
    CHECK_FALSE(decodeVector("AAAA"));                       // 3 bytes: not a float
    CHECK_FALSE(decodeVector("not base64!"));
    CHECK_FALSE(decodeVector(encodeVector(Vector{NAN})));     // non-finite values are refused
    CHECK(toBase64("") == "");
    CHECK(toBase64("f") == "Zg==");
    CHECK(toBase64("fo") == "Zm8=");
    CHECK(toBase64("foo") == "Zm9v");
    CHECK(fromBase64("Zm9vYg==") == std::optional<std::string>("foob"));
    CHECK_FALSE(fromBase64("Zm9vY"));
}

// --- Resampling --------------------------------------------------------------------------------------------------------

namespace {

/// The 7 x 5 test picture of scripts/pil_ref.py: ((x*37 + y*11) % 256, (x*5 + y*53) % 256, (x*x + 3*y*y) % 256).
Image testPicture() {
    Image image;
    image.width = 7;
    image.height = 5;
    for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 7; ++x) {
            image.rgb.push_back(static_cast<std::uint8_t>((x * 37 + y * 11) % 256));
            image.rgb.push_back(static_cast<std::uint8_t>((x * 5 + y * 53) % 256));
            image.rgb.push_back(static_cast<std::uint8_t>((x * x + y * y * 3) % 256));
        }
    }
    return image;
}

} // namespace

TEST_CASE("Pictures are resized exactly like Pillow's bilinear filter", "[ai][search]") {
    // Reference bytes from Pillow 12.3 (Image.resize(size, Image.BILINEAR)), what SiglipImageProcessor uses.
    const Image picture = testPicture();
    const Image down = resizeBilinear(picture, 3, 4);
    CHECK(down.rgb == std::vector<std::uint8_t>{35,  17,  2,   114, 28,  11, 193, 39,  28, 48,  79,  8,  127, 90,  17, 206, 101, 34,
                                               60,  141, 22,  139, 152, 31, 218, 163, 48, 73,  203, 44, 152, 214, 53, 163, 225, 70});
    const Image flat = resizeBilinear(picture, 7, 2);
    CHECK(flat.rgb == std::vector<std::uint8_t>{11,  53,  5,   48,  58,  6,   85,  63,  9,   122, 68,  14,  159, 73,
                                               21,  196, 78,  30,  233, 83,  41,  33,  159, 29,  70,  164, 30,  107,
                                               169, 33,  144, 174, 38,  181, 179, 45,  218, 184, 54,  174, 189, 65});
    const Image up = resizeBilinear(picture, 10, 8);
    REQUIRE(up.rgb.size() == 240);
    CHECK(std::vector<std::uint8_t>(up.rgb.begin(), up.rgb.begin() + 12) == std::vector<std::uint8_t>{0, 0, 0, 20, 3, 1, 46, 6, 2, 72, 10, 4});
    CHECK(std::vector<std::uint8_t>(up.rgb.end() - 6, up.rgb.end()) == std::vector<std::uint8_t>{130, 239, 78, 10, 242, 84});

    // The keyframe path: a 384 x 216 frame squeezed to the model's 224 x 224.
    Image frame;
    frame.width = 384;
    frame.height = 216;
    for (int y = 0; y < 216; ++y) {
        for (int x = 0; x < 384; ++x) {
            frame.rgb.push_back(static_cast<std::uint8_t>((x * 3 + y) % 256));
            frame.rgb.push_back(static_cast<std::uint8_t>((x + y * 2) % 256));
            frame.rgb.push_back(static_cast<std::uint8_t>((x * y) % 256));
        }
    }
    const Image model = resizeBilinear(frame, 224, 224);
    REQUIRE(model.valid());
    CHECK(ai::Sha256::hex(std::string_view(reinterpret_cast<const char*>(model.rgb.data()), model.rgb.size())) ==
          "8f4eb747e5be7a3ffe20207a0ac142fa4c41f4da8ac35f491d532742d14858d8");

    CHECK_FALSE(resizeBilinear(Image{}, 4, 4).valid());
    CHECK_FALSE(resizeBilinear(picture, 0, 4).valid());
    CHECK(resizeBilinear(picture, 7, 5).rgb == picture.rgb);
}

// --- Tokenizer ---------------------------------------------------------------------------------------------------------

namespace {

/// A tiny SigLIP-2-shaped tokenizer.json: the " " -> "▁" normalizer, a BPE model with byte fallback.
std::string tinyTokenizer(const std::string& normalizer = R"({"type":"Replace","pattern":{"String":" "},"content":"▁"})",
                          const std::string& preTokenizer = R"({"type":"Split","pattern":{"String":" "},"behavior":"MergedWithPrevious","invert":false})",
                          const std::string& merges = R"([["a","b"],["ab","c"],["▁","d"],["o","g"],["▁d","og"],["▁d","o"]])",
                          bool mergesFirst = false) {
    const std::string vocab = R"("vocab":{"<pad>":0,"<eos>":1,"<bos>":2,"<unk>":3,"<0xC3>":5,"<0xA9>":6,"a":10,"b":11,"c":12,"▁":13,)"
                              R"("d":14,"o":15,"g":16,"ab":20,"abc":21,"▁d":22,"▁do":23,"▁dog":24,"og":25})";
    const std::string mergeList = "\"merges\":" + merges;
    return R"({"version":"1.0","added_tokens":[{"id":0,"content":"<pad>"}],"normalizer":)" + normalizer +
           R"(,"pre_tokenizer":)" + preTokenizer +
           R"(,"post_processor":null,"model":{"type":"BPE","dropout":null,"unk_token":"<unk>","fuse_unk":true,"byte_fallback":true,)" +
           (mergesFirst ? mergeList + "," + vocab : vocab + "," + mergeList) + "}}";
}

std::vector<std::int64_t> ids(std::initializer_list<std::int64_t> list) { return list; }

} // namespace

TEST_CASE("The BPE tokenizer merges lowest rank first, falls back to bytes and pads like SigLIP 2", "[ai][search][tokenizer]") {
    auto tokenizer = BpeTokenizer::fromJson(tinyTokenizer());
    REQUIRE(tokenizer);
    CHECK(tokenizer->vocabularySize() == 19);
    CHECK(tokenizer->mergeCount() == 6);
    CHECK(tokenizer->eosId() == 1);
    CHECK(tokenizer->padId() == 0);
    const TokenizerOptions noPad{true, 0, false};

    CHECK(tokenizer->encode("abc dog", noPad) == ids({21, 24, 1}));   // abc + ▁dog (via og: rank 3 before ▁do's 5)
    CHECK(tokenizer->encode("ABC Dog", noPad) == ids({21, 24, 1}));   // lowercased
    CHECK(tokenizer->encode("abab", noPad) == ids({20, 20, 1}));      // equal ranks: leftmost first
    CHECK(tokenizer->encode("é", noPad) == ids({5, 6, 1}));           // not in the vocabulary: its UTF-8 bytes
    CHECK(tokenizer->encode("xx", noPad) == ids({3, 1}));             // no byte token either: one fused <unk>
    CHECK(tokenizer->encode("", noPad) == ids({1}));
    CHECK(tokenizer->encode("abc dog", TokenizerOptions{true, 6, true}) == ids({21, 24, 1, 0, 0, 0}));
    CHECK(tokenizer->encode("abc dog abc", TokenizerOptions{true, 3, true}) == ids({21, 24, 1})); // cut, <eos> kept
    CHECK(tokenizer->encode("ABC", TokenizerOptions{false, 0, false}) == ids({3, 1}));            // case kept: unknown

    // The older "left right" merge form, and merges listed before the vocabulary, read the same.
    auto older = BpeTokenizer::fromJson(tinyTokenizer(R"({"type":"Replace","pattern":{"String":" "},"content":"▁"})",
                                                      "null", R"(["a b","ab c","▁ d","o g","▁d og","▁d o"])", true));
    REQUIRE(older);
    CHECK(older->encode("abc dog", noPad) == ids({21, 24, 1}));

    // Anything but SigLIP 2's pipeline is refused rather than tokenized differently.
    const auto lower = BpeTokenizer::fromJson(tinyTokenizer(R"({"type":"Lowercase"})"));
    REQUIRE_FALSE(lower);
    CHECK(lower.error().code == ErrorCode::Unsupported);
    const auto meta = BpeTokenizer::fromJson(tinyTokenizer(R"({"type":"Replace","pattern":{"String":" "},"content":"▁"})",
                                                           R"({"type":"Metaspace","replacement":"▁"})"));
    REQUIRE_FALSE(meta);
    CHECK(meta.error().code == ErrorCode::Unsupported);
    const auto wordPiece = BpeTokenizer::fromJson(R"({"normalizer":null,"model":{"type":"WordPiece","vocab":{}}})");
    REQUIRE_FALSE(wordPiece);
    CHECK(wordPiece.error().code == ErrorCode::ParseError);
    CHECK(BpeTokenizer::fromJson("{not json").error().code == ErrorCode::ParseError);
    CHECK(BpeTokenizer::load("no/such/tokenizer.json").error().code == ErrorCode::NotFound);
}

TEST_CASE("lowercaseUtf8 folds the common cased scripts and leaves Thai alone", "[ai][search][tokenizer]") {
    CHECK(lowercaseUtf8("Hello ÉCOLE Ÿ ŁÓDŹ ΑΒΓ ДОМ") == "hello école ÿ łódź αβγ дом");
    CHECK(lowercaseUtf8("แมววิ่ง") == "แมววิ่ง");
    CHECK(lowercaseUtf8(std::string("A\xFF" "B", 3)) == std::string("a\xFF" "b", 3)); // invalid bytes kept
}

TEST_CASE("The real SigLIP 2 tokenizer gives the reference ids (WABCUT_TEST_SIGLIP_DIR)", "[ai][search][tokenizer][model]") {
    const char* dir = std::getenv("WABCUT_TEST_SIGLIP_DIR");
    if (dir == nullptr || *dir == '\0') {
        SKIP("set WABCUT_TEST_SIGLIP_DIR to the folder with SigLIP 2's tokenizer.json");
    }
    auto tokenizer = BpeTokenizer::load(std::filesystem::path(dir) / "tokenizer.json");
    REQUIRE(tokenizer);
    CHECK(tokenizer->vocabularySize() == 256000);
    // Reference ids from the tokenizers library on the same file (text lowercased first, as SigLIP 2 expects).
    const auto first = [&](std::string_view text, std::size_t n) {
        auto all = tokenizer->encode(text);
        REQUIRE(all.size() == 64);
        return std::vector<std::int64_t>(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(n));
    };
    CHECK(first("a photo of a cat", 7) == ids({235250, 2686, 576, 476, 4401, 1, 0}));
    CHECK(first("a dog running on the beach", 8) == ids({235250, 5929, 5327, 611, 573, 8318, 1, 0}));
    CHECK(first("แมววิ่งบนชายหาด", 10) == ids({157273, 235702, 38095, 16450, 104944, 118005, 235829, 33463, 1, 0}));
    CHECK(first("Hello World!", 5) == ids({17534, 2134, 235341, 1, 0}));
    CHECK(first("  two  spaces ", 7) == ids({139, 9721, 139, 46633, 235248, 1, 0}));
    CHECK(first("naïve café 🎬", 8) == ids({556, 236370, 524, 18688, 235248, 244290, 1, 0}));
    CHECK(first(std::string(200, 'x'), 15) ==
          ids({223505, 223505, 223505, 223505, 223505, 223505, 223505, 223505, 223505, 223505, 223505, 223505, 83234, 1, 0}));
}

// --- Protocol ----------------------------------------------------------------------------------------------------------

TEST_CASE("The wabcut-embed protocol round-trips and refuses what is out of bounds", "[ai][search][protocol]") {
    using namespace embed;
    const Load load{"1", "C:/models/vision.onnx", "C:/models/text.onnx", "C:/models/tokenizer.json", "cpu", 4};
    auto decoded = decodeRequest(encode(Request{load}));
    REQUIRE(decoded);
    const auto& back = std::get<Load>(*decoded);
    CHECK(back.vision == load.vision);
    CHECK(back.tokenizer == load.tokenizer);
    CHECK(back.device == "cpu");
    CHECK(back.threads == 4);

    const EmbedImages images{"2", ImageBatch{"C:/temp/batch.rgb", 224, 224, 16}};
    auto imagesBack = decodeRequest(encode(Request{images}));
    REQUIRE(imagesBack);
    CHECK(std::get<EmbedImages>(*imagesBack).images.count == 16);
    auto textBack = decodeRequest(encode(Request{EmbedText{"3", {"แมว", "a dog"}}}));
    REQUIRE(textBack);
    CHECK(std::get<EmbedText>(*textBack).texts == std::vector<std::string>{"แมว", "a dog"});
    CHECK(std::holds_alternative<Shutdown>(*decodeRequest(encode(Request{Shutdown{}}))));

    const Embeddings embeddings{"2", {Vector{1.0f, 0.5f}, Vector{-1.0f, 0.25f}}, 12.5};
    auto reply = decodeResponse(encode(Response{embeddings}));
    REQUIRE(reply);
    CHECK(std::get<Embeddings>(*reply).vectors == embeddings.vectors);
    CHECK(std::get<Embeddings>(*reply).milliseconds == 12.5);
    auto hello = decodeResponse(encode(Response{HelloReply{1, "onnxruntime", "1.24.4", {"DmlExecutionProvider", "CPUExecutionProvider"}}}));
    REQUIRE(hello);
    CHECK(std::get<HelloReply>(*hello).providers.size() == 2);
    auto failure = decodeResponse(encode(Response{Failure{"2", "inference", "out of memory"}}));
    REQUIRE(failure);
    CHECK(std::get<Failure>(*failure).message == "out of memory");

    for (const char* bad : {R"({"type":"embed_images","id":"2","images":{"path":"x","width":224,"height":224,"count":0}})",
                            R"({"type":"embed_images","id":"2","images":{"path":"x","width":5000,"height":224,"count":1}})",
                            R"({"type":"embed_images","id":"2","images":{"path":"x","width":224,"height":224,"count":257}})",
                            R"({"type":"embed_text","id":"3","texts":[]})", R"({"type":"embed_text","id":"3","texts":[7]})",
                            R"({"type":"embed_text","texts":["no id"]})", R"({"type":"load","id":"1","device":"tpu","vision":"v"})",
                            R"({"type":"load","id":"1","text":"t.onnx"})", R"({"type":"load","id":"1"})", R"({"type":"warp","id":"1"})",
                            R"({"id":"1"})", R"([1,2,3])", "not json"}) {
        INFO(bad);
        CHECK_FALSE(decodeRequest(bad));
    }
    const std::string longText(kMaxTextBytes + 1, 'a');
    CHECK_FALSE(decodeRequest(R"({"type":"embed_text","id":"3","texts":[")" + longText + "\"]}"));
    // A batch whose vectors disagree on the dimension, or are not vectors, is a protocol error.
    CHECK_FALSE(decodeResponse(R"({"type":"embeddings","id":"2","vectors":[")" + encodeVector(Vector{1, 2}) + R"(",")" +
                               encodeVector(Vector{1, 2, 3}) + "\"]}"));
    CHECK_FALSE(decodeResponse(R"({"type":"embeddings","id":"2","vectors":[null]})"));
    CHECK_FALSE(decodeResponse(R"({"type":"loaded","id":"1","dimension":-1})"));
    CHECK(decodeResponse(R"({"type":"loaded","id":"1","dimension":0})")); // detectors only (AI-9)
}

TEST_CASE("Picture batches are written and read back, refusing mixed sizes and short files", "[ai][search][protocol]") {
    TempDir dir("batch");
    const std::vector<Image> pictures{solid(4, 3, 255, 0, 0), solid(4, 3, 0, 0, 255)};
    auto batch = embed::writeImageBatch(dir.path / "b.rgb", pictures);
    REQUIRE(batch);
    CHECK(batch->count == 2);
    auto back = embed::readImageBatch(*batch);
    REQUIRE(back);
    CHECK((*back)[1].rgb == pictures[1].rgb);
    const std::vector<Image> mixed{solid(4, 3, 0, 0, 0), solid(5, 3, 0, 0, 0)};
    CHECK(embed::writeImageBatch(dir.path / "m.rgb", mixed).error().code == ErrorCode::InvalidArgument);
    embed::ImageBatch longer = *batch;
    longer.count = 3;
    CHECK(embed::readImageBatch(longer).error().code == ErrorCode::IoError);
}

// --- Keyframes ---------------------------------------------------------------------------------------------------------

TEST_CASE("Keyframes: one per shot, more in long shots, within a budget", "[ai][search]") {
    const auto plan = planKeyframes(shotsAt(20, {4, 6}), KeyframeOptions{seconds(5), 1000});
    REQUIRE(plan.size() == 5); // [0,4) [4,6) and [6,20) in three stretches
    CHECK(plan[0].time == seconds(2));
    CHECK(plan[0].range == TimeRange::fromStartEnd(seconds(0), seconds(4)));
    CHECK(plan[1].time == seconds(5));
    CHECK(plan[2].shot == TimeRange::fromStartEnd(seconds(6), seconds(20)));
    CHECK(plan[2].range.start == seconds(6));
    CHECK(plan[4].range.end() == seconds(20));
    for (const ai::search::Keyframe& k : plan) {
        CHECK(k.range.contains(k.time));
        CHECK(k.range.duration <= seconds(5));
    }
    for (std::size_t i = 1; i < plan.size(); ++i) {
        CHECK(plan[i].range.start == plan[i - 1].range.end()); // they tile the video
    }
    // A 100 s take with a budget of 4: the interval grows, the take is still covered.
    const auto budget = planKeyframes(shotsAt(100, {}), KeyframeOptions{seconds(5), 4});
    CHECK(budget.size() <= 4);
    CHECK(budget.back().range.end() == seconds(100));
    CHECK(planKeyframes(ai::analysis::ShotAnalysis{}).empty());
    // Cuts outside the stretch or out of order are ignored.
    const auto odd = planKeyframes(shotsAt(10, {12, 3, 2}), KeyframeOptions{seconds(10), 100});
    CHECK(odd.size() == 2);
}

// --- Transcript queries ------------------------------------------------------------------------------------------------

TEST_CASE("Transcript queries: trigram terms, quoted phrases, a LIKE fallback for short terms", "[ai][search][fts]") {
    const TextQuery q = buildTextQuery("Red car, at night!");
    CHECK(q.terms == std::vector<std::string>{"red", "car", "at", "night"});
    CHECK(q.match == R"("red" OR "car" OR "night")"); // "at" cannot use the trigram index and is not needed
    CHECK(q.likeTerms.empty());

    CHECK(buildTextQuery(R"("red car" night)").match == R"("red car" OR "night")");
    CHECK(buildTextQuery("RED red Red").terms == std::vector<std::string>{"red"});
    CHECK(buildTextQuery("say \"hi\"").terms == std::vector<std::string>{"say", "hi"});
    CHECK(buildTextQuery("100%_off").match == R"("100%_off")");
    CHECK(buildTextQuery("แมววิ่ง").match == "\"แมววิ่ง\"");                         // Thai: one term, a substring match
    CHECK(buildTextQuery("แมว\xC2\xA0วิ่ง").terms == std::vector<std::string>{"แมว", "วิ่ง"}); // no-break space splits

    const TextQuery shortThai = buildTextQuery("รถ");
    CHECK(shortThai.match.empty());
    CHECK(shortThai.likeTerms == std::vector<std::string>{"%รถ%"});
    CHECK(buildTextQuery("a b").likeTerms == std::vector<std::string>{"%a%", "%b%"});
    CHECK(buildTextQuery("  ,.;  ").empty());

    std::string many;
    for (int i = 0; i < 20; ++i) {
        many += "term" + std::to_string(i) + " ";
    }
    CHECK(buildTextQuery(many).terms.size() == kMaxQueryTerms);
    CHECK(codePoints(buildTextQuery(std::string(100, 'z')).terms.front()) == kMaxTermCodePoints);

    CHECK(escapeLike("100%_a\\b") == "100\\%\\_a\\\\b");
    CHECK(codePoints("แมว") == 3);
    CHECK(writtenWithoutSpaces("th"));
    CHECK(writtenWithoutSpaces("zh-Hant"));
    CHECK_FALSE(writtenWithoutSpaces("en"));
}

TEST_CASE("Matched words: whole words, phrases and Thai runs across words", "[ai][search][fts]") {
    const std::vector<std::string> english{"The", "red", "car", "carried", "us"};
    CHECK(matchedWords(english, buildTextQuery("car RED")) == std::vector<std::size_t>{1, 2, 3});
    CHECK(matchedWords(english, buildTextQuery(R"("red car")")) == std::vector<std::size_t>{1, 2});
    CHECK(matchedWords(english, buildTextQuery("ca")) == std::vector<std::size_t>{2, 3}); // a short term alone: substrings
    CHECK(matchedWords(english, buildTextQuery("bicycle")).empty());
    const std::vector<std::string> thai{"วันนี้", "แมว", "วิ่ง", "บน", "ชายหาด"};
    CHECK(matchedWords(thai, buildTextQuery("แมววิ่ง")) == std::vector<std::size_t>{1, 2});
    CHECK(matchedWords(thai, buildTextQuery("ชายหาด")) == std::vector<std::size_t>{4});
}
