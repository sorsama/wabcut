// 0.7 (card 062): the channel names a field recorder writes into a polyphonic WAV - the iXML track list, the Broadcast
// Wave description - read bounded from untrusted files (media/WavMetadata.h) and carried by probe() into the media
// item. The WAV files are written here, byte by byte.

#include <wabcut/media/Probe.h>
#include <wabcut/media/WavMetadata.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::media;

namespace {

namespace fs = std::filesystem;

void put32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out += static_cast<char>((v >> (8 * i)) & 0xFFU);
}
void put16(std::string& out, std::uint16_t v) {
    out += static_cast<char>(v & 0xFFU);
    out += static_cast<char>((v >> 8) & 0xFFU);
}
void put64(std::string& out, std::uint64_t v) {
    put32(out, static_cast<std::uint32_t>(v & 0xFFFFFFFFULL));
    put32(out, static_cast<std::uint32_t>(v >> 32));
}

std::string chunk(const char* id, const std::string& body, bool declaredSize = true) {
    std::string out(id, 4);
    put32(out, declaredSize ? static_cast<std::uint32_t>(body.size()) : 0xFFFFFFFFU);
    out += body;
    if (body.size() % 2 != 0)
        out += '\0'; // chunks are padded to an even size
    return out;
}

std::string fmtChunk(int channels) {
    std::string body;
    put16(body, 1); // PCM
    put16(body, static_cast<std::uint16_t>(channels));
    put32(body, 48000);
    put32(body, static_cast<std::uint32_t>(48000 * 2 * channels));
    put16(body, static_cast<std::uint16_t>(2 * channels));
    put16(body, 16);
    return chunk("fmt ", body);
}

std::string dataChunk(int channels, int frames, bool declaredSize = true) {
    return chunk("data", std::string(static_cast<std::size_t>(channels * frames * 2), '\0'), declaredSize);
}

std::string iXml(const std::vector<std::string>& tracks, const std::string& head = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>") {
    std::string xml = head + "<BWFXML><IXML_VERSION>2.0</IXML_VERSION><TRACK_LIST><TRACK_COUNT>" + std::to_string(tracks.size()) +
                      "</TRACK_COUNT>";
    for (const std::string& t : tracks)
        xml += "<TRACK>" + t + "</TRACK>";
    return xml + "</TRACK_LIST></BWFXML>";
}

std::string track(int channel, int interleave, const std::string& name) {
    return "<CHANNEL_INDEX>" + std::to_string(channel) + "</CHANNEL_INDEX><INTERLEAVE_INDEX>" + std::to_string(interleave) +
           "</INTERLEAVE_INDEX><NAME>" + name + "</NAME>";
}

