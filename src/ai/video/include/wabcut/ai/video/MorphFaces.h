#pragma once

// The face finder of the face-aware Morph Cut (0.7, playback::FaceFinder, docs/motion.md "Morph Cut and faces"): YuNet (OpenCV Zoo,
// MIT; the detector Auto Reframe uses, 0.2 MB) in the AI helper, asked for the faces of the two sides of a jump cut. The weights are
// never bundled: they are on this computer once Auto Reframe, AI Face Retouch or AI Eye Contact downloaded them (or the Morph Cut's
// own Download). Until then the finder finds nothing and Morph Cut is the plain flow.
//
// Detection runs on the luma of the picture the flow is estimated on (0..1 grey, at most 960 pixels on the long side; YuNet finds
// faces in grey pictures as it does in colour: 0.92 against 0.94 on the test portrait), sent to the helper at most 640 pixels on the
// long side. Nothing leaves this computer.

#include <wabcut/ai/ModelStore.h>
#include <wabcut/playback/video/MotionAnalysis.h>

#include <filesystem>
#include <memory>

namespace wabcut::ai::video {

/// The face detector's ModelStore entry (YuNet 2023mar), for downloads and for the Morph Cut's status.
const ModelInfo* faceDetectorModel();
/// Whether it is on this computer (`root` empty: the model store's current folder).
bool faceDetectorInstalled(const std::filesystem::path& root = {});

/// A finder over the AI helper (`helper` empty: wabcut-embed next to the running executable; `root` empty: the model store's folder,
/// read at every call, so a download made while the app runs is used at once). Its version() is "none" while the weights are not
/// installed and names their digest after, so a Morph Cut analysed before the download is analysed again after it.
std::shared_ptr<playback::FaceFinder> makeFaceFinder(const std::filesystem::path& root = {}, const std::filesystem::path& helper = {});

} // namespace wabcut::ai::video
