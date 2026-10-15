#pragma once

// Pictures for the colour commands and tools (I.5): color::ColorServices on the export renderer, so Auto, Color
// Match and scopes.read look at exactly what an export would contain.
//
//   * the sequence at a time, as shown;
//   * one clip alone (every other clip of the sequence left out, captions off), optionally with its effect stack cut
//     before a grade instance - the grade's input. Its coverage (the clip's transformed frame on the canvas) is the
//     alpha, so letterbox black never counts as part of the picture.

#include <wabcut/color/ColorCommands.h>
#include <wabcut/editor/Recovery.h>
#include <wabcut/model/ProjectState.h>

#include <expected>
#include <filesystem>

namespace wabcut::editor {

/// The project showing only `clip` in its sequence (other clips removed), its effect stack cut before `stopBefore`
/// (kept whole when invalid). Media and assets unchanged.
ProjectState soloClipState(const ProjectState& state, SequenceId sequence, ClipId clip, EffectInstanceId stopBefore = {});

/// One frame of `sequence` at `time`, `width` pixels wide (height by the canvas aspect), RGBA8 display-referred.
std::expected<PosterImage, Error> renderSequenceFrame(const ProjectState& state, SequenceId sequence, Ticks time, int width,
                                                      const std::filesystem::path& projectFile = {});

class ColorFrameService final : public color::ColorServices {
public:
    explicit ColorFrameService(std::filesystem::path projectFile = {}) : m_projectFile(std::move(projectFile)) {}
    std::expected<color::ColorImage, Error> frame(const ProjectState& state, SequenceId sequence,
                                                  const color::FrameRequest& request) override;

private:
    std::filesystem::path m_projectFile;
};

} // namespace wabcut::editor
