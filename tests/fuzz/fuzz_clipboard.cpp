// Fuzz harness: a clipboard document (Clipboard.h, C.11) - pasted from another Wabcut, so untrusted. What reads must
// come back the same through its JSON, and pasting it (overwrite and insert) into a small project with every shape
// (ScaleProject.h) gives a project that passes validate(Strict), or is refused.

#include "FuzzSupport.h"

#include "ScaleProject.h"

#include <wabcut/model/Clipboard.h>

using namespace wabcut;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const test::ScaleProject project = test::makeScaleProject(test::smallScaleSpec());
    const auto doc = clipboardFromJson(fuzz::text(data, size));
    if (!doc) {
        return 0;
    }
    const auto text = clipboardToJson(*doc);
    WABCUT_FUZZ_CHECK(text, text ? std::string() : text.error().message);
    const auto again = clipboardFromJson(*text);
    WABCUT_FUZZ_CHECK(again, again ? std::string() : again.error().message);
    WABCUT_FUZZ_CHECK(again->content == doc->content && again->duration == doc->duration, "clipboard round trip");
    const SequenceId seq = project.sequences.front();
    for (const PasteMode mode : {PasteMode::Overwrite, PasteMode::Insert}) {
        PasteOptions options;
        options.mode = mode;
        const auto pasted = pasteClips(project.state, seq, *doc, Ticks{7}, options);
        if (pasted) {
            const auto valid = validate(pasted->edit.state, Validation::Strict);
            WABCUT_FUZZ_CHECK(valid, valid ? std::string() : valid.error().message);
        }
    }
    return 0;
}
