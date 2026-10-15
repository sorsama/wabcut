// Fuzz harness: the C.15 parity pack's untrusted input, chosen by the first byte. 0: a preview cache index
// ("previews.json", PreviewCache::parseIndex): what parses has plain file names, valid hex hashes and ranges, and comes
// back the same through its JSON. 1: a project (.wabcut JSON) with blend modes, synthetic items, adjustment layers and
// keyers: what loads resolves to composite plans, a render bar and content hashes without crashing, at a few times.

#include "FuzzSupport.h"

#include <wabcut/model/PreviewCache.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/Resolve.h>
#include <wabcut/model/Synthetic.h>

#include <cmath>

using namespace wabcut;

namespace {

void checkPlan(const CompositePlan& plan, int depth) {
    WABCUT_FUZZ_CHECK(depth <= static_cast<int>(kMaxNestingDepth) + 2, "a composite plan nests too deep");
    for (const CompositeStep& s : plan.steps) {
        WABCUT_FUZZ_CHECK(s.clip != nullptr, "a step without a clip");
        WABCUT_FUZZ_CHECK(std::isfinite(s.opacity) && s.opacity >= 0.0 && s.opacity <= 1.0, "a step's opacity");
        if (s.nested) {
            checkPlan(*s.nested, depth + 1);
        }
        if (s.matte && s.matte->plan) {
            checkPlan(*s.matte->plan, depth + 1);
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const std::string_view text = fuzz::text(data + 1, size - 1);
    if (data[0] % 2 == 0) {
        const auto entries = PreviewCache::parseIndex(text);
        if (!entries) {
            return 0;
        }
        for (const PreviewEntry& e : *entries) {
            WABCUT_FUZZ_CHECK(PreviewCache::isPlainFileName(e.file), "an index entry with a path in its file name");
            WABCUT_FUZZ_CHECK(e.range.duration.value > 0 && e.range.start.value >= 0, "an index entry with a bad range");
        }
        const auto again = PreviewCache::parseIndex(PreviewCache::indexJson(*entries));
        WABCUT_FUZZ_CHECK(again && *again == *entries, again ? std::string("index round trip") : again.error().message);
        return 0;
    }
    const auto project = io::fromJson(text);
    if (!project || !validate(*project, Validation::Strict)) {
        return 0;
    }
    std::size_t sequences = 0;
    for (const SequencePtr& seq : project->sequences) {
        if (sequences++ == 3) {
            break; // a few per input keep each run short
        }
        const Ticks end = seq->duration;
        for (const Ticks t : {Ticks{}, Ticks{end.value / 2}, end - Ticks{1}, Ticks{-1}}) {
            checkPlan(resolveComposite(*project, seq->id, t), 0);
        }
        const TimeRange span{Ticks{}, std::max(end, Ticks{1})};
        for (const RenderSegment& s : renderSegments(*project, seq->id, span)) {
            WABCUT_FUZZ_CHECK(std::isfinite(s.cost) && s.cost >= 0.0, "a segment cost");
            WABCUT_FUZZ_CHECK(s.range.duration.value > 0, "an empty segment");
        }
        WABCUT_FUZZ_CHECK(renderContentHash(*project, seq->id, span).size() == 16, "a content hash");
    }
    for (const MediaPtr& m : project->media) {
        if (syntheticOf(*m) || isAdjustmentLayer(*m)) {
            WABCUT_FUZZ_CHECK(!syntheticProblem(*m), syntheticProblem(*m).value_or("")); // validate() checked it
        }
    }
    return 0;
}
