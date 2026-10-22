// Fuzz harness: caption files (P.2, interchange/CaptionFiles.h) - subtitle and closed caption files from anywhere, so
// untrusted. Every format's reader takes the input (and detection); every successful read gives cues the model accepts
// (sorted, positive, not overlapping, valid text within the limits), and writing them back in that format works.

#include "FuzzSupport.h"

#include <wabcut/interchange/CaptionFiles.h>
#include <wabcut/model/CaptionStyle.h>
#include <wabcut/model/CaptionText.h>

using namespace wabcut;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view body = fuzz::text(data, size);
    static_cast<void>(interchange::detectCaptionFile(body, "fuzz.xml"));
    for (const interchange::CaptionFileFormat format : interchange::captionFileFormats()) {
        interchange::CaptionReadOptions options;
        options.startTimecode = size % 2 == 1 ? Ticks::fromSeconds(3600) : Ticks{};
        const auto read = interchange::readCaptionData(body, format, options, "fuzz");
        if (!read) {
            WABCUT_FUZZ_CHECK(read.error().code == ErrorCode::ParseError, read.error().message);
            continue;
        }
        WABCUT_FUZZ_CHECK(read->cues.size() <= kMaxCaptionCues, "too many cues");
        for (std::size_t i = 0; i < read->cues.size(); ++i) {
            const CaptionCue& c = read->cues[i];
            WABCUT_FUZZ_CHECK(c.start.value >= 0 && c.duration.value > 0, "a cue with no time");
            WABCUT_FUZZ_CHECK(c.text.size() <= kMaxCaptionTextBytes && isValidCaptionText(c.text),
                              "cue text the model refuses");
            WABCUT_FUZZ_CHECK(i == 0 || read->cues[i - 1].end() <= c.start, "overlapping or unsorted cues");
        }
        if (!read->cues.empty()) {
            interchange::CaptionWriteOptions out;
            out.frameRate = format == interchange::CaptionFileFormat::EbuStl ? kRate25 : kRate29_97;
            const auto written =
                interchange::writeCaptionData(read->cues, TimeRange{Ticks{}, read->cues.back().end()}, format, out, "fuzz");
            WABCUT_FUZZ_CHECK(written.has_value(), written ? std::string() : written.error().message);
        }
    }
    return 0;
}
