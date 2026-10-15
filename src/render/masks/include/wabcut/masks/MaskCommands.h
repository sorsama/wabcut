#pragma once

// The mask, tracking and redaction commands of the catalog (I.6, docs/masks.md "Commands"), registered into
// editingCommands() so wabcut-cli, the MCP server, the agent and the app serve them. Every edit is one undo step (a
// dry run previews it):
//
//   mask.add            add an ellipse, rectangle or Bezier-path mask to an effect of a clip (local corrections), or
//                       to the clip's opacity mask (cut-outs)
//   mask.update         change a mask: name, shape, mode, invert, on/off, parameters (constants or keyframes), path
//   mask.remove
//   track.mask          a job: track a mask through the clip's media frame by frame (forward or backward, to the end
//                       or a frame count, stopping where the picture is lost); its transform.* parameters get
//                       ordinary keyframes
//   track.transform     a job: track a region and key a clip's Motion - another clip follows it, or the clip is
//                       stabilised on it
//   redaction.add       mark a region of a media item in SOURCE time (blur, mosaic, solid; optional mute/bleep of
//                       word ranges): every use of that range in every sequence is redacted
//   redaction.update / redaction.remove
//   redaction.override  "don't redact here" for one clip (and its linked partners), or back on
//   redaction.list      the project's redactions (read)
//
// track.* need the host's TrackingServices (pictures of the media, at a working resolution; proxies are fine); without
// them they fail with InvalidRequest. TrackingServices::detect() is the seam for a face / person detector (AI-9): no
// detector ships yet, so "detect" requests fail with Unsupported.

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
#include <wabcut/masks/Tracker.h>
#include <wabcut/model/Masks.h>
#include <wabcut/model/ProjectState.h>

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::masks {

/// One picture of a media item for the tracker.
struct TrackingFrame {
    GrayImage luma;  ///< square pixels, the whole frame (display aspect = width / height)
    Ticks start;     ///< the decoded frame's media time span [start, start + duration)
    Ticks duration;
};

/// Where a running track is (the UI's progress bar and confidence strip).
struct TrackProgress {
    int frame = 0;  ///< frames tracked so far
    int total = 0;  ///< frames it will track at most
    Ticks time;     ///< media time of the latest frame
    double confidence = 1.0;
    bool lost = false;
};

/// A region a detector found, normalised frame coordinates.
struct DetectedRegion {
    std::string kind; ///< "face", "person"
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    double score = 0.0;
};

class TrackingServices : public command::CommandServices {
public:
    /// The frame of `media`'s video stream `stream` (index into its info.video) that contains media time `time`, at
    /// most `maxWidth` pixels wide. NotFound past the last frame.
    virtual std::expected<TrackingFrame, Error> frame(const ProjectState& state, MediaId media, int stream, Ticks time,
                                                      int maxWidth) = 0;
    /// Called after every tracked frame (any thread the command runs on). Default: nothing.
    virtual void progress(const TrackProgress& progress);
    /// Seam for a face / person detector (AI-9). Default: Unsupported.
    virtual std::expected<std::vector<DetectedRegion>, Error> detect(const GrayImage& picture, std::string_view kind);
};

/// Adds the commands above.
void registerMaskCommands(command::CommandRegistry& registry);

// ---- Helpers shared with the editor and the app ---------------------------------------------------------------

/// transform.* values whose maskTransformAt() is `affine`, keeping the anchor (anchorX, anchorY); rotation
/// continues `previousRotation` (no 360-degree jumps). A matrix has two such decompositions, scaleX > 0 or scaleX < 0
/// (half a turn more, both scales negated); `flippedX` picks the second, so a flipped mask's keys stay flipped and
/// interpolate instead of collapsing through a zero scale.
struct MaskTransformValues {
    double x = 0.0;
    double y = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
    double rotation = 0.0;
    double skew = 0.0;
};
MaskTransformValues maskTransformValues(const MaskAffine& affine, double anchorX, double anchorY, double aspect,
                                        double previousRotation = 0.0, bool flippedX = false);

/// The mask's outline at a curve time (transform applied) as a closed polygon, normalised frame coordinates.
std::vector<Point2> maskPolygonAt(const Mask& mask, Ticks curveTime, double aspect);

} // namespace wabcut::masks
