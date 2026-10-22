// 0.2 (P.3): XMP of media files (interchange/Xmp.h) - the Metadata panel's File section and Premiere's links between XMP
// and the Project panel's clip metadata.

#include <wabcut/interchange/Xmp.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace wabcut;
namespace fs = std::filesystem;

namespace {

constexpr const char* kPacket = R"(<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""
    xmlns:dm="http://ns.adobe.com/xmp/1.0/DynamicMedia/"
    xmlns:dc="http://purl.org/dc/elements/1.1/"
    xmlns:xmp="http://ns.adobe.com/xap/1.0/"
    xmlns:acme="http://example.com/acme/"
    dm:scene="12" dm:good="True" xmp:CreatorTool="Camera 9000" acme:roll="R7">
   <dm:shotName>3A</dm:shotName>
   <dm:takeNumber>2</dm:takeNumber>
   <dm:logComment>Laughs at the end</dm:logComment>
   <dm:tapeName>A001</dm:tapeName>
   <dc:title><rdf:Alt><rdf:li xml:lang="fr">Entretien</rdf:li><rdf:li xml:lang="x-default">Interview</rdf:li></rdf:Alt></dc:title>
   <dc:description><rdf:Alt><rdf:li xml:lang="x-default">Kitchen, day</rdf:li></rdf:Alt></dc:description>
   <dc:subject><rdf:Bag><rdf:li>kitchen</rdf:li><rdf:li>day</rdf:li></rdf:Bag></dc:subject>
   <dm:startTimecode rdf:parseType="Resource">
    <dm:timeFormat>25Timecode</dm:timeFormat>
    <dm:timeValue>01:00:00:00</dm:timeValue>
   </dm:startTimecode>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>
<?xpacket end="w"?>)";

