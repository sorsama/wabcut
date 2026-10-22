#pragma once

// Automatic face redaction (0.2, docs/masks.md "Redactions", docs/ai.md "Face redaction"): every face of a media item's
// span is found (the AI-9 subject detector, YuNet, on this computer), followed from sample to sample (ByteTrack, reset at
// shot cuts) and covered by one source-anchored redaction per face: an ellipse whose centre and size are keyframed at
// the samples, grown by a margin, so the face stays covered between them. Fail closed: a face seen once is redacted,
// and each redaction starts and ends half a sample interval beyond its first and last sighting.
//
//   redaction.detectFaces   "media" (+ "in" / "out", media time) or a "clip" (its media between its In and Out), the
//                           "style" (blur, mosaic, solid), "strength", samples per second and the smallest face. All the
//                           redactions are one undo step; dryRun previews them. Like every redaction.* command it is out
//                           of the assistant's reach (privacy is the user's call). The Source Monitor's Redactions
//                           popover runs it ("Redact faces").

#include <wabcut/ai/segment/ObjectMask.h>
#include <wabcut/model/Masks.h>

#include <expected>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::ai::segment {

struct FaceRedactionOptions {
    TimeRange span;                ///< media time (empty: the whole media)
    double samplesPerSecond = 8.0; ///< 1..30
    double minFaceHeight = 0.02;   ///< faces smaller than this share of the frame height are left out (0..0.5)
    double margin = 0.25;          ///< each box grown by this share of its size on every side
    RedactionStyle style = RedactionStyle::Blur;
    double strength = 0.05;
    std::string name = "Face";     ///< "Face 1", "Face 2", ...
};

struct FaceRedactionResult {
    std::vector<Redaction> redactions; ///< ids unassigned; the ops give them
    int samples = 0;                   ///< pictures looked at
    int detections = 0;                ///< faces seen over all samples
};

/// Finds and follows the faces of stream `videoStream` of `media` (SegmentServices::frames and ::detector; `shots`
/// resets the tracking at cuts, null: none). Errors: Unsupported (no detector or pictures), InvalidArgument, Cancelled,
/// the decoder's and the detector's. No faces: an empty result.
std::expected<FaceRedactionResult, Error> detectFaceRedactions(SegmentServices& services, analysis::AnalysisProvider* shots,
                                                               const MediaItem& media, int videoStream, const FaceRedactionOptions& options,
                                                               std::stop_token stop = {},
                                                               const std::function<bool(double, const std::string&)>& checkpoint = {});

/// Adds redaction.detectFaces.
void registerFaceRedactionCommands(command::CommandRegistry& registry);

} // namespace wabcut::ai::segment
