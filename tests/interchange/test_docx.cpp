// 0.2 graphics: Word documents for transcripts (Docx.h): written so Word opens them, read back as lines, untrusted
// input refused cleanly.

#include <wabcut/interchange/Docx.h>
#include <wabcut/interchange/Zip.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::interchange;

TEST_CASE("A written .docx has the parts Word needs and reads back as its lines", "[interchange][docx]") {
    const std::vector<DocxParagraph> doc{
        {{{"00:00:01:00 - 00:00:04:12", false, true}}},
        {{{"Somchai & <Guest>", true, false}}},
        {{{"Line one\nline two\twith a tab", false, false}}, 10},
        {},
        {{{"\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5", false, false}}}, // Thai
    };
    const std::string bytes = writeDocx(doc, "Interview");
    auto zip = ZipArchive::open(bytes);
    REQUIRE(zip);
    for (const char* part : {"[Content_Types].xml", "_rels/.rels", "word/document.xml", "docProps/core.xml"}) {
        INFO(part);
        CHECK(zip->find(part));
    }
    const std::string xml = *zip->read(*zip->find("word/document.xml"));
    CHECK(xml.find("Somchai &amp; &lt;Guest&gt;") != std::string::npos);
    CHECK(xml.find("<w:b/>") != std::string::npos);
    CHECK(xml.find("<w:br/>") != std::string::npos);
    CHECK(xml.find("<w:tab/>") != std::string::npos);
    const auto text = readDocxText(bytes);
    REQUIRE(text);
    CHECK(*text == "00:00:01:00 - 00:00:04:12\nSomchai & <Guest>\nLine one\nline two\twith a tab\n\n"
                   "\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5\n");
}

TEST_CASE("Reading a .docx skips tracked deletions and refuses what is not a Word document", "[interchange][docx]") {
    const std::string document =
        "<?xml version=\"1.0\"?><w:document xmlns:w=\"x\"><w:body>"
        "<w:p><w:r><w:t>kept</w:t></w:r><w:del><w:r><w:delText>gone</w:delText></w:r></w:del></w:p>"
        "<w:tbl><w:tr><w:tc><w:p><w:r><w:t>cell</w:t></w:r></w:p></w:tc></w:tr></w:tbl>"
        "</w:body></w:document>";
    const std::array<std::pair<std::string, std::string>, 1> files{{{"word/document.xml", document}}};
    const auto text = readDocxText(writeStoredZip(files));
    REQUIRE(text);
    CHECK(*text == "kept\ncell\n");

    CHECK_FALSE(readDocxText("not a zip"));
    const std::array<std::pair<std::string, std::string>, 1> other{{{"content.xml", "<x/>"}}};
    CHECK_FALSE(readDocxText(writeStoredZip(other)));
    const std::array<std::pair<std::string, std::string>, 1> broken{{{"word/document.xml", "<w:document><w:body>"}}};
    CHECK_FALSE(readDocxText(writeStoredZip(broken)));
    // Control characters cannot be in XML: they are dropped when writing.
    const std::vector<DocxParagraph> odd{{{{std::string("a\x01" "b"), false, false}}}};
    const auto back = readDocxText(writeDocx(odd, "x"));
    REQUIRE(back);
    CHECK(*back == "ab\n");
}
