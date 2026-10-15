// Fuzz harness: a .wabcut project file (ProjectIO) - untrusted when a project comes from someone else. Whatever loads
// must be structurally valid, save, and load back to the same project whose save is the same text (round trip).

#include "FuzzSupport.h"

#include <wabcut/model/ProjectIO.h>

using namespace wabcut;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto state = io::fromJson(fuzz::text(data, size));
    if (!state) {
        return 0;
    }
    const auto valid = validate(*state, Validation::Structure);
    WABCUT_FUZZ_CHECK(valid, valid ? std::string() : valid.error().message);
    const auto saved = io::toJson(*state);
    WABCUT_FUZZ_CHECK(saved, saved ? std::string() : saved.error().message);
    const auto again = io::fromJson(*saved);
    WABCUT_FUZZ_CHECK(again, again ? std::string() : again.error().message + "\n" + *saved);
    WABCUT_FUZZ_CHECK(*again == *state, *saved);
    const auto resaved = io::toJson(*again);
    WABCUT_FUZZ_CHECK(resaved && *resaved == *saved, *saved);
    return 0;
}
