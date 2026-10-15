#pragma once

// The host's Source Monitor, for the source commands (CatalogSource.cpp: source.*, edit.insertFromSource /
// overwriteFromSource, sequence.setPatches / setTargets / matchFrame / reverseMatchFrame).
//
// Source patching (Premiere's source track indicators: which stream of the loaded source goes to which track,
// and which are switched off) is view state of the host, not project data. A host with a Source Monitor (the app)
// provides it through EnginePolicy::services, so source edits without explicit `patches` use what its track headers
// show. Hosts without one (wabcut-cli, the MCP server) leave it out, and the commands use Premiere's default
// patching (defaultSourcePatches). Executors only read from it: what a command shows or stores in the host (the
// source to load, the patching to keep, the playhead to move) comes back in its `data`, so dry runs never act.

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/Clip.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <string>
#include <vector>

namespace wabcut::command {

/// One source track indicator: a stream of the source, the track it is patched to, and whether it takes part in
/// edits (Premiere: Alt-click switches an indicator off).
struct SourcePatchSetting {
    StreamRef source;
    TrackId destination;
    bool enabled = true;
    /// A.10, audio streams: the name of the track the stream goes to (edit::SourcePatch::newTrack): without a
    /// `destination` a new audio track of that name, else the name an unnamed destination is given.
    std::string newTrack;

    bool operator==(const SourcePatchSetting&) const = default;
};

class SourceMonitorServices : public CommandServices {
public:
    /// The patching the host shows for `media` in `sequence`; empty when it has none for them (the default
    /// applies).
    virtual std::vector<SourcePatchSetting> sourcePatching(SequenceId sequence, MediaId media) const = 0;
};

/// The frame rate source times are shown and parsed in (source commands, the Source Monitor's timecode): the
/// media's video rate, else the first sequence's, else 30 fps.
Rate sourceTimecodeRate(const ProjectState& state, const MediaItem& media);
/// "V1" for video stream 0, "A2" for audio stream 1 (Premiere's source track indicators).
std::string sourceStreamLabel(StreamRef stream);

} // namespace wabcut::command