/// A WAV in a temporary folder that goes away with the object.
class TempWav {
public:
    TempWav(const std::string& name, const std::string& bytes) {
        static int counter = 0;
        m_dir = fs::temp_directory_path() / ("wabcut-wavmeta-" + std::to_string(++counter) + "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(m_dir);
        m_path = m_dir / name;
        std::ofstream out(m_path, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    ~TempWav() {
        std::error_code ec;
        fs::remove_all(m_dir, ec);
    }
    TempWav(const TempWav&) = delete;
    TempWav& operator=(const TempWav&) = delete;
    const fs::path& path() const { return m_path; }
    std::string utf8() const { return m_path.generic_string(); }

private:
    fs::path m_dir;
    fs::path m_path;
};

std::string riff(const std::string& id, const std::string& chunks) {
    std::string out(id);
    put32(out, static_cast<std::uint32_t>(4 + chunks.size()));
    out += "WAVE";
    return out + chunks;
}

} // namespace

TEST_CASE("iXML track names: channels are named by their interleave index (062)", "[media][wav][v07]") {
    // Out of order, and the recorder's input numbers (CHANNEL_INDEX 4 and 6) are not the file's channels.
    const std::string xml = iXml({track(6, 2, "Lav A"), track(4, 1, "Boom"), track(7, 4, "Mix R"), track(8, 3, "Mix L")});
    const auto names = parseIxmlTrackNames(xml, 4);
    REQUIRE(names.size() == 4);
    CHECK(names[0] == "Boom");
    CHECK(names[1] == "Lav A");
    CHECK(names[2] == "Mix L");
    CHECK(names[3] == "Mix R");

    // Without INTERLEAVE_INDEX the place in the list counts.
    const auto ordered = parseIxmlTrackNames(iXml({"<NAME>One</NAME>", "<NAME>Two</NAME>"}), 2);
    REQUIRE(ordered.size() == 2);
    CHECK(ordered[0] == "One");
    CHECK(ordered[1] == "Two");

    // A channel nobody named stays empty; the vector is as long as the file's channels.
    const auto gaps = parseIxmlTrackNames(iXml({track(1, 1, "Boom"), track(3, 3, "Lav")}), 4);
    REQUIRE(gaps.size() == 4);
    CHECK(gaps[1].empty());
    CHECK(gaps[3].empty());
}

TEST_CASE("iXML track names: what the file cannot be trusted with is dropped (062)", "[media][wav][v07]") {
    // Out of range, duplicate, empty, control characters, not UTF-8, white space.
    const std::string xml = iXml({track(1, 0, "zero"), track(1, 5, "five"), track(1, 1, "First"), track(2, 1, "Again"),
                                  track(3, 2, "   "), track(4, 3, "Tab\tand\nnew line  gap"), track(5, 4, "Caf\xE9 bad")});
    const auto names = parseIxmlTrackNames(xml, 4);
    REQUIRE(names.size() == 4);
    CHECK(names[0] == "First");  // the second claim of channel 1 is ignored
    CHECK(names[1].empty());     // only white space
    CHECK(names[2] == "Tab and new line gap");
    CHECK(names[3].empty());     // 0xE9 alone is not UTF-8

    CHECK(parseIxmlTrackNames("", 2).empty());
    CHECK(parseIxmlTrackNames("not xml at all", 2).empty());
    CHECK(parseIxmlTrackNames("<BWFXML><PROJECT>x</PROJECT></BWFXML>", 2).empty()); // no track list
    CHECK(parseIxmlTrackNames("<BWFXML><TRACK_LIST><TRACK><NAME>A</NAME></TRACK>", 2).empty()); // cut off: not well formed
    CHECK(parseIxmlTrackNames(iXml({track(1, 1, "A")}), 0).empty());

    // A long name is cut on a character boundary.
    std::string thai;
    for (int i = 0; i < 60; ++i)
        thai += "\xE0\xB8\x81"; // 180 bytes
    const auto cut = parseIxmlTrackNames(iXml({track(1, 1, thai)}), 1);
    REQUIRE(cut.size() == 1);
    CHECK(cut[0].size() <= kMaxChannelNameBytes);
    CHECK(cut[0].size() % 3 == 0);

    // More tracks than the file has channels: the extra ones are ignored; a channel count past the bound is capped.
    CHECK(parseIxmlTrackNames(iXml({track(1, 1, "A"), track(2, 2, "B"), track(3, 3, "C")}), 2).size() == 2);
    CHECK(parseIxmlTrackNames(iXml({track(1, 1, "A")}), 100000).size() == static_cast<std::size_t>(kMaxNamedChannels));
}

TEST_CASE("iXML track names: entities and encodings and hostile documents (062)", "[media][wav][v07]") {
    // The five predefined entities and numeric references are text; entities a DTD defines are never expanded.
    const auto entity = parseIxmlTrackNames(iXml({track(1, 1, "Boom &amp; Lav &#233;")}), 1);
    REQUIRE(entity.size() == 1);
    CHECK(entity[0] == "Boom & Lav \xC3\xA9");

    std::string bomb = "<?xml version=\"1.0\"?><!DOCTYPE x [<!ENTITY a \"aaaaaaaaaa\"><!ENTITY b \"&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;\">"
                       "<!ENTITY c \"&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;\"><!ENTITY d \"&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;\">]>"
                       "<BWFXML><TRACK_LIST><TRACK><INTERLEAVE_INDEX>1</INTERLEAVE_INDEX><NAME>&d;</NAME></TRACK></TRACK_LIST></BWFXML>";
    const auto expanded = parseIxmlTrackNames(bomb, 1);
    for (const std::string& n : expanded)
        CHECK(n.size() <= kMaxChannelNameBytes); // whatever came out, it is small

    // A declared Latin-1 document is read as Latin-1.
    const std::string latin = iXml({track(1, 1, "Caf\xE9")}, "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>");
    const auto decoded = parseIxmlTrackNames(latin, 1);
    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0] == "Caf\xC3\xA9");

