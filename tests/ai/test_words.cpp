// Language-aware word building from byte-level tokens, UTF-8 helpers, and the time mapping.

#include <wabcut/ai/Text.h>
#include <wabcut/ai/Timestamps.h>
#include <wabcut/ai/WordBuilder.h>

#include <catch2/catch_test_macros.hpp>

#include <random>

using namespace wabcut;
using namespace wabcut::ai;

namespace {

RecognizedSegment segment(std::vector<RecognizedToken> tokens) {
    RecognizedSegment s;
    s.t0 = tokens.empty() ? 0 : tokens.front().t0;
    s.t1 = tokens.empty() ? 0 : tokens.back().t1;
    s.tokens = std::move(tokens);
    return s;
}

std::vector<std::string> texts(const std::vector<BuiltWord>& words) {
    std::vector<std::string> out;
    for (const BuiltWord& w : words) {
        out.push_back(w.text);
    }
    return out;
}

} // namespace

TEST_CASE("UTF-8 helpers", "[ai][words]") {
    CHECK(text::isValidUtf8("plain ascii"));
    CHECK(text::isValidUtf8("สวัสดี 你好 😀"));
    CHECK_FALSE(text::isValidUtf8("\xC0\xAF"));         // overlong
    CHECK_FALSE(text::isValidUtf8("\xED\xA0\x80"));     // surrogate
    CHECK_FALSE(text::isValidUtf8("\xF4\x90\x80\x80")); // above U+10FFFF
    CHECK_FALSE(text::isValidUtf8("\xE0\xB8"));         // cut off
    CHECK(text::completeUtf8Prefix("ab\xE0\xB8") == 2);
    CHECK(text::utf8SequenceAt("\xE0\xB8", 0) == -1);
    CHECK(text::utf8SequenceAt("\xFF", 0) == 0);
    CHECK(text::sanitizeUtf8("a\xFF" "b") == "a\xEF\xBF\xBD" "b");
    CHECK(text::trimAsciiSpace("  hi \n") == "hi");
    CHECK(text::isTrailingPunctuation(","));
    CHECK(text::isTrailingPunctuation("。"));
    CHECK(text::isTrailingPunctuation("?!"));
    CHECK_FALSE(text::isTrailingPunctuation("a,"));
    CHECK(text::startsWithCombiningMark("ั")); // Thai MAI HAN-AKAT
    CHECK(text::startsWithCombiningMark("่า"));
    CHECK_FALSE(text::startsWithCombiningMark("ส"));
    CHECK_FALSE(text::startsWithCombiningMark("เ")); // a leading vowel starts a syllable
}

TEST_CASE("Space-delimited languages split words at leading spaces and keep punctuation on the word",
          "[ai][words]") {
    const auto words = buildWords(segment({{" Hello", 0, 30, 0.9f},
                                           {",", 30, 32, 0.5f},
                                           {" wor", 40, 60, 0.8f},
                                           {"ld", 60, 80, 0.6f},
                                           {".", 80, 81, 1.0f},
                                           {" It's", 100, 120, 0.7f},
                                           {" 3", 130, 140, 0.9f},
                                           {"%", 140, 141, 0.9f}}),
                                  WordSplit::Whitespace);
    REQUIRE(texts(words) == std::vector<std::string>{"Hello,", "world.", "It's", "3%"});
    CHECK(words[0].t0 == 0);
    CHECK(words[0].t1 == 32);
    CHECK(words[1].t0 == 40);
    CHECK(words[1].t1 == 81);
    CHECK(words[1].confidence == (0.8f + 0.6f + 1.0f) / 3.0f);
    CHECK(wordSplitFor("en") == WordSplit::Whitespace);
    CHECK(wordSplitFor("auto") == WordSplit::Whitespace);
}

TEST_CASE("Thai, Chinese and Japanese are never split on whitespace: tokens stay words", "[ai][words]") {
    CHECK(wordSplitFor("th") == WordSplit::PerUnit);
    CHECK(wordSplitFor("zh") == WordSplit::PerUnit);
    CHECK(wordSplitFor("ja") == WordSplit::PerUnit);
    // "สวัสดี ครับ" as whisper tokenises it: pieces, a space that is not a word boundary marker in Thai.
    const auto words = buildWords(segment({{"สวั", 0, 20, 0.9f}, {"สดี", 20, 40, 0.8f}, {" ครับ", 50, 70, 0.7f}}),
                                  WordSplit::PerUnit);
    CHECK(texts(words) == std::vector<std::string>{"สวั", "สดี", "ครับ"}); // TODO(ai): dictionary segmentation
    const auto zh = buildWords(segment({{"你好", 0, 20, 1.0f}, {"世界", 20, 40, 1.0f}, {"。", 40, 41, 1.0f}}),
                               WordSplit::PerUnit);
    CHECK(texts(zh) == std::vector<std::string>{"你好", "世界。"});
}

