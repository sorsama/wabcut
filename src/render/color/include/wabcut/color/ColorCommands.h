#pragma once

// The colour commands of the catalog (I.5, docs/color.md "Commands"), registered into editingCommands() so
// wabcut-cli, the MCP server, the agent and the app serve them. Every edit is one undo step (a dry run previews it):
//
//   color.grade.set    set grade parameters (partial), stage switches, curves and LUTs on clips; adds a grade if needed
//   color.grade.get    a clip's grade at a time: every parameter, curve, LUT and its stage switches
//   color.auto         Auto: white balance and levels from the clip's own picture (editable Basic parameters)
//   color.match        Color Match: the clip's grade fitted so it looks like a reference frame
//   color.matchScene   match every clip of a scene (C.5 shot pieces of one take), a track or a list to a reference clip
//   lut.import         add a .cube file to the project (a LUT asset) and return its reference
//   lut.list           the built-in looks and input LUTs and the project's LUT assets
//   cdl.export         write clips' colour wheels as an ASC CDL (.cc, .ccc or .cdl)
//   cdl.import         set clips' colour wheels from an ASC CDL file
//   scopes.read        scope statistics of the picture at a time (read only): exposure, clipping, colour cast
//
// The commands that look at pixels (auto, match, matchScene, scopes.read) need the host's ColorServices; without
// them they fail with InvalidRequest.

#include <wabcut/color/ColorTools.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>

#include <expected>

namespace wabcut::color {

/// What a frame request asks the host to render.
struct FrameRequest {
    Ticks time;                  ///< sequence time
    ClipId solo;                 ///< only this clip (invalid = the whole sequence as shown)
    EffectInstanceId stopBefore; ///< with `solo`: the clip's effect stack cut before this instance (the grade's input)
    int maxWidth = 640;          ///< the host may render smaller than the canvas
};

/// Pictures for the colour commands, provided by the host (EnginePolicy::services, McpServerOptions::services).
class ColorServices : public command::CommandServices {
public:
    /// Display-referred straight RGB + coverage alpha of the request.
    virtual std::expected<ColorImage, Error> frame(const ProjectState& state, SequenceId sequence, const FrameRequest& request) = 0;
};

/// 0.2: the faces on a picture, for Color Match's face priority ("faceDetection", Premiere's Face Detection), provided by
/// the host when a face detector is installed (the AI subject detector, ai::vision::detectorFaceFinder). Without it a
/// match that asks for face detection fails with the reason.
class FaceFinder : public command::CommandServices {
public:
    /// The faces on `image` (display-referred), as normalised boxes. Errors: the detector's.
    virtual std::expected<std::vector<FaceBox>, Error> faces(const ColorImage& image) = 0;
};

/// Adds the commands above.
void registerColorCommands(command::CommandRegistry& registry);

/// The first enabled-or-not grade instance of a clip's stack (null when it has none).
const EffectInstance* gradeOf(const Clip& clip, EffectInstanceId preferred = {});

/// The clips of the "scene" around `reference` (C.5): the reference and its neighbours on the same track that touch
/// it without a gap and come from the same media item - the pieces clip.splitAtShots cut one take into.
std::vector<ClipId> sceneClips(const Sequence& sequence, ClipId reference);

} // namespace wabcut::color