    // NUL padding at the end of the chunk is not part of the document.
    const auto padded = parseIxmlTrackNames(iXml({track(1, 1, "Boom")}) + std::string(7, '\0'), 1);
    REQUIRE(padded.size() == 1);
    CHECK(padded[0] == "Boom");

    // A document past the bound is not read at all.
    const std::string huge = iXml({track(1, 1, "Boom"), "<NAME>" + std::string(kMaxIxmlBytes, 'x') + "</NAME>"});
    CHECK(parseIxmlTrackNames(huge, 1).empty());

    // Thousands of tracks are cheap.
    std::vector<std::string> many;
    for (int i = 1; i <= 3000; ++i)
        many.push_back(track(i, i, "T" + std::to_string(i)));
    const auto big = parseIxmlTrackNames(iXml(many), 300);
    REQUIRE(big.size() == static_cast<std::size_t>(kMaxNamedChannels));
    CHECK(big[255] == "T256");
}

TEST_CASE("Broadcast Wave description track names: sTRK lines (062)", "[media][wav][v07]") {
    const auto names = parseBextTrackNames("sSPEED=024.000-ND\r\nsTAKE=3\r\nsTRK1=Boom\r\nsTRK2=Lav A\r\nsTRK9=nope\r\nsTRK3=\r\nsNOTE=x\r\n", 4);
    REQUIRE(names.size() == 4);
    CHECK(names[0] == "Boom");
    CHECK(names[1] == "Lav A");
    CHECK(names[2].empty());
    CHECK(parseBextTrackNames("nothing of the kind", 2).empty());
    CHECK(parseBextTrackNames(std::string("sTRK1=Boom") + '\0' + "sTRK2=after the end", 2).at(1).empty());
}