TEST_CASE("A character split across tokens is reassembled with the span of both tokens", "[ai][words]") {
    // "ดี" = E0 B8 94 E0 B8 B5; whisper's byte-level BPE may cut inside a character.
    const auto words = buildWords(segment({{"\xE0\xB8", 0, 10, 0.4f}, {"\x94\xE0\xB8\xB5", 10, 30, 0.8f}}),
                                  WordSplit::PerUnit);
    REQUIRE(words.size() == 1);
    CHECK(words[0].text == "ดี");
    CHECK(words[0].t0 == 0);
    CHECK(words[0].t1 == 30);
    CHECK(words[0].confidence == (0.4f + 0.8f) / 2.0f);
    // A complete character followed by a cut-off one: the complete part is a unit, the rest waits.
    const auto more = buildWords(segment({{"ก\xE0", 0, 10, 1.0f}, {"\xB8\x82", 10, 20, 1.0f}}), WordSplit::PerUnit);
    CHECK(texts(more) == std::vector<std::string>{"ก", "ข"});
}

TEST_CASE("A token starting with a combining mark joins the word before it", "[ai][words]") {
    // "สัตว์" tokenised so that a vowel sign and a silencer arrive alone: neither may become a word.
    const auto words = buildWords(
        segment({{"ส", 0, 10, 1.0f}, {"ั", 10, 20, 1.0f}, {"ต", 20, 30, 1.0f}, {"ว", 30, 40, 1.0f}, {"์", 40, 50, 1.0f}}),
        WordSplit::PerUnit);
    CHECK(texts(words) == std::vector<std::string>{"สั", "ต", "ว์"});
    CHECK(words[0].t1 == 20);
    CHECK(words[2].t1 == 50);
}

TEST_CASE("Latin words inside Thai text join like in English", "[ai][words]") {
    const auto words = buildWords(segment({{"ใช้", 0, 10, 1.0f}, {" i", 10, 20, 1.0f}, {"Phone", 20, 30, 1.0f}, {"ถ่าย", 30, 40, 1.0f}}),
                                  WordSplit::PerUnit);
    CHECK(texts(words) == std::vector<std::string>{"ใช้", "iPhone", "ถ่าย"});
}

TEST_CASE("Word building never crashes, never emits invalid UTF-8 and keeps times ordered", "[ai][words]") {
    std::mt19937 rng(1234);
    std::uniform_int_distribution<int> byte(0, 255);
    std::uniform_int_distribution<int> length(0, 6);
    std::uniform_int_distribution<int> time(0, 500);
    for (int round = 0; round < 2000; ++round) {
        std::vector<RecognizedToken> tokens;
        const int n = 1 + round % 12;
        for (int i = 0; i < n; ++i) {
            std::string bytes;
            for (int k = length(rng); k > 0; --k) {
                bytes += static_cast<char>(byte(rng));
            }
            const int a = time(rng);
            tokens.push_back({bytes, a, a + time(rng) / 10, 0.5f}); // unordered times on purpose
        }
        RecognizedSegment s{100, 300, tokens, false};
        for (const WordSplit split : {WordSplit::Whitespace, WordSplit::PerUnit}) {
            const auto words = buildWords(s, split);
            std::int64_t last = s.t0;
            for (const BuiltWord& w : words) {
                CHECK(text::isValidUtf8(w.text));
                CHECK_FALSE(w.text.empty());
                CHECK(w.t0 >= last);
                CHECK(w.t1 >= w.t0);
                CHECK(w.t1 <= s.t1);
                last = w.t0;
            }
            CHECK(text::isValidUtf8(segmentText(s)));
        }
    }
}

TEST_CASE("Whisper centiseconds and 16 kHz samples map exactly to source ticks", "[ai][timestamps]") {
    CHECK(centisecondsToTicks(0) == Ticks{0});
    CHECK(centisecondsToTicks(1) == Ticks{kTicksPerSecond / 100});
    CHECK(centisecondsToTicks(123456) == Ticks{123456 * (kTicksPerSecond / 100)});
    CHECK(transcriptionSamplesToTicks(16000) == Ticks::fromSeconds(1));
    CHECK(transcriptionSamplesToTicks(1) == Ticks{kTicksPerSecond / 16000});
    // 10 min into the media, a chunk starting at 1 h of PCM, 2.5 s into the chunk.
    const auto t = chunkTimeToSource(Ticks::fromSeconds(600), 3600LL * 16000, 250);
    REQUIRE(t);
    CHECK(*t == Ticks::fromSeconds(600 + 3600) + Ticks{kTicksPerSecond / 2 * 5});
    // Negative positions and overflow are refused, never wrapped.
    CHECK_FALSE(chunkTimeToSource(Ticks{}, -1, 0));
    CHECK_FALSE(chunkTimeToSource(Ticks{}, 0, -5));
    CHECK_FALSE(centisecondsToTicks(std::numeric_limits<std::int64_t>::max() / 1000));
    CHECK_FALSE(chunkTimeToSource(Ticks{std::numeric_limits<std::int64_t>::max() - 10}, 16000, 0));
    // 48 h of audio is far inside the range.
    CHECK(chunkTimeToSource(Ticks{}, 48LL * 3600 * 16000, 100));
    CHECK(centisecondsToSamples(250) == 40000);
}
