// Fuzz harness: a timeline from another editor (Import.h) - built once per format, WABCUT_FUZZ_FORMAT naming it:
// Fcp7Xml, FcpXml, Otio, Edl or PremiereProject (gzip + XML). The importer promises a project that passes
// validate(Strict) or an error, never a crash; what it imports must also survive a .wabcut round trip, and (X.1) export
// to this format again and import back into a valid project. Media paths in the file are never looked up on disk
// (ImportOptions::fileExists).

#include "FuzzSupport.h"

#include <wabcut/interchange/Export.h>
#include <wabcut/interchange/Import.h>
#include <wabcut/model/ProjectIO.h>

using namespace wabcut;

#ifndef WABCUT_FUZZ_FORMAT
#error "WABCUT_FUZZ_FORMAT must name an interchange::Format"
#endif

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    interchange::ImportOptions options;
    options.experimentalPremiereProject = true;
    options.fileExists = [](const std::filesystem::path&) { return false; };
    options.edlFrameRate = size > 0 && data[0] % 2 == 0 ? std::optional<Rate>() : std::optional<Rate>(kRate25);
    const auto result = interchange::ImportRouter::importData(fuzz::text(data, size), interchange::Format::WABCUT_FUZZ_FORMAT,
                                                              "fuzz", options);
    if (!result) {
        return 0;
    }
    const auto valid = validate(result->project, Validation::Strict);
    WABCUT_FUZZ_CHECK(valid, valid ? std::string() : valid.error().message);
    const auto saved = io::toJson(result->project);
    WABCUT_FUZZ_CHECK(saved, saved ? std::string() : saved.error().message);
    const auto again = io::fromJson(*saved);
    WABCUT_FUZZ_CHECK(again, again ? std::string() : again.error().message + "\n" + *saved);
    WABCUT_FUZZ_CHECK(*again == result->project, *saved);
    // X.1: the exporter's inverse. Exported back to the same format (the .prproj reader's timelines as FCP7 XML), the
    // file imports again.
    constexpr interchange::Format kBack =
        interchange::Format::WABCUT_FUZZ_FORMAT == interchange::Format::PremiereProject ? interchange::Format::Fcp7Xml
                                                                                       : interchange::Format::WABCUT_FUZZ_FORMAT;
    for (const SequenceId seq : result->sequences) {
        const auto written = interchange::exportSequence(result->project, seq, kBack);
        WABCUT_FUZZ_CHECK(written, written ? std::string() : written.error().message);
        options.edlFrameRate = result->project.findSequence(seq)->frameRate;
        const auto back = interchange::ImportRouter::importData(written->data, kBack, "export", options);
        if (!back && kBack == interchange::Format::Edl && back.error().message.find("no CMX3600 events") != std::string::npos) {
            continue;
        }
        WABCUT_FUZZ_CHECK(back, (back ? std::string() : back.error().message) + "\n" + written->data);
        const auto backValid = validate(back->project, Validation::Strict);
        WABCUT_FUZZ_CHECK(backValid, backValid ? std::string() : backValid.error().message);
        break; // the first sequence is enough per input
    }
    return 0;
}