TEST_CASE("A WAV file's chunks give the channel names; sizes that lie are survived (062)", "[media][wav][v07]") {
    const std::string xml = iXml({track(1, 1, "Boom"), track(2, 2, "Lav A")});
    // iXML after the sound, as recorders write it.
    {
        TempWav wav("poly.wav", riff("RIFF", fmtChunk(2) + dataChunk(2, 480) + chunk("iXML", xml)));
        const auto names = readWavChannelNames(wav.path(), 2);
        REQUIRE(names.size() == 2);
        CHECK(names[0] == "Boom");
        CHECK(names[1] == "Lav A");
    }
    // RF64: the data chunk's size is 0xFFFFFFFF and the real one is in ds64.
    {
        std::string ds64;
        put64(ds64, 0);
        put64(ds64, static_cast<std::uint64_t>(2 * 480 * 2));
        put64(ds64, 480);
        put32(ds64, 0);
        TempWav wav("big.wav", riff("RF64", chunk("ds64", ds64) + fmtChunk(2) + dataChunk(2, 480, false) + chunk("iXML", xml)));
        const auto names = readWavChannelNames(wav.path(), 2);
        REQUIRE(names.size() == 2);
        CHECK(names[1] == "Lav A");
    }
    // Only a bext description: its names stand; an iXML track list takes precedence when there is one.
    {
        std::string bext(256, '\0');
        const std::string text = "sTRK1=Desc One\r\nsTRK2=Desc Two";
        bext.replace(0, text.size(), text);
        bext += std::string(100, '\0');
        TempWav onlyBext("bext.wav", riff("RIFF", chunk("bext", bext) + fmtChunk(2) + dataChunk(2, 480)));
        REQUIRE(readWavChannelNames(onlyBext.path(), 2).size() == 2);
        CHECK(readWavChannelNames(onlyBext.path(), 2)[0] == "Desc One");
        TempWav both("both.wav", riff("RIFF", chunk("bext", bext) + fmtChunk(2) + dataChunk(2, 480) + chunk("iXML", xml)));
        CHECK(readWavChannelNames(both.path(), 2)[0] == "Boom");
    }
    // A chunk that claims more than the file holds, a cut-off file, a file that is not a WAV, no names.
    {
        std::string lying("iXML", 4);
        put32(lying, 0x7FFFFFF0U);
        lying += "<BWFXML>";
        TempWav wav("lies.wav", riff("RIFF", fmtChunk(2) + dataChunk(2, 10) + lying));
        CHECK(readWavChannelNames(wav.path(), 2).empty());
        TempWav cut("cut.wav", riff("RIFF", fmtChunk(2)).substr(0, 20));
        CHECK(readWavChannelNames(cut.path(), 2).empty());
        TempWav notWav("text.wav", "this is not a wave file at all");
        CHECK(readWavChannelNames(notWav.path(), 2).empty());
        TempWav plain("plain.wav", riff("RIFF", fmtChunk(2) + dataChunk(2, 10)));
        CHECK(readWavChannelNames(plain.path(), 2).empty());
        CHECK(readWavChannelNames(plain.path().parent_path() / "missing.wav", 2).empty());
    }
    // An iXML chunk past kMaxIxmlBytes is skipped, not read.
    {
        TempWav wav("fat.wav", riff("RIFF", fmtChunk(2) + dataChunk(2, 10) + chunk("iXML", xml + std::string(kMaxIxmlBytes, ' '))));
        CHECK(readWavChannelNames(wav.path(), 2).empty());
    }
}

TEST_CASE("probe() puts a WAV's channel names in the media item; a mono name titles the stream (062)", "[media][wav][v07]") {
    TempWav poly("poly4.wav", riff("RIFF", fmtChunk(4) + dataChunk(4, 4800) +
                                                   chunk("iXML", iXml({track(1, 1, "Boom"), track(2, 2, "Lav A"), track(3, 3, "Lav B"),
                                                                       track(4, 4, "Mix")}))));
    const auto info = probe(poly.utf8());
    REQUIRE(info);
    REQUIRE(info->audio.size() == 1);
    CHECK(info->audio.front().channels == 4);
    REQUIRE(info->audio.front().channelNames.size() == 4);
    CHECK(info->audio.front().channelNames[0] == "Boom");
    CHECK(info->audio.front().channelNames[3] == "Mix");
    CHECK(info->audio.front().title.empty()); // a poly file has no single name

    TempWav mono("mono.wav", riff("RIFF", fmtChunk(1) + dataChunk(1, 4800) + chunk("iXML", iXml({track(2, 1, "Boom")}))));
    const auto monoInfo = probe(mono.utf8());
    REQUIRE(monoInfo);
    CHECK(monoInfo->audio.front().title == "Boom");
    CHECK(monoInfo->audio.front().channelNames == std::vector<std::string>{"Boom"});

    TempWav plain("plain2.wav", riff("RIFF", fmtChunk(2) + dataChunk(2, 4800)));
    const auto plainInfo = probe(plain.utf8());
    REQUIRE(plainInfo);
    CHECK(plainInfo->audio.front().channelNames.empty());
}
