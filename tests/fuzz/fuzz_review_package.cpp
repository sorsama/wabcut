// Fuzz harness: a review package's notes.json (ReviewPackage.h) - comes back from a client or reviewer, so untrusted.
// Notes are imported into a small project with every shape (ScaleProject.h); what imports must be a valid project, and
// importing the same package twice changes nothing more.

#include "FuzzSupport.h"

#include "ScaleProject.h"

#include <wabcut/command/ReviewPackage.h>

using namespace wabcut;

namespace {

const ProjectState& base() {
    static const ProjectState state = test::makeScaleProject(test::smallScaleSpec()).state;
    return state;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text = fuzz::text(data, size);
    const auto imported = command::importReviewPackageData(base(), text);
    if (!imported) {
        return 0;
    }
    const auto valid = validate(imported->state, Validation::Strict);
    WABCUT_FUZZ_CHECK(valid, valid ? std::string() : valid.error().message);
    const auto twice = command::importReviewPackageData(imported->state, text);
    WABCUT_FUZZ_CHECK(twice, twice ? std::string() : twice.error().message);
    WABCUT_FUZZ_CHECK(twice->added == 0 && twice->state == imported->state, "a second import changed the project");
    return 0;
}
