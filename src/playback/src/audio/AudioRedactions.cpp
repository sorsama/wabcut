#include <wabcut/playback/audio/AudioRedactions.h>

#include <wabcut/media/AudioDecoder.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/RedactionMedia.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <mutex>
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

// ---- 0.7 (card 102): the sound that replaces the bleep tone ----------------------------------------------------------

std::shared_ptr<const ReplacementSound> loadReplacementSound(const std::string& path) {
    namespace fs = std::filesystem;
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const ReplacementSound>> cache; // by file version; null = unreadable
    const fs::path file = pathFromUtf8(path);
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec) {
        return nullptr;
    }
    const auto time = fs::last_write_time(file, ec);
    const std::string key = path + "|" + std::to_string(size) + "|" +
                            std::to_string(ec ? 0 : static_cast<long long>(time.time_since_epoch().count()));
    const std::lock_guard lock(mutex);
    if (const auto it = cache.find(key); it != cache.end()) {
        return it->second;
    }
    std::shared_ptr<const ReplacementSound> loaded;
    if (auto decoder = media::AudioDecoder::open(path, -1, kRate48kHz, media::ChannelLayout::mono())) {
        auto sound = std::make_shared<ReplacementSound>();
        sound->key = key;
        constexpr std::size_t kChunk = 4096;
        const std::size_t limit = static_cast<std::size_t>(ReplacementSound::kMaxSeconds) * 48000;
        std::vector<float> buffer(kChunk);
        for (std::size_t done = 0; done < limit;) {
            const auto read = (*decoder)->read(samplesToTicks(static_cast<std::int64_t>(done), kRate48kHz), buffer);
            if (!read) {
                break;
            }
            const std::size_t frames = std::min(kChunk, read->leadingSilence + read->framesFromStream);
            sound->samples.insert(sound->samples.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(frames));
            if (read->endOfStream || frames < kChunk) {
                break;
            }
            done += kChunk;
        }
        // Trailing silence is not part of the sound.
        while (!sound->samples.empty() && sound->samples.back() == 0.0f) {
            sound->samples.pop_back();
        }
        if (!sound->samples.empty()) {
            loaded = std::move(sound);
        }
    }
    cache[key] = loaded;
    return loaded;
}

std::shared_ptr<const AudioRedactions> AudioRedactions::of(const ProjectState& project) {
    std::vector<Range> ranges;
    for (const Redaction& r : project.redactions) {
        if (!r.enabled || r.audio == RedactionAudio::None) {
            continue;
        }
        const MediaId root = redactionRoot(project, r.media); // a subclip's or stem's redaction marks its source
        // 0.7 (card 102): the chosen replacement sound, decoded once (cached by file version); the tone when it is missing.
        std::shared_ptr<const ReplacementSound> sound;
        if (r.audio == RedactionAudio::Bleep && r.sound.isValid()) {
            if (const ProjectAsset* asset = project.findAsset(r.sound);
                asset != nullptr && asset->kind == kAssetKindSound && !asset->locator.offline && !asset->locator.path.empty()) {
                sound = loadReplacementSound(asset->locator.path);
            }
        }
        const auto add = [&](const TimeRange& source) {
            if (!source.isEmpty()) {
                ranges.push_back({root, source, r.audio, r.exceptions, sound});
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
               return x.media == y.media && x.source == y.source && x.mode == y.mode && x.exceptions == y.exceptions && x.sound == y.sound;
           });
}

/// 0.7: the sound's sample `offset` ticks after the start of its range (linear between samples; 0 before the start and
/// after the end of the sound).
static float soundAt(const ReplacementSound& sound, std::int64_t offset) {
    if (offset < 0) {
        return 0.0f;
    }
    const long double position = static_cast<long double>(offset) * 48000.0L / static_cast<long double>(kTicksPerSecond);
    const auto i = static_cast<std::size_t>(position);
    if (i >= sound.samples.size()) {
        return 0.0f;
    }
    const float frac = static_cast<float>(position - static_cast<long double>(i));
    const float next = i + 1 < sound.samples.size() ? sound.samples[i + 1] : 0.0f;
    return sound.samples[i] + (next - sound.samples[i]) * frac;
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
        const ReplacementSound* sound = r.sound.get(); // 0.7: plays from the range's start instead of the tone
        for (std::int64_t k = 0; k < frames; ++k) {
            const std::int64_t s = s0 + step * k;
            const double g = coverage(s, a, b);
            if (g <= 0.0) {
                continue;
            }
            const auto keep = static_cast<float>(1.0 - g);
            const float add = !bleep ? 0.0f : sound != nullptr ? static_cast<float>(g) * soundAt(*sound, s - a)
                                                               : static_cast<float>(g) * bleepAt(Ticks{s});
            float* frame = dst + static_cast<std::size_t>(k) * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                frame[c] = frame[c] * keep + add;
            }
        }
    }
}

} // namespace wabcut::playback
