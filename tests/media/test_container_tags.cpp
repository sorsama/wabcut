// 0.2 (P.3): the demuxer's metadata tags for the Metadata panel's File section (media/ContainerTags.h).

#include "TestMedia.h"

#include <wabcut/media/ContainerTags.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <string>

using namespace wabcut;
using namespace wabcut::media;
using namespace wabcut::media::test;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

TEST_CASE("Container tags: the file's and each stream's, as the demuxer reads them", "[media][tags]") {
    const auto tags = readContainerTags(mediaPath("obs_multitrack_tracknames.mkv"));
    REQUIRE(tags);
    // The audio streams carry their titles (OBS's track names), each with its stream index and kind.
    std::vector<std::string> titles;
    for (const ContainerTag& t : *tags) {
        if (t.stream >= 0 && t.kind == "audio" && lower(t.key) == "title")
            titles.push_back(t.value);
    }
    CHECK(titles == std::vector<std::string>{"Track1", "Track2", "Track3"});
    // The container's tags come first (stream -1).
    REQUIRE_FALSE(tags->empty());
    const auto firstStream = std::find_if(tags->begin(), tags->end(), [](const ContainerTag& t) { return t.stream >= 0; });
    CHECK(std::all_of(tags->begin(), firstStream, [](const ContainerTag& t) { return t.stream == -1 && t.kind.empty(); }));

    CHECK(readContainerTags(mediaPath("does_not_exist.mkv")).error().code == ErrorCode::NotFound);
}
