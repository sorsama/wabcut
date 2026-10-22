// Fuzz harness: Motion Graphics templates (.mogrt, 0.2, interchange/Mogrt.h) and the bounded ZIP reader (Zip.h) - files
// from anywhere, so untrusted. The first byte picks the input: '0' + a definition.json wrapped in a valid ZIP (the
// importer's JSON handling), '1' + a raw ZIP (the reader), '2' + entries "name\ncontents\n..." zipped (names and
// contents). A ZIP opens only with safe names; reading an entry never exceeds its declared size; an import either fails
// with an error that is not an importer bug ("mogrt: internal ..."), or gives a template that passes templateProblem
// and comes back the same through the template file JSON.

#include "FuzzSupport.h"

#include <wabcut/interchange/Mogrt.h>
#include <wabcut/interchange/Zip.h>
#include <wabcut/model/GraphicJson.h>

#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::interchange;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const std::string_view rest = fuzz::text(data + 1, size - 1);
    std::string archive;
    switch (data[0] % 3) {
    case 0: {
        const std::vector<std::pair<std::string, std::string>> files{{"definition.json", std::string(rest)},
                                                                     {"project.prgraphic", "graphic"}};
        archive = writeStoredZip(files);
        break;
    }
    case 1:
        archive = std::string(rest);
        break;
    default: {
        std::vector<std::pair<std::string, std::string>> files;
        std::size_t at = 0;
        while (at < rest.size() && files.size() < 16) {
            const std::size_t a = std::min(rest.find('\n', at), rest.size());
            const std::size_t b = std::min(rest.find('\n', a + 1 < rest.size() ? a + 1 : rest.size()), rest.size());
            files.emplace_back(std::string(rest.substr(at, a - at)), a < rest.size() ? std::string(rest.substr(a + 1, b - a - 1)) : "");
            at = b + 1;
        }
        archive = writeStoredZip(files);
        break;
    }
    }
    if (auto zip = ZipArchive::open(archive)) {
        for (const ZipEntry& e : zip->entries()) {
            WABCUT_FUZZ_CHECK(isSafeZipPath(e.name), e.name);
            if (auto bytes = zip->read(e)) {
                WABCUT_FUZZ_CHECK(bytes->size() == e.size, e.name);
            }
        }
    }
    const auto imported = importMogrt(archive, "fuzz.mogrt");
    if (!imported) {
        WABCUT_FUZZ_CHECK(!imported.error().message.starts_with("mogrt: internal"), imported.error().message);
        return 0;
    }
    WABCUT_FUZZ_CHECK(!templateProblem(imported->tmpl), templateProblem(imported->tmpl).value_or(""));
    const auto again = io::templateFileFromJson(io::templateFileToJson(imported->tmpl));
    WABCUT_FUZZ_CHECK(again && *again == imported->tmpl, again ? std::string("template round trip") : again.error().message);
    return 0;
}
