#pragma once

// AI object mattes in the compositor (AI-14, docs/masks.md "Object masks"). A matte mask (MaskShape::Matte) shows, at
// each frame, an alpha matte that a segmentation model made from the media (kept in the analysis cache). The render
// code never reads that cache: whoever owns it installs a MatteSource (the app, wabcut-cli and export install
// src/ai/segment's), which turns the matte at a media time into a signed-distance field over the frame. A matte
// therefore enters the mask pipeline as any shape's distance texture (render/Mask.h, step 1): feather, expansion (a
// choke when negative), opacity, invert, the combination, the coverage cache and the GPU / CPU parity are the same as
// for vector shapes.
//
// Fail closed: for redactions (MatteUse::Redaction) a source returns, at a frame without a matte, the nearest known
// matte grown with the time gap (MatteField::fallback); with no matte known at all it returns null and the redaction
// covers the mask's bounding box grown by a quarter (maskSpecAt). A redaction never covers nothing.

#include <wabcut/model/Masks.h>
#include <wabcut/model/Time.h>
#include <wabcut/render/Geometry.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace wabcut::render {

/// A matte as a distance field over the frame [0, 1]^2.
struct MatteField {
    int width = 0;  ///< texels
    int height = 0;
    /// Signed distance to the matte's edge (its 0.5 level), in frame heights, positive inside; row-major, top row first.
    std::vector<float> distance;
    /// Content identity (the track, the frame, how it was made): the distance texture's cache key. Never 0.
    std::uint64_t identity = 1;
    /// A redaction's stand-in at a frame without a matte: the nearest one, grown.
    bool fallback = false;
    /// The distances as half floats (toHalf), made once by the source with the field: the compositor uploads these
    /// instead of converting every texel on the render thread. Empty = the compositor converts.
    std::vector<std::uint16_t> half;
};

enum class MatteUse : std::uint8_t {
    Effect,    ///< effect and opacity masks: the matte at the frame (or the nearest within a frame), else none
    Redaction, ///< fail closed: never none while any matte of the track is known
};

class MatteSource {
public:
    virtual ~MatteSource() = default;
    /// The matte of `matte` at `mediaTime` (media time of `matte.media`) as a distance field, `frameAspect` = the frame's
    /// displayed width / height. Null when there is none. Called from playback and export threads: thread-safe.
    virtual std::shared_ptr<const MatteField> fieldAt(const MaskMatte& matte, Ticks mediaTime, double frameAspect, MatteUse use) = 0;
    /// The content revision of `matte`'s track: it changes whenever the track's mattes change (refined, propagated
    /// again, made anew under the same identity). Pictures made from mattes (thumbnails) key on it. 0 = unknown.
    virtual std::uint64_t revision(const MaskMatte& matte) {
        (void)matte;
        return 0;
    }
};

/// The process's matte source (none by default: matte masks then show their bounding box). Thread-safe.
void setMatteSource(std::shared_ptr<MatteSource> source);
std::shared_ptr<MatteSource> matteSource();

/// The field's distance at `p` (normalised frame coordinates), bilinear between texel centres, clamped at the edges.
/// An unusable field (usableField) covers nothing here: maskSpecAt() and the CPU coverage treat it as no matte.
double matteDistance(const MatteField& field, Vec2 p);

/// A field the renderer can use: a positive size, as many distances as texels, all finite.
bool usableField(const MatteField& field);

/// IEEE 754 half-precision bits of `value` (round to nearest even): MatteField::half.
std::uint16_t toHalf(float value);

} // namespace wabcut::render
