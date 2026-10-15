#pragma once

// The render::MatteSource over the analysis cache (AI-14, docs/masks.md "Object masks"): the source the app,
// wabcut-cli and export install (render::setMatteSource), so the compositor gets a matte mask's matte at a media time as
// a signed-distance field. Full builds only (it implements a wabcut_render interface).
//
// Effect and opacity masks: the matte at the frame (within three quarters of a frame), else none (the mask's bounding
// box stands in, render/Mask.h). Redactions fail closed: the matte at the frame when the model saw the object there,
// else the nearest present matte grown with the gap (1 % of the frame height + 15 % per second, at most half the
// frame); with no present matte at all, none (the redaction covers its bounding box grown by a quarter).
//
// Fields are keyed by the track's content revision and the frame's time (a refine or a second propagation under the
// same identity never serves an old field), capped at 256 texels on the longer side and carry their half floats, so the
// compositor uploads without converting (AI-14 review); lookups are O(log frames) plus an O(1) LRU.

#include <wabcut/ai/segment/Matte.h>
#include <wabcut/render/Matte.h>

#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wabcut::ai::segment {

/// Thread-safe; tracks (8) and fields (`fields`) kept in small LRUs.
class CacheMatteSource final : public render::MatteSource {
public:
    explicit CacheMatteSource(std::filesystem::path cacheRoot, std::size_t fields = 48);
    std::shared_ptr<const render::MatteField> fieldAt(const MaskMatte& matte, Ticks mediaTime, double frameAspect,
                                                      render::MatteUse use) override;
    /// The track's content revision (a hash of its frames): changes when its mattes change under the same identity.
    std::uint64_t revision(const MaskMatte& matte) override;
    /// Makes a freshly made track known without reading it back.
    void remember(std::shared_ptr<const MatteTrack> track);
    /// Fields made so far (tests: the LRU works).
    std::size_t fieldsMade() const;
    const std::filesystem::path& root() const { return m_store.root(); }

private:
    /// A track as it is known here (null track: none on this computer), its revision and its present frames' indices.
    struct TrackEntry {
        std::shared_ptr<const MatteTrack> track;
        std::uint64_t revision = 0;
        std::vector<std::size_t> present;
    };
    static std::shared_ptr<const TrackEntry> makeEntry(std::shared_ptr<const MatteTrack> track);
    std::shared_ptr<const TrackEntry> entry(const std::string& identity);
    std::shared_ptr<const render::MatteField> field(const TrackEntry& known, std::size_t index, double grow);

    MatteStore m_store;
    std::size_t m_capacity;
    mutable std::mutex m_mutex;
    std::map<std::string, std::shared_ptr<const TrackEntry>> m_tracks;
    std::list<std::string> m_trackOrder;
    using FieldList = std::list<std::pair<std::string, std::shared_ptr<const render::MatteField>>>;
    FieldList m_fieldLru;
    std::unordered_map<std::string, FieldList::iterator> m_fieldIndex;
    std::size_t m_made = 0;
};

} // namespace wabcut::ai::segment
