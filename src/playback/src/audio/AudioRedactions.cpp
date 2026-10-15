#include <wabcut/playback/audio/AudioRedactions.h>

#include <wabcut/model/RedactionMedia.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wabcut::playback {

namespace {

/// How much of the range [a, b) covers media time `s`: 1 inside, ramping to 0 over kRamp on both sides outside it.
double coverage(std::int64_t s, std::int64_t a, std::int64_t b) {
    const std::int64_t r = AudioRedactions::kRamp.value;
    if (s >= a && s < b) {
        return 1.0;
    }
    if (s < a) {
        return s >= a - r ? static_cast<double>(s - (a - r)) / static_cast<double>(r) : 0.0;
    }
    return s < b + r ? static_cast<double>(b + r - s) / static_cast<double>(r) : 0.0;
}

bool before(const AudioRedactions::Range& x, const AudioRedactions::Range& y) {
    return x.media != y.media ? x.media < y.media : x.source.start < y.source.start;
}

} // namespace

std::shared_ptr<const AudioRedactions> AudioRedactions::of(const ProjectState& project) {
    std::vector<Range> ranges;
    for (const Redaction& r : project.redactions) {
        if (!r.enabled || r.audio == RedactionAudio::None) {
            continue;
        }
        const MediaId root = redactionRoot(project, r.media); // a subclip's or stem's redaction marks its source
        const auto add = [&](const TimeRange& source) {
            if (!source.isEmpty()) {
                ranges.push_back({root, source, r.audio, r.exceptions});
            }
        };
        if (r.audioRanges.empty()) {
            add(r.source);
        }
        for (const TimeRange& words : r.audioRanges) {
            add(words);
        }
    }
    if (ranges.empty()) {
        return nullptr;
    }
    RootMap roots;
    for (const MediaPtr& item : project.media) {
        if (const MediaId root = redactionRoot(project, item->id); root != item->id) {
            roots.emplace_back(item->id, root);
        }
    }
    return std::make_shared<const AudioRedactions>(std::move(ranges), std::move(roots));
}

AudioRedactions::AudioRedactions(std::vector<Range> ranges, RootMap roots)
    : m_ranges(std::move(ranges)), m_roots(std::move(roots)) {
    std::stable_sort(m_ranges.begin(), m_ranges.end(), before);
    std::sort(m_roots.begin(), m_roots.end());
}

MediaId AudioRedactions::rootOf(MediaId media) const noexcept {
    const auto it = std::lower_bound(m_roots.begin(), m_roots.end(), media,
                                     [](const std::pair<MediaId, MediaId>& e, MediaId m) { return e.first < m; });
    return it != m_roots.end() && it->first == media ? it->second : media;
}

std::vector<MediaId> AudioRedactions::changedMedia(const AudioRedactions* before, const AudioRedactions* after) {
    // What a clip of `media` hears from a table: the ranges of its root.
    const auto heard = [](const AudioRedactions* table, MediaId media) {
        std::vector<Range> out;
        if (table == nullptr) {
            return out;
        }
        const MediaId root = table->rootOf(media);
        for (const Range& r : table->m_ranges) {
            if (r.media == root) {
                out.push_back(r);
            }
        }
        return out;
    };
    std::vector<MediaId> media;
    for (const AudioRedactions* table : {before, after}) {
        if (table == nullptr) {
            continue;
        }
        for (const Range& r : table->m_ranges) {
            media.push_back(r.media);
        }
        for (const auto& entry : table->m_roots) {
            media.push_back(entry.first); // subclips and stems hear their root's ranges
        }
    }
    std::sort(media.begin(), media.end());
    media.erase(std::unique(media.begin(), media.end()), media.end());
    std::erase_if(media, [&](MediaId m) { return heard(before, m) == heard(after, m); });
    return media;
}

bool AudioRedactions::operator==(const AudioRedactions& o) const {
    return m_roots == o.m_roots &&
           std::equal(m_ranges.begin(), m_ranges.end(), o.m_ranges.begin(), o.m_ranges.end(), [](const Range& x, const Range& y) {
               return x.media == y.media && x.source == y.source && x.mode == y.mode && x.exceptions == y.exceptions;
           });
}

float AudioRedactions::bleepAt(Ticks t) {
    // The phase of a whole number of cycles per second depends on t modulo one second only: exact in integers.
    constexpr std::int64_t second = kTicksPerSecond;
    const std::int64_t inSecond = ((t.value % second) + second) % second;
    const std::int64_t cycle = inSecond * static_cast<std::int64_t>(kBleepHz) % second;
    const double amplitude = std::pow(10.0, kBleepDbfs / 20.0);
    return static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * static_cast<double>(cycle) / static_cast<double>(second)));
}

void AudioRedactions::apply(MediaId media, ClipId clip, Ticks firstSource, std::int64_t step, std::int64_t frames, int channels,
                            float* dst, std::span<const ClipId> path) const {
    if (frames <= 0 || step == 0) {
        return;
    }
    const std::int64_t s0 = firstSource.value;
    const std::int64_t s1 = s0 + step * (frames - 1);
    const std::int64_t lo = std::min(s0, s1) - kRamp.value;
    const std::int64_t hi = std::max(s0, s1) + kRamp.value;
    const auto ch = static_cast<std::size_t>(channels);
    // The ranges of this media's redaction root near this stretch of media time.
    const MediaId root = rootOf(media);
    const auto first = std::lower_bound(m_ranges.begin(), m_ranges.end(), root,
                                        [](const Range& r, MediaId m) { return r.media < m; });
    for (auto it = first; it != m_ranges.end() && it->media == root; ++it) {
        const Range& r = *it;
        const std::int64_t a = r.source.start.value;
        const std::int64_t b = r.source.end().value;
        const auto excepts = [&](ClipId id) { return std::binary_search(r.exceptions.begin(), r.exceptions.end(), id); };
        if (b <= lo || a > hi || excepts(clip) || std::any_of(path.begin(), path.end(), excepts)) {
            continue;
        }
        const bool bleep = r.mode == RedactionAudio::Bleep;
        for (std::int64_t k = 0; k < frames; ++k) {
            const std::int64_t s = s0 + step * k;
            const double g = coverage(s, a, b);
            if (g <= 0.0) {
                continue;
            }
            const auto keep = static_cast<float>(1.0 - g);
            const float add = bleep ? static_cast<float>(g) * bleepAt(Ticks{s}) : 0.0f;
            float* frame = dst + static_cast<std::size_t>(k) * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                frame[c] = frame[c] * keep + add;
            }
        }
    }
}

} // namespace wabcut::playback
