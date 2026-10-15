#pragma once

// The offline audio takes clips can play instead of their source (A.4: Enhance Speech; model AudioTake), resolved
// from a project snapshot off the audio thread (the player's feeder when the media change, an export or an analysis
// when it starts) and shared, immutable, with the track renderers, like AudioRedactions.
//
// A take is usable when its file exists and it was rendered from the file the item has now (its source fingerprint
// has the item's size and sample hash, when both are known). Anything else is left out: the clip plays its source,
// and the UI shows the take missing (the effect's "render enhancement" state).
//
// TrackRenderer reads a usable take (AudioFeed::readTake) for clips with an enabled wabcut.audio.enhanceSpeech effect
// at the same source time as the source, blends the two by the effect's Strength on the parameter grid, and only then
// applies the clip's Volume and Panner and the redactions: a mute or a bleep always replaces the enhanced sound, it is
// never enhanced itself.

#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::playback {

class AudioTakes {
public:
    struct Take {
        MediaId media;
        int stream = 0;
        std::string kind;
        std::string path; ///< UTF-8
        std::uint64_t generation = 0;

        bool operator==(const Take&) const = default;
    };

    /// The usable takes of `project` (their files exist and match the items' files); null when there are none.
    static std::shared_ptr<const AudioTakes> of(const ProjectState& project);

    explicit AudioTakes(std::vector<Take> takes);

    /// The take of (media, stream, kind), or null.
    const Take* find(MediaId media, int stream, std::string_view kind) const;
    const std::vector<Take>& takes() const noexcept { return m_takes; }

    /// The media whose usable takes differ between two tables (either may be null: none), sorted.
    static std::vector<MediaId> changedMedia(const AudioTakes* before, const AudioTakes* after);

private:
    std::vector<Take> m_takes; ///< sorted by (media, stream, kind)
};

/// The enhanceSpeech effect of a stack that plays a take: the first enabled, evaluable wabcut.audio.enhanceSpeech
/// instance, or null.
const EffectInstance* enhanceSpeechEffect(const EffectStack& stack);

} // namespace wabcut::playback