const interchange::XmpProperty* find(const std::vector<interchange::XmpProperty>& list, std::string_view prefix,
                                     std::string_view name) {
    for (const auto& p : list) {
        if (p.prefix == prefix && p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "xmp" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void writeBytes(const fs::path& file, const std::string& bytes) {
    std::ofstream out(file, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string readBytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string be32(std::uint32_t v) {
    return {static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
}

std::string box(const std::string& type, const std::string& payload) {
    return be32(static_cast<std::uint32_t>(8 + payload.size())) + type + payload;
}

} // namespace

TEST_CASE("XMP properties are read whatever prefix and form the writer chose", "[interchange][xmp]") {
    const auto props = interchange::parseXmp(kPacket);
    REQUIRE(props);
    const auto* scene = find(*props, "xmpDM", "scene"); // written as "dm:", read as the namespace's usual prefix
    REQUIRE(scene);
    CHECK(scene->value == "12");
    CHECK(scene->label == "Scene");
    CHECK(scene->group == "Dynamic Media");
    CHECK(find(*props, "xmpDM", "shotName")->value == "3A");
    CHECK(find(*props, "dc", "title")->value == "Interview"); // x-default wins over the first language
    CHECK(find(*props, "dc", "subject")->value == "kitchen, day");
    CHECK(find(*props, "dc", "subject")->label == "Keywords");
    CHECK(find(*props, "xmpDM", "startTimecode/timeValue")->value == "01:00:00:00");
    CHECK(find(*props, "xmp", "CreatorTool")->value == "Camera 9000");
    const auto* custom = find(*props, "acme", "roll");
    REQUIRE(custom);
    CHECK(custom->ns == "http://example.com/acme/");
    CHECK(custom->group == "acme");

    const interchange::XmpClipFields f = interchange::clipFieldsFromXmp(*props);
    CHECK(f.scene == "12");
    CHECK(f.shot == "3A");
    CHECK(f.take == "2");
    CHECK(f.logNote == "Laughs at the end");
    CHECK(f.tapeName == "A001");
    CHECK(f.good == true);
    CHECK(f.description == "Kitchen, day");
    CHECK_FALSE(f.client.has_value());

    CHECK_FALSE(interchange::parseXmp("<x:xmpmeta><rdf:RDF").has_value());
    CHECK(interchange::parseXmp("<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/>").value().empty());
}

TEST_CASE("XMP is found in a sidecar first, then in MP4 boxes or by scanning the file", "[interchange][xmp]") {
    const fs::path dir = scratch("read");

    // An MP4 with Adobe's XMP uuid box after the media data: found by walking the boxes.
    const std::string uuid = "\xBE\x7A\xCF\xCB\x97\xA9\x42\xE8\x9C\x71\x99\x94\x91\xE3\xAF\xAC";
    const fs::path mp4 = dir / "clip.mp4";
    writeBytes(mp4, box("ftyp", "isom\0\0\0\0isom") + box("mdat", std::string(4096, '\x07')) + box("uuid", uuid + kPacket));
    auto read = interchange::readMediaXmp(mp4);
    REQUIRE(read);
    REQUIRE(read->has_value());
    CHECK((*read)->source == interchange::XmpSource::Embedded);
    CHECK(find((*read)->properties, "xmpDM", "scene")->value == "12");

    // QuickTime's moov/udta/XMP_.
    const fs::path mov = dir / "clip2.mov";
    writeBytes(mov, box("ftyp", "qt  \0\0\0\0qt  ") + box("moov", box("udta", box("XMP_", kPacket))));
    read = interchange::readMediaXmp(mov);
    REQUIRE(read);
    REQUIRE(read->has_value());
    CHECK(find((*read)->properties, "xmpDM", "takeNumber")->value == "2");

    // Any other file: packet scanning (a JPEG-like file with the packet in an APP1 segment).
    const fs::path jpg = dir / "still.jpg";
    writeBytes(jpg, std::string("\xFF\xD8\xFF\xE1", 4) + "http://ns.adobe.com/xap/1.0/" + std::string(1, '\0') + kPacket +
                        std::string(2000, '\x11'));
    read = interchange::readMediaXmp(jpg);
    REQUIRE(read);
    REQUIRE(read->has_value());
    CHECK(find((*read)->properties, "dc", "title")->value == "Interview");

    // A sidecar wins over the packet inside.
    std::string sidecarText = kPacket;
    sidecarText.replace(sidecarText.find("dm:scene=\"12\""), 13, "dm:scene=\"99\"");
    writeBytes(interchange::xmpSidecarPath(jpg), sidecarText);
    CHECK(interchange::xmpSidecarPath(jpg) == dir / "still.xmp");
    read = interchange::readMediaXmp(jpg);
    REQUIRE(read);
    CHECK((*read)->source == interchange::XmpSource::Sidecar);
    CHECK(find((*read)->properties, "xmpDM", "scene")->value == "99");

    // No XMP at all.
    const fs::path wav = dir / "plain.wav";
    writeBytes(wav, "RIFF" + std::string(64, '\0'));
    read = interchange::readMediaXmp(wav);
    REQUIRE(read);
    CHECK_FALSE(read->has_value());
    CHECK_FALSE(interchange::readMediaXmp(dir / "missing.mov").has_value());
}

TEST_CASE("A sidecar is written with the clip metadata and keeps what else it holds", "[interchange][xmp]") {
    const fs::path dir = scratch("write");
    const fs::path media = dir / "A001_C002.mxf";
    writeBytes(media, std::string(256, '\0'));
    MediaItem item;
    item.name = "Interview <1> & more";
    item.locator.path = pathToUtf8(media);
    item.description = "Kitchen";
    item.comment = "Use the second half";
    item.metadata.scene = "12";
    item.metadata.shot = "3";
    item.metadata.take = "4";
    item.metadata.logNote = "Laughs";
    item.metadata.client = "Northwind";
    item.metadata.good = true;
    item.timecode = MediaTimecode{std::nullopt, "A001"};

    // Fresh.
    const auto written = interchange::writeXmpSidecar(item);
    REQUIRE(written);
    CHECK(*written == dir / "A001_C002.xmp");
    auto back = interchange::readMediaXmp(media);
    REQUIRE(back);
    REQUIRE(back->has_value());
    interchange::XmpClipFields f = interchange::clipFieldsFromXmp((*back)->properties);
    CHECK(f.scene == "12");
    CHECK(f.shot == "3");
    CHECK(f.take == "4");
    CHECK(f.logNote == "Laughs");
    CHECK(f.client == "Northwind");
    CHECK(f.good == true);
    CHECK(f.tapeName == "A001");
    CHECK(f.comment == "Use the second half");
    CHECK(f.description == "Kitchen");
    CHECK(find((*back)->properties, "dc", "title")->value == "Interview <1> & more");

    // An existing sidecar from another program: its other properties stay, the linked ones are replaced or removed.
    writeBytes(*written, kPacket);
    item.metadata.scene = "13";
    item.metadata.good = false;
    item.metadata.take = "4b"; // not a number: xmpDM:takeNumber is an integer, so it is left out
    REQUIRE(interchange::writeXmpSidecar(item));
    back = interchange::readMediaXmp(media);
    REQUIRE(back);
    REQUIRE(back->has_value());
    f = interchange::clipFieldsFromXmp((*back)->properties);
    CHECK(f.scene == "13");
    CHECK_FALSE(f.good.has_value());
    CHECK_FALSE(f.take.has_value());
    CHECK(find((*back)->properties, "acme", "roll")->value == "R7");
    CHECK(find((*back)->properties, "xmp", "CreatorTool")->value == "Camera 9000");
    CHECK(find((*back)->properties, "dc", "subject")->value == "kitchen, day");
    CHECK(find((*back)->properties, "xmpDM", "startTimecode/timeValue")->value == "01:00:00:00");
    const std::string text = readBytes(*written);
    CHECK(text.find("dm:scene=\"12\"") == std::string::npos); // the old attribute went

    // Items without a file have nothing to write next to.
    MediaItem title;
    title.name = "Title";
    title.generator = GeneratorSpec{std::string(kTitleGenerator), "Hi", "Arial", "Regular", {}};
    CHECK(interchange::writeXmpSidecar(title).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("The File section's edits go into the sidecar; bags, alternatives and removals keep their XMP form", "[interchange][xmp]") {
    const fs::path dir = scratch("edit");
    const fs::path media = dir / "C001.mxf";
    writeBytes(media, std::string(128, '\0'));
    writeBytes(interchange::xmpSidecarPath(media), kPacket);
    const std::string dm = "http://ns.adobe.com/xmp/1.0/DynamicMedia/";
    const std::string dc = "http://purl.org/dc/elements/1.1/";
    const std::vector<interchange::XmpEdit> edits{
        {dm, "xmpDM", "cameraModel", "FX6"},
        {dc, "dc", "subject", "kitchen, night, b-roll"},
        {dc, "dc", "rights", "(c) 2026 Northwind"},
        {dm, "xmpDM", "scene", ""},                      // removed (it was an attribute, prefix "dm")
        {dm, "xmpDM", "startTimecode/timeValue", "x"},   // a structure's field: not set
    };
    REQUIRE(interchange::editXmpSidecar(media, edits));
    auto back = interchange::readMediaXmp(media);
    REQUIRE(back);
    REQUIRE(back->has_value());
    const auto& props = (*back)->properties;
    CHECK(find(props, "xmpDM", "cameraModel")->value == "FX6");
    CHECK(find(props, "dc", "subject")->value == "kitchen, night, b-roll");
    CHECK(find(props, "dc", "rights")->value == "(c) 2026 Northwind");
    CHECK(find(props, "xmpDM", "scene") == nullptr);
    CHECK(find(props, "xmpDM", "startTimecode/timeValue")->value == "01:00:00:00"); // untouched
    CHECK(find(props, "acme", "roll")->value == "R7");
    // The camera fields are linked to the clip metadata.
    const interchange::XmpClipFields f = interchange::clipFieldsFromXmp(props);
    CHECK(f.cameraModel == "FX6");
    CHECK_FALSE(f.scene.has_value());
    // Known properties offered to add.
    const auto known = interchange::knownXmpProperties();
    CHECK(std::any_of(known.begin(), known.end(), [](const interchange::XmpProperty& p) {
        return p.prefix == "xmpDM" && p.name == "shotLocation" && p.label == "Shot Location";
    }));
}

TEST_CASE("XMP is written into an MP4 through a checked copy; the media data never moves", "[interchange][xmp][embed]") {
    const fs::path dir = scratch("embed");
    const std::string uuid = "\xBE\x7A\xCF\xCB\x97\xA9\x42\xE8\x9C\x71\x99\x94\x91\xE3\xAF\xAC";
    // ftyp, moov with an old QuickTime XMP_, an old XMP uuid box, then mdat of open size (to the end of the file).
    const std::string ftyp = box("ftyp", "isom");
    const std::string moov = box("moov", box("udta", box("XMP_", kPacket)));
    const std::string oldUuid = box("uuid", uuid + kPacket);
    const std::string mdatBody(5000, '\x5A');
    const std::string mdat = be32(0) + "mdat" + mdatBody;
    const fs::path mp4 = dir / "clip.mp4";
    writeBytes(mp4, ftyp + moov + oldUuid + mdat);
    REQUIRE(interchange::canEmbedXmp(mp4));
    CHECK_FALSE(interchange::canEmbedXmp(dir / "missing.mp4"));
    const std::uint64_t mdatAt = ftyp.size() + moov.size() + oldUuid.size();

    const std::string dm = "http://ns.adobe.com/xmp/1.0/DynamicMedia/";
    REQUIRE(interchange::embedXmp(mp4, {{dm, "xmpDM", "scene", "42"}, {dm, "xmpDM", "cameraLabel", "B cam"}}));
    const std::string after = readBytes(mp4);
    // The media data is where it was, with its size written out; the old boxes are free space now.
    REQUIRE(after.size() > mdatAt + 8 + mdatBody.size());
    CHECK(after.substr(mdatAt + 4, 4) == "mdat");
    CHECK(after.substr(mdatAt, 4) == be32(static_cast<std::uint32_t>(8 + mdatBody.size())));
    CHECK(after.substr(mdatAt + 8, mdatBody.size()) == mdatBody);
    CHECK(after.find("XMP_") == std::string::npos);
    CHECK(after.substr(ftyp.size() + moov.size() + 4, 4) == "free");
    auto read = interchange::readMediaXmp(mp4);
    REQUIRE(read);
    REQUIRE(read->has_value());
    CHECK(find((*read)->properties, "xmpDM", "scene")->value == "42");
    CHECK(find((*read)->properties, "xmpDM", "cameraLabel")->value == "B cam");
    CHECK(find((*read)->properties, "acme", "roll")->value == "R7"); // the rest of the old packet kept
    CHECK_FALSE(fs::exists(fs::path(mp4.string() + ".wabcut-xmp.tmp")));

    // Again: the trailing XMP box is replaced, the file does not keep growing.
    const auto size = fs::file_size(mp4);
    REQUIRE(interchange::embedXmp(mp4, {{dm, "xmpDM", "scene", "43"}}));
    CHECK(fs::file_size(mp4) < size + 64);
    read = interchange::readMediaXmp(mp4);
    REQUIRE(read);
    CHECK(find((*read)->properties, "xmpDM", "scene")->value == "43");

    // Not ISO-BMFF: refused, nothing written.
    const fs::path wav = dir / "sound.wav";
    writeBytes(wav, "RIFF" + std::string(64, '\0'));
    CHECK(interchange::embedXmp(wav, {{dm, "xmpDM", "scene", "1"}}).error().code == ErrorCode::InvalidArgument);
    CHECK(readBytes(wav) == "RIFF" + std::string(64, '\0'));
}

#ifdef _WIN32
TEST_CASE("A file another program holds open is left as it was when XMP cannot be written into it", "[interchange][xmp][embed]") {
    const fs::path dir = scratch("embed-busy");
    const fs::path mp4 = dir / "busy.mp4";
    const std::string original = box("ftyp", "isom") + box("moov", "") + box("mdat", std::string(100, '\x01'));
    writeBytes(mp4, original);
    std::ifstream holder(mp4, std::ios::binary); // no FILE_SHARE_DELETE: the file cannot be replaced while it is open
    REQUIRE(holder);
    const std::string dm = "http://ns.adobe.com/xmp/1.0/DynamicMedia/";
    const auto written = interchange::embedXmp(mp4, {{dm, "xmpDM", "scene", "1"}});
    CHECK_FALSE(written.has_value());
    holder.close();
    CHECK(readBytes(mp4) == original);
    CHECK_FALSE(fs::exists(fs::path(mp4.string() + ".wabcut-xmp.tmp")));
}
#endif
