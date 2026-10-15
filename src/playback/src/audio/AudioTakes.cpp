#include <wabcut/playback/audio/AudioTakes.h>

#include <wabcut/model/Effects.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <tuple>

namespace wabcut::playback {

namespace {

constexpr std::string_view kEnhanceSpeechEffect = "wabcut.audio.enhanceSpeech";

auto keyOf(const AudioTakes::Take& t) {
    return std::tie(t.media, t.stream, t.kind);
}

bool usable(const MediaItem& item, const AudioTake& take) {
    if (take.path.empty()) {
        return false;
    }
    // Rendered from another file than the one the item has now (replaced or relinked): stale.
    if (take.source.isKnown() && item.locator.fingerprint.isKnown() && !take.source.sameContentAs(item.locator.fingerprint)) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(pathFromUtf8(take.path), ec);
}

} // namespace

std::shared_ptr<const AudioTakes> AudioTakes::of(const ProjectState& project) {
    std::vector<Take> takes;
    for (const MediaPtr& item : project.media) {
        for (const AudioTake& t : item->audioTakes) {
            if (usable(*item, t)) {
                takes.push_back(Take{item->id, t.stream, t.kind, t.path, t.generation});
            }
        }
    }
    if (takes.empty()) {
        return nullptr;
    }
    return std::make_shared<const AudioTakes>(std::move(takes));
}

AudioTakes::AudioTakes(std::vector<Take> takes) : m_takes(std::move(takes)) {
    std::ranges::sort(m_takes, [](const Take& a, const Take& b) { return keyOf(a) < keyOf(b); });
}

const AudioTakes::Take* AudioTakes::find(MediaId media, int stream, std::string_view kind) const {
    for (const Take& t : m_takes) {
        if (t.media == media && t.stream == stream && t.kind == kind) {
            return &t;
        }
    }
    return nullptr;
}

std::vector<MediaId> AudioTakes::changedMedia(const AudioTakes* before, const AudioTakes* after) {
    static const std::vector<Take> none;
    const std::vector<Take>& a = before != nullptr ? before->m_takes : none;
    const std::vector<Take>& b = after != nullptr ? after->m_takes : none;
    std::vector<MediaId> media;
    for (const Take& t : a) {
        media.push_back(t.media);
    }
    for (const Take& t : b) {
        media.push_back(t.media);
    }
    std::ranges::sort(media);
    media.erase(std::unique(media.begin(), media.end()), media.end());
    std::erase_if(media, [&](MediaId m) {
        const auto of = [&](const std::vector<Take>& list) {
            std::vector<Take> out;
            std::ranges::copy_if(list, std::back_inserter(out), [&](const Take& t) { return t.media == m; });
            return out;
        };
        return of(a) == of(b);
    });
    return media;
}

const EffectInstance* enhanceSpeechEffect(const EffectStack& stack) {
    for (const EffectInstance& e : stack) {
        if (e.enabled && e.effect == kEnhanceSpeechEffect && isEvaluable(e, EffectKind::Audio)) {
            return &e;
        }
    }
    return nullptr;
}

} // namespace wabcut::playback
