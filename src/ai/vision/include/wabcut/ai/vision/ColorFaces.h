#pragma once

// 0.2 Color Match face priority (docs/color.md "Auto and Color Match"): color::FaceFinder over the AI subject detector
// (YuNet faces in wabcut-embed, AI-9), so color.match / color.matchScene "faceDetection" (Premiere's Face Detection)
// weigh the faces' skin in both pictures. The pictures go to the detector at 640 px on their longer side; nothing leaves
// this computer.

#include <wabcut/ai/vision/Subjects.h>
#include <wabcut/color/ColorCommands.h>

#include <filesystem>
#include <memory>

namespace wabcut::ai::vision {

/// The faces `detector` finds on colour pictures. Errors: the detector's.
std::shared_ptr<color::FaceFinder> detectorFaceFinder(std::shared_ptr<SubjectDetector> detector);

/// The built-in face finder when the subject detectors are downloaded (Settings > AI > Local models), else null.
/// `modelRoot` empty = the model store's default; `helper` empty = wabcut-embed next to the running executable.
std::shared_ptr<color::FaceFinder> installedFaceFinder(const std::filesystem::path& modelRoot = {}, const std::filesystem::path& helper = {});

/// `image` (display-referred float RGBA) as the detector's 8-bit RGB picture, `longSide` on its longer side.
Image detectorPicture(const color::ColorImage& image, int longSide = 640);

} // namespace wabcut::ai::vision
