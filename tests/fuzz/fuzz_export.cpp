// Fuzz harness: timeline export (Export.h, X.1). The input is a project file (.wabcut JSON, as fuzz_project_io reads
// it); every project that loads has its first sequences exported to FCP7 XML, FCPXML, OTIO and an EDL, and each export
// must import again into a project that passes validate(Strict). Exporters never crash on a valid project and never
// write a file their own importers refuse.

#include "FuzzSupport.h"

#include <wabcut/interchange/Export.h>
#include <wabcut/interchange/Import.h>
#include <wabcut/model/ProjectIO.h>

using namespace wabcut;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto project = io::fromJson(fuzz::text(data, size));
    if (!project) {
        return 0;
    }
    interchange::ImportOptions options;
    options.fileExists = [](const std::filesystem::path&) { return false; };
    std::size_t exported = 0;
    for (const SequencePtr& seq : project->sequences) {
        if (exported++ == 3) {
            break; // a few sequences per input keep each run short
        }
        for (const interchange::Format format : {interchange::Format::Fcp7Xml, interchange::Format::FcpXml,
                                                 interchange::Format::Otio, interchange::Format::Edl}) {
            const auto written = interchange::exportSequence(*project, seq->id, format);
            WABCUT_FUZZ_CHECK(written, written ? std::string() : written.error().message);
            options.edlFrameRate = seq->frameRate;
            const auto back = interchange::ImportRouter::importData(written->data, format, "export", options);
            if (!back && format == interchange::Format::Edl && back.error().message.find("no CMX3600 events") != std::string::npos) {
                continue; // nothing on V1 and A1..A4: an EDL without events, which the importer rightly refuses
            }
            WABCUT_FUZZ_CHECK(back, (back ? std::string() : back.error().message) + "\n" + written->data);
            const auto valid = validate(back->project, Validation::Strict);
            WABCUT_FUZZ_CHECK(valid, valid ? std::string() : valid.error().message);
        }
    }
    return 0;
}
