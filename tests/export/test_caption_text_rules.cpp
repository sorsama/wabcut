// Card 114: a caption track's text rules (case, no punctuation, no accents) reach every caption file: the SRT and VTT
// sidecars, the other caption formats (through captionTrackForFile, which captions.export and the export's sidecars use)
// and the soft subtitle track's cues. The cues themselves keep their words as written.

#include <wabcut/export/CaptionSidecar.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Sequence.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace wabcut;
using namespace wabcut::exporter;

namespace {

Ticks ms(std::int64_t v) { return Ticks{v * (kTicksPerSecond / 1000)}; }

CaptionTrack styledTrack(const char* language) {
    CaptionTrack track;
    track.name = "Captions";
    track.language = language;
    track.cues = {{ms(0), ms(2000), "Hello, World! It's 3.5 now."}, {ms(2000), ms(2000), "Qu\xC3\xA9 d\xC3\xAD" "a, Se\xC3\xB1or"},
                  {ms(4000), ms(2000), "ISTANBUL"}};
    return track;
}

TimeRange whole() { return TimeRange::fromStartEnd(Ticks{}, ms(6000)); }

} // namespace

TEST_CASE("Caption text rules reach the SRT and VTT sidecars", "[export][captions][text]") {
    CaptionTrack track = styledTrack("en");
    // As written: untouched.
    const std::string plain = captionSidecarText(track, whole(), CaptionExport::Srt);
    CHECK(plain.find("Hello, World! It's 3.5 now.") != std::string::npos);
    CHECK(plain.find("Qu\xC3\xA9 d\xC3\xAD" "a") != std::string::npos);

    track.style.textCase = CaptionTextCase::Lower;
    track.style.stripPunctuation = true;
    const std::string srt = captionSidecarText(track, whole(), CaptionExport::Srt);
    CHECK(srt.find("hello world it's 3.5 now\n") != std::string::npos);
    CHECK(srt.find("qu\xC3\xA9 d\xC3\xAD" "a se\xC3\xB1or\n") != std::string::npos); // accents stay unless asked
    CHECK(srt.find("istanbul") != std::string::npos);
    CHECK(srt.find("Hello") == std::string::npos);
    CHECK(srt.find("00:00:00,000 --> 00:00:02,000") != std::string::npos);
    // VTT: the same words (its time lines have full stops).
    const std::string vtt = captionSidecarText(track, whole(), CaptionExport::Vtt);
    CHECK(vtt.find("hello world it's 3.5 now\n") != std::string::npos);

    // Accents too, and Upper: one rule at a time or all of them.
    track.style.stripAccents = true;
    track.style.textCase = CaptionTextCase::Upper;
    const std::string loud = captionSidecarText(track, whole(), CaptionExport::Srt);
    CHECK(loud.find("HELLO WORLD IT'S 3.5 NOW\n") != std::string::npos);
    CHECK(loud.find("QUE DIA SENOR\n") != std::string::npos);

    // The track's language picks Turkish casing: I -> dotless i.
    CaptionTrack turkish = styledTrack("tr");
    turkish.style.textCase = CaptionTextCase::Lower;
    CHECK(captionSidecarText(turkish, whole(), CaptionExport::Srt).find("\xC4\xB1stanbul") != std::string::npos);
    // The cue text is as it was.
    CHECK(track.cues[0].text == "Hello, World! It's 3.5 now.");
}

TEST_CASE("Caption text rules reach every other caption file through captionTrackForFile", "[export][captions][text]") {
    ProjectState project;
    Sequence sequence;
    CaptionTrack track = styledTrack("en");
    track.style.textCase = CaptionTextCase::Lower;
    track.style.stripPunctuation = true;
    track.style.stripAccents = true;
    const CaptionTrack file = captionTrackForFile(project, sequence, track);
    REQUIRE(file.cues.size() == 3);
    CHECK(file.cues[0].text == "hello world it's 3.5 now");
    CHECK(file.cues[1].text == "que dia senor");
    CHECK(file.cues[2].text == "istanbul");
    // Timing and the rest of the track are untouched; a style with no rules leaves the text as it is.
    CHECK(file.cues[1].start == ms(2000));
    CHECK(file.language == "en");
    const CaptionTrack asWritten = captionTrackForFile(project, sequence, styledTrack("en"));
    CHECK(asWritten.cues[0].text == "Hello, World! It's 3.5 now.");
    // The styled text helper itself.
    CHECK(captionStyledText(styledTrack("en"), "No change, here.") == "No change, here.");
    CHECK(captionStyledText(track, "A, B") == "a b");
}
