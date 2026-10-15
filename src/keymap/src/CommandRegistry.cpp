#include <wabcut/keymap/CommandRegistry.h>

#include <array>

namespace wabcut::keymap {
namespace {

// Built-in commands. Keys are not here: they live in keymaps/premiere-*.json (one table per OS).
// Columns: id, scope, group, display name, Premiere command name, .kys context, .kys command name,
// auto-repeat. The .kys identities were verified against Premiere's default Windows and macOS key sets
// (docs/keymap.md).
// clang-format off
constexpr std::array kBuiltinCommands{
    // File
    CommandInfo{"file.newProject", Scope::Application, "File", "New Project…", "New Project …", "context.global", "cmd.file.new.project", false},
    CommandInfo{"file.newSequence", Scope::Application, "File", "New Sequence…", "New Sequence…", "context.global", "cmd.file.new.sequence", false},
    CommandInfo{"file.openProject", Scope::Application, "File", "Open Project…", "Open Project...", "context.global", "cmd.file.openproject", false},
    CommandInfo{"file.close", Scope::Application, "File", "Close", "Close", "context.global", "cmd.file.closepanel", false},
    CommandInfo{"file.closeProject", Scope::Application, "File", "Close Project", "Close Project", "context.global", "cmd.file.close", false},
    CommandInfo{"file.save", Scope::Application, "File", "Save", "Save", "context.global", "cmd.file.save", false},
    CommandInfo{"file.saveAs", Scope::Application, "File", "Save As…", "Save As...", "context.global", "cmd.file.saveas", false},
    CommandInfo{"file.saveCopy", Scope::Application, "File", "Save a Copy…", "Save a Copy...", "context.global", "cmd.file.savecopy", false},
    CommandInfo{"file.importFromMediaBrowser", Scope::Application, "File", "Import from Media Browser", "Import from Media Browser", "context.global", "cmd.file.importfrombrowser", false},
    CommandInfo{"file.import", Scope::Application, "File", "Import…", "Import...", "context.global", "cmd.file.import", false},
    CommandInfo{"file.exportMedia", Scope::Application, "File", "Export Media…", "Export Media", "context.global", "cmd.file.export.movie", false},
    CommandInfo{"file.sendToMediaEncoder", Scope::Application, "File", "Send to Media Encoder", "Send to Media Encoder", "context.global", "cmd.file.export.sendtoqueue", false},
    CommandInfo{"file.getProperties", Scope::Application, "File", "Get Media File Properties for Selection…", "Get Media File Properties for Selection...", "context.global", "cmd.file.properties.selection", false},
    CommandInfo{"file.exit", Scope::Application, "File", "Exit", "Exit", "context.global", "cmd.file.exit", false},
    // Edit
    CommandInfo{"edit.undo", Scope::Application, "Edit", "Undo", "Undo", "context.global", "cmd.edit.undo", true},
    CommandInfo{"edit.redo", Scope::Application, "Edit", "Redo", "Redo", "context.global", "cmd.edit.redo", true},
    CommandInfo{"edit.cut", Scope::Application, "Edit", "Cut", "Cut", "context.global", "cmd.edit.cut", false},
    CommandInfo{"edit.copy", Scope::Application, "Edit", "Copy", "Copy", "context.global", "cmd.edit.copy", false},
    CommandInfo{"edit.paste", Scope::Application, "Edit", "Paste", "Paste", "context.global", "cmd.edit.paste", false},
    CommandInfo{"edit.pasteInsert", Scope::Application, "Edit", "Paste Insert", "Paste Insert", "context.global", "cmd.edit.pasteinsert", false},
    CommandInfo{"edit.pasteAttributes", Scope::Application, "Edit", "Paste Attributes…", "Paste Attributes", "context.global", "cmd.edit.pasteattributes", false},
    // C.11 / M5m: no default key and no item in Premiere's shipped key sets (docs/keymap.md).
    CommandInfo{"edit.removeAttributes", Scope::Application, "Edit", "Remove Attributes…", "Remove Attributes", "", "", false},
    CommandInfo{"edit.clear", Scope::Application, "Edit", "Clear", "Clear", "context.global", "cmd.edit.clear", false},
    CommandInfo{"edit.rippleDelete", Scope::Application, "Edit", "Ripple Delete", "Ripple Delete", "context.global", "cmd.edit.rippledelete", false},
    CommandInfo{"edit.duplicate", Scope::Application, "Edit", "Duplicate", "Duplicate", "context.global", "cmd.edit.duplicate", false},
    CommandInfo{"edit.selectAll", Scope::Application, "Edit", "Select All", "Select All", "context.global", "cmd.edit.selectall", false},
    CommandInfo{"edit.deselectAll", Scope::Application, "Edit", "Deselect All", "Deselect All", "context.global", "cmd.edit.deselectall", false},
    CommandInfo{"edit.find", Scope::Application, "Edit", "Find…", "Find...", "context.global", "cmd.edit.find", false},
    CommandInfo{"edit.editOriginal", Scope::Application, "Edit", "Edit Original", "Edit Original", "context.global", "cmd.edit.editoriginal", false},
    CommandInfo{"edit.keyboardShortcuts", Scope::Application, "Edit", "Keyboard Shortcuts…", "Keyboard Shortcuts", "context.global", "cmd.edit.keyboardshortcuts", false},
    // Clip
    CommandInfo{"clip.makeSubclip", Scope::Application, "Clip", "Make Subclip…", "Make Subclip...", "context.global", "cmd.clip.makesubclip", false},
    CommandInfo{"clip.audioChannels", Scope::Application, "Clip", "Audio Channels…", "Audio Channels...", "context.global", "cmd.clip.audiooptions.sourcechannelmappings", false},
    CommandInfo{"clip.audioGain", Scope::Application, "Clip", "Audio Gain…", "Audio Gain…", "context.global", "cmd.clip.audiooptions.gain", false},
    CommandInfo{"clip.speedDuration", Scope::Application, "Clip", "Speed/Duration…", "Speed/Duration...", "context.global", "cmd.clip.speed", false},
    CommandInfo{"clip.insert", Scope::Application, "Clip", "Insert", "Insert", "context.global", "cmd.clip.insert", false},
    CommandInfo{"clip.overwrite", Scope::Application, "Clip", "Overwrite", "Overwrite", "context.global", "cmd.clip.overlay", false},
    CommandInfo{"clip.enable", Scope::Application, "Clip", "Enable", "Enable", "context.global", "cmd.clip.enable", false},
    CommandInfo{"clip.link", Scope::Application, "Clip", "Link", "Link", "context.global", "cmd.clip.linkaudioandvideo", false},
    CommandInfo{"clip.group", Scope::Application, "Clip", "Group", "Group", "context.global", "cmd.clip.group", false},
    CommandInfo{"clip.ungroup", Scope::Application, "Clip", "Ungroup", "Ungroup", "context.global", "cmd.clip.ungroup", false},
    // M5o (gaming): the Moments presets on the clip at the playhead. Wabcut's, with no default key.
    CommandInfo{"moments.slowMo", Scope::Application, "Clip", "Slow-mo at Playhead", "", "", "", false},
    CommandInfo{"moments.punchIn", Scope::Application, "Clip", "Punch-in at Playhead", "", "", "", false},
    // Sequence
    CommandInfo{"sequence.renderInToOut", Scope::Application, "Sequence", "Render Effects In to Out", "Render Effects In to Out", "context.global", "cmd.sequence.preview", false},
    CommandInfo{"sequence.matchFrame", Scope::Application, "Sequence", "Match Frame", "Match Frame", "context.global", "cmd.sequence.matchframe", false},
    CommandInfo{"sequence.reverseMatchFrame", Scope::Application, "Sequence", "Reverse Match Frame", "Reverse Match Frame", "context.global", "cmd.sequence.reversematchframe", false},
    CommandInfo{"sequence.addEdit", Scope::Application, "Sequence", "Add Edit", "Add Edit", "context.global", "cmd.sequence.razorateditline", false},
    CommandInfo{"sequence.addEditToAllTracks", Scope::Application, "Sequence", "Add Edit to All Tracks", "Add Edit to All Tracks", "context.global", "cmd.sequence.razorateditline.all", false},
    CommandInfo{"sequence.trimEdit", Scope::Application, "Sequence", "Trim Edit", "Trim Edit", "context.global", "cmd.placeholder", false},
    CommandInfo{"sequence.toggleTrimType", Scope::Application, "Sequence", "Toggle Trim Type", "Toggle Trim Type", "context.global", "cmd.sequence.toggletrimtype", false},
    CommandInfo{"sequence.trimBackward", Scope::Application, "Sequence", "Trim Backward", "Trim Backward", "context.global", "cmd.sequence.trimbackward", true},
    CommandInfo{"sequence.trimForward", Scope::Application, "Sequence", "Trim Forward", "Trim Forward", "context.global", "cmd.sequence.trimforward", true},
    CommandInfo{"sequence.trimBackwardMany", Scope::Application, "Sequence", "Trim Backward Many", "Trim Backward Many", "context.global", "cmd.sequence.trimbackwardmany", true},
    CommandInfo{"sequence.trimForwardMany", Scope::Application, "Sequence", "Trim Forward Many", "Trim Forward Many", "context.global", "cmd.sequence.trimforwardmany", true},
    CommandInfo{"sequence.extendEditToPlayhead", Scope::Application, "Sequence", "Extend Selected Edit to Playhead", "Extend Selected Edit to Playhead", "context.global", "cmd.sequence.extendselectededittoplayhead", false},
    CommandInfo{"sequence.applyVideoTransition", Scope::Application, "Sequence", "Apply Video Transition", "Apply Video Transition", "context.global", "cmd.sequence.applydefaultvideotransition", false},
    CommandInfo{"sequence.applyAudioTransition", Scope::Application, "Sequence", "Apply Audio Transition", "Apply Audio Transition", "context.global", "cmd.sequence.applydefaultaudiotransition", false},
    CommandInfo{"sequence.applyDefaultTransitions", Scope::Application, "Sequence", "Apply Default Transitions to Selection", "Apply Default Transitions to Selection", "context.global", "cmd.sequence.applydefaulttransitions", false},
    CommandInfo{"sequence.lift", Scope::Application, "Sequence", "Lift", "Lift", "context.global", "cmd.sequence.lift", false},
    CommandInfo{"sequence.extract", Scope::Application, "Sequence", "Extract", "Extract", "context.global", "cmd.sequence.extract", false},
    CommandInfo{"sequence.zoomIn", Scope::Application, "Sequence", "Zoom In", "Zoom In", "context.global", "cmd.zoom.in", true},
    CommandInfo{"sequence.zoomOut", Scope::Application, "Sequence", "Zoom Out", "Zoom Out", "context.global", "cmd.zoom.out", true},
    CommandInfo{"sequence.zoomToSequence", Scope::Application, "Sequence", "Zoom to Sequence", "Zoom to Sequence", "context.global", "cmd.tlnav.zoomto.sequence", false},
    CommandInfo{"sequence.nextInSequence", Scope::Application, "Sequence", "Next in Sequence", "Next in Sequence", "context.global", "cmd.sequence.findnextsequencegap", false},
    CommandInfo{"sequence.previousInSequence", Scope::Application, "Sequence", "Previous in Sequence", "Previous in Sequence", "context.global", "cmd.sequence.findprevioussequencegap", false},
    CommandInfo{"sequence.snap", Scope::Application, "Sequence", "Snap in Timeline", "Snap in Timeline", "context.global", "cmd.sequence.snap", false},
    CommandInfo{"sequence.makeSubsequence", Scope::Application, "Sequence", "Make Subsequence", "Make Subsequence", "context.global", "cmd.sequence.makesubsequence", false},
    CommandInfo{"sequence.addCaptionTrack", Scope::Application, "Sequence", "Add New Caption Track", "Add new caption track", "context.global", "cmd.caption.add.track", false},
    CommandInfo{"sequence.addCaptionAtPlayhead", Scope::Application, "Sequence", "Add Caption at Playhead", "Add caption at playhead", "context.global", "cmd.caption.add.trackitem", false},
    CommandInfo{"sequence.nextCaptionSegment", Scope::Application, "Sequence", "Go to Next Caption Segment", "Go to the next caption segment", "context.timeline", "cmd.timeline.goto.next.caption.trackitem", true},
    CommandInfo{"sequence.previousCaptionSegment", Scope::Application, "Sequence", "Go to Previous Caption Segment", "Go to the previous caption segment", "context.timeline", "cmd.timeline.goto.prev.caption.trackitem", true},
    CommandInfo{"sequence.rippleTrimPreviousToPlayhead", Scope::Application, "Sequence", "Ripple Trim Previous Edit to Playhead", "Ripple Trim Previous Edit To Playhead", "context.global", "cmd.sequence.rippletrimpreviousedittoplayhead", false},
    CommandInfo{"sequence.rippleTrimNextToPlayhead", Scope::Application, "Sequence", "Ripple Trim Next Edit to Playhead", "Ripple Trim Next Edit To Playhead", "context.global", "cmd.sequence.rippletrimnextedittoplayhead", false},
    CommandInfo{"sequence.selectClipAtPlayhead", Scope::Application, "Sequence", "Select Clip at Playhead", "Select Clip at Playhead", "context.global", "cmd.tlnav.select.clip.at.playhead", false},
    // Multi-Camera (M5l): Premiere's shipped key sets list these in context.global (docs/keymap.md).
    CommandInfo{"multicam.selectCamera1", Scope::Application, "Multi-Camera", "Select Camera 1", "Select Camera 1", "context.global", "cmd.multicam.choosenocut.camera1", false},
    CommandInfo{"multicam.selectCamera2", Scope::Application, "Multi-Camera", "Select Camera 2", "Select Camera 2", "context.global", "cmd.multicam.choosenocut.camera2", false},
    CommandInfo{"multicam.selectCamera3", Scope::Application, "Multi-Camera", "Select Camera 3", "Select Camera 3", "context.global", "cmd.multicam.choosenocut.camera3", false},
    CommandInfo{"multicam.selectCamera4", Scope::Application, "Multi-Camera", "Select Camera 4", "Select Camera 4", "context.global", "cmd.multicam.choosenocut.camera4", false},
    CommandInfo{"multicam.selectCamera5", Scope::Application, "Multi-Camera", "Select Camera 5", "Select Camera 5", "context.global", "cmd.multicam.choosenocut.camera5", false},
    CommandInfo{"multicam.selectCamera6", Scope::Application, "Multi-Camera", "Select Camera 6", "Select Camera 6", "context.global", "cmd.multicam.choosenocut.camera6", false},
    CommandInfo{"multicam.selectCamera7", Scope::Application, "Multi-Camera", "Select Camera 7", "Select Camera 7", "context.global", "cmd.multicam.choosenocut.camera7", false},
    CommandInfo{"multicam.selectCamera8", Scope::Application, "Multi-Camera", "Select Camera 8", "Select Camera 8", "context.global", "cmd.multicam.choosenocut.camera8", false},
    CommandInfo{"multicam.selectCamera9", Scope::Application, "Multi-Camera", "Select Camera 9", "Select Camera 9", "context.global", "cmd.multicam.choosenocut.camera9", false},
    CommandInfo{"multicam.cutToCamera1", Scope::Application, "Multi-Camera", "Cut to Camera 1", "Cut to Camera 1", "context.global", "cmd.multicam.choose.camera.1", false},
    CommandInfo{"multicam.cutToCamera2", Scope::Application, "Multi-Camera", "Cut to Camera 2", "Cut to Camera 2", "context.global", "cmd.multicam.choose.camera.2", false},
    CommandInfo{"multicam.cutToCamera3", Scope::Application, "Multi-Camera", "Cut to Camera 3", "Cut to Camera 3", "context.global", "cmd.multicam.choose.camera.3", false},
    CommandInfo{"multicam.cutToCamera4", Scope::Application, "Multi-Camera", "Cut to Camera 4", "Cut to Camera 4", "context.global", "cmd.multicam.choose.camera.4", false},
    CommandInfo{"multicam.cutToCamera5", Scope::Application, "Multi-Camera", "Cut to Camera 5", "Cut to Camera 5", "context.global", "cmd.multicam.choose.camera.5", false},
    CommandInfo{"multicam.cutToCamera6", Scope::Application, "Multi-Camera", "Cut to Camera 6", "Cut to Camera 6", "context.global", "cmd.multicam.choose.camera.6", false},
    CommandInfo{"multicam.cutToCamera7", Scope::Application, "Multi-Camera", "Cut to Camera 7", "Cut to Camera 7", "context.global", "cmd.multicam.choose.camera.7", false},
    CommandInfo{"multicam.cutToCamera8", Scope::Application, "Multi-Camera", "Cut to Camera 8", "Cut to Camera 8", "context.global", "cmd.multicam.choose.camera.8", false},
    CommandInfo{"multicam.cutToCamera9", Scope::Application, "Multi-Camera", "Cut to Camera 9", "Cut to Camera 9", "context.global", "cmd.multicam.choose.camera.9", false},
    CommandInfo{"multicam.toggleView", Scope::Application, "Multi-Camera", "Toggle Multi-Camera View", "Toggle Multi-Camera View", "context.global", "cmd.multicam.toggle.multicam.view", false},
    // Markers
    CommandInfo{"marker.markIn", Scope::Application, "Markers", "Mark In", "Mark In", "context.global", "cmd.common.setin", false},
    CommandInfo{"marker.markOut", Scope::Application, "Markers", "Mark Out", "Mark Out", "context.global", "cmd.common.setout", false},
    CommandInfo{"marker.markClip", Scope::Application, "Markers", "Mark Clip", "Mark Clip", "context.global", "cmd.marker.setsequenceinoutmarkeraroundtargetclip", false},
    CommandInfo{"marker.markSelection", Scope::Application, "Markers", "Mark Selection", "Mark Selection", "context.global", "cmd.marker.setsequenceinoutmarkeraroundselection.out", false},
    CommandInfo{"marker.goToIn", Scope::Application, "Markers", "Go to In", "Go to In", "context.global", "cmd.goto.in", false},
    CommandInfo{"marker.goToOut", Scope::Application, "Markers", "Go to Out", "Go to Out", "context.global", "cmd.goto.out", false},
    CommandInfo{"marker.clearIn", Scope::Application, "Markers", "Clear In", "Clear In", "context.global", "cmd.clear.in", false},
    CommandInfo{"marker.clearOut", Scope::Application, "Markers", "Clear Out", "Clear Out", "context.global", "cmd.clear.out", false},
    CommandInfo{"marker.clearInAndOut", Scope::Application, "Markers", "Clear In and Out", "Clear In and Out", "context.global", "cmd.clear.inandout", false},
    CommandInfo{"marker.add", Scope::Application, "Markers", "Add Marker", "Add Marker", "context.global", "cmd.set.marker", false},
    CommandInfo{"marker.goToNext", Scope::Application, "Markers", "Go to Next Marker", "Go to Next Marker", "context.global", "cmd.marker.gotomarker.next", false},
    CommandInfo{"marker.goToPrevious", Scope::Application, "Markers", "Go to Previous Marker", "Go to Previous Marker", "context.global", "cmd.marker.gotomarker.previous", false},
    CommandInfo{"marker.clearSelected", Scope::Application, "Markers", "Clear Selected Marker", "Clear Selected Marker", "context.global", "cmd.marker.clearmarker.current", false},
    CommandInfo{"marker.clearAll", Scope::Application, "Markers", "Clear All Markers", "Clear All Markers", "context.global", "cmd.marker.clearmarker.all", false},
    // Graphics and Titles
    CommandInfo{"graphics.newTextLayer", Scope::Application, "Graphics and Titles", "New Layer: Text", "Text", "context.global", "cmd.graphics.add.text", false},
    CommandInfo{"graphics.newRectangleLayer", Scope::Application, "Graphics and Titles", "New Layer: Rectangle", "Rectangle", "context.global", "cmd.graphics.add.shape.rectangle", false},
    CommandInfo{"graphics.newEllipseLayer", Scope::Application, "Graphics and Titles", "New Layer: Ellipse", "Ellipse", "context.global", "cmd.graphics.add.shape.ellipse", false},
    CommandInfo{"graphics.bringToFront", Scope::Application, "Graphics and Titles", "Bring to Front", "Bring to Front", "context.global", "cmd.graphics.move.graphic.layer.to.top", false},
    CommandInfo{"graphics.bringForward", Scope::Application, "Graphics and Titles", "Bring Forward", "Bring Forward", "context.global", "cmd.graphics.move.graphic.layer.up", false},
    CommandInfo{"graphics.sendBackward", Scope::Application, "Graphics and Titles", "Send Backward", "Send Backward", "context.global", "cmd.graphics.move.graphic.layer.down", false},
    CommandInfo{"graphics.sendToBack", Scope::Application, "Graphics and Titles", "Send to Back", "Send to Back", "context.global", "cmd.graphics.move.graphic.layer.to.bottom", false},
    CommandInfo{"graphics.selectNextLayer", Scope::Application, "Graphics and Titles", "Select Next Layer", "Select Next Layer", "context.global", "cmd.graphics.select.next.graphic.layer", false},
    CommandInfo{"graphics.selectPreviousLayer", Scope::Application, "Graphics and Titles", "Select Previous Layer", "Select Previous Layer", "context.global", "cmd.graphics.select.previous.graphic.layer", false},
    // Window
    CommandInfo{"window.workspace1", Scope::Application, "Window", "Workspace: All Panels", "All Panels", "context.global", "cmd.window.user.workspace.0", false},
    CommandInfo{"window.workspace2", Scope::Application, "Window", "Workspace: Assembly", "Assembly", "context.global", "cmd.window.user.workspace.1", false},
    CommandInfo{"window.workspace3", Scope::Application, "Window", "Workspace: Audio", "Audio", "context.global", "cmd.window.user.workspace.2", false},
    CommandInfo{"window.workspace4", Scope::Application, "Window", "Workspace: Captions and Graphics", "Captions and Graphics", "context.global", "cmd.window.user.workspace.3", false},
    CommandInfo{"window.workspace5", Scope::Application, "Window", "Workspace: Color", "Color", "context.global", "cmd.window.user.workspace.4", false},
    CommandInfo{"window.workspace6", Scope::Application, "Window", "Workspace: Editing", "Editing", "context.global", "cmd.window.user.workspace.5", false},
    CommandInfo{"window.workspace7", Scope::Application, "Window", "Workspace: Effects", "Effects", "context.global", "cmd.window.user.workspace.6", false},
    CommandInfo{"window.workspace8", Scope::Application, "Window", "Workspace: Essential", "Essential", "context.global", "cmd.window.user.workspace.7", false},
    CommandInfo{"window.workspace9", Scope::Application, "Window", "Workspace: Learning", "Learning", "context.global", "cmd.window.user.workspace.8", false},
    CommandInfo{"window.resetToSavedLayout", Scope::Application, "Window", "Reset to Saved Layout", "Reset to Saved Layout", "context.global", "cmd.window.workspace.revert", false},
    CommandInfo{"window.maximizeFrame", Scope::Application, "Window", "Maximize Frame", "Maximize Frame", "context.global", "cmd.toggle.maximize.focused.frame", false},
    CommandInfo{"window.audioClipMixer", Scope::Application, "Window", "Audio Clip Mixer", "Audio Clip Mixer", "context.global", "uif.window.Audio Clip Mixer", false},
    CommandInfo{"window.audioTrackMixer", Scope::Application, "Window", "Audio Track Mixer", "Audio Track Mixer", "context.global", "uif.window.Audio Mixers", false},
    CommandInfo{"window.effectControls", Scope::Application, "Window", "Effect Controls", "Effect Controls", "context.global", "uif.window.Effect Controls", false},
    CommandInfo{"window.effects", Scope::Application, "Window", "Effects", "Effects", "context.global", "uif.window.Effects", false},
    CommandInfo{"window.mediaBrowser", Scope::Application, "Window", "Media Browser", "Media Browser", "context.global", "uif.window.Media Browser", false},
    CommandInfo{"window.programMonitor", Scope::Application, "Window", "Program Monitor", "Program Monitor", "context.global", "uif.window.Program Monitors", false},
    CommandInfo{"window.projects", Scope::Application, "Window", "Projects", "Projects", "context.global", "uif.window.Projects", false},
    CommandInfo{"window.sourceMonitor", Scope::Application, "Window", "Source Monitor", "Source Monitor", "context.global", "uif.window.Source Monitors", false},
    CommandInfo{"window.timeline", Scope::Application, "Window", "Timeline", "Timeline", "context.global", "uif.window.Timelines", false},
    // Tools
    CommandInfo{"tool.selection", Scope::Application, "Tools", "Selection Tool", "Selection Tool", "context.global", "cmd.tools.01pointer", false},
    CommandInfo{"tool.trackSelectForward", Scope::Application, "Tools", "Track Select Forward Tool", "Track Select Forward Tool", "context.global", "cmd.tools.02trackselectforward", false},
    CommandInfo{"tool.trackSelectBackward", Scope::Application, "Tools", "Track Select Backward Tool", "Track Select Backward Tool", "context.global", "cmd.tools.02_5trackselectbackward", false},
    CommandInfo{"tool.rippleEdit", Scope::Application, "Tools", "Ripple Edit Tool", "Ripple Edit Tool", "context.global", "cmd.tools.03ripple", false},
    CommandInfo{"tool.rollingEdit", Scope::Application, "Tools", "Rolling Edit Tool", "Rolling Edit Tool", "context.global", "cmd.tools.04roll", false},
    CommandInfo{"tool.rateStretch", Scope::Application, "Tools", "Rate Stretch Tool", "Rate Stretch Tool", "context.global", "cmd.tools.05ratestretch", false},
    CommandInfo{"tool.razor", Scope::Application, "Tools", "Razor Tool", "Razor Tool", "context.global", "cmd.tools.06razor", false},
    CommandInfo{"tool.slip", Scope::Application, "Tools", "Slip Tool", "Slip Tool", "context.global", "cmd.tools.07slip", false},
    CommandInfo{"tool.slide", Scope::Application, "Tools", "Slide Tool", "Slide Tool", "context.global", "cmd.tools.08slide", false},
    CommandInfo{"tool.pen", Scope::Application, "Tools", "Pen Tool", "Pen Tool", "context.global", "cmd.tools.09pen", false},
    CommandInfo{"tool.hand", Scope::Application, "Tools", "Hand Tool", "Hand Tool", "context.global", "cmd.tools.10hand", false},
    CommandInfo{"tool.zoom", Scope::Application, "Tools", "Zoom Tool", "Zoom Tool", "context.global", "cmd.tools.11zoom", false},
    CommandInfo{"tool.type", Scope::Application, "Tools", "Type Tool", "Type Tool", "context.global", "cmd.tools.12text", false},
    // Playback
    CommandInfo{"transport.playStop", Scope::Application, "Playback", "Play/Stop", "Play-Stop Toggle", "context.global", "cmd.transport.toggleplay", false},
    CommandInfo{"transport.shuttleBackward", Scope::Application, "Playback", "Shuttle Left", "Shuttle Left", "context.global", "cmd.transport.shuttle.left", false},
    CommandInfo{"transport.shuttleStop", Scope::Application, "Playback", "Shuttle Stop", "Shuttle Stop", "context.global", "cmd.transport.shuttle.stop", false},
    CommandInfo{"transport.shuttleForward", Scope::Application, "Playback", "Shuttle Right", "Shuttle Right", "context.global", "cmd.transport.shuttle.right", false},
    CommandInfo{"transport.shuttleSlowBackward", Scope::Application, "Playback", "Shuttle Slow Left", "Shuttle Slow Left", "context.global", "cmd.transport.shuttle.slow.left", false},
    CommandInfo{"transport.shuttleSlowForward", Scope::Application, "Playback", "Shuttle Slow Right", "Shuttle Slow Right", "context.global", "cmd.transport.shuttle.slow.right", false},
    CommandInfo{"transport.stepBackward", Scope::Application, "Playback", "Step Back One Frame", "Step Back", "context.global", "cmd.transport.step.back", true},
    CommandInfo{"transport.stepForward", Scope::Application, "Playback", "Step Forward One Frame", "Step Forward", "context.global", "cmd.transport.step.forward", true},
    CommandInfo{"transport.stepBackwardFive", Scope::Application, "Playback", "Step Back Five Frames", "Step Back Five Frames - Units", "context.global", "cmd.transport.step.back.five", true},
    CommandInfo{"transport.stepForwardFive", Scope::Application, "Playback", "Step Forward Five Frames", "Step Forward Five Frames - Units", "context.global", "cmd.transport.step.forward.five", true},
    CommandInfo{"transport.goToPreviousEdit", Scope::Application, "Playback", "Go to Previous Edit Point", "Go to Previous Edit Point", "context.global", "cmd.tlnav.prev.edit", true},
    CommandInfo{"transport.goToNextEdit", Scope::Application, "Playback", "Go to Next Edit Point", "Go to Next Edit Point", "context.global", "cmd.tlnav.next.edit", true},
    CommandInfo{"transport.goToPreviousEditAnyTrack", Scope::Application, "Playback", "Go to Previous Edit Point on Any Track", "Go to Previous Edit Point on Any Track", "context.global", "cmd.tlnav.prev.edit.any.track", true},
    CommandInfo{"transport.goToNextEditAnyTrack", Scope::Application, "Playback", "Go to Next Edit Point on Any Track", "Go to Next Edit Point on Any Track", "context.global", "cmd.tlnav.next.edit.any.track", true},
    CommandInfo{"transport.goToStart", Scope::Application, "Playback", "Go to Sequence-Clip Start", "Go to Sequence-Clip Start", "context.global", "cmd.transport.sequence.start", false},
    CommandInfo{"transport.goToEnd", Scope::Application, "Playback", "Go to Sequence-Clip End", "Go to Sequence-Clip End", "context.global", "cmd.transport.sequence.end", false},
    CommandInfo{"transport.goToSelectedClipStart", Scope::Application, "Playback", "Go to Selected Clip Start", "Go to Selected Clip Start", "context.global", "cmd.transport.selectedclip.start", false},
    CommandInfo{"transport.goToSelectedClipEnd", Scope::Application, "Playback", "Go to Selected Clip End", "Go to Selected Clip End", "context.global", "cmd.transport.selectedclip.end", false},
    CommandInfo{"transport.playInToOut", Scope::Application, "Playback", "Play In to Out", "Play In to Out", "context.global", "cmd.transport.playintoout", false},
    CommandInfo{"transport.playAround", Scope::Application, "Playback", "Play Around", "Play Around", "context.global", "cmd.transport.playedit", false},
    // A.8: voice-over recording (Premiere's Voice-over Record, the track header's microphone). Premiere's default key sets
    // bind no key to it (the macOS set lists cmd.timeline.voiceover.track.record in context.timeline, unbound; the
    // Windows set does not list it), so ours have none either; a custom .kys that binds one carries over to
    // record.toggle (docs/keymap.md).
    CommandInfo{"record.toggle", Scope::Application, "Playback", "Record Voice-over", "", "context.timeline", "cmd.timeline.voiceover.track.record", false},
    CommandInfo{"record.start", Scope::Application, "Playback", "Start Voice-over Recording", "", "", "", false},
    CommandInfo{"record.stop", Scope::Application, "Playback", "Stop Voice-over Recording", "", "", "", false},
    // Audio Track Mixer
    CommandInfo{"audioMixer.showHideTracks", Scope::AudioMixer, "Audio Track Mixer", "Show/Hide Tracks", "Show/Hide Tracks", "context.audiomixer", "cmd.audiomixer.showhidetracks", false},
    CommandInfo{"audioMixer.loop", Scope::AudioMixer, "Audio Track Mixer", "Loop", "Loop", "context.audiomixer", "cmd.audiomixer.loop", false},
    CommandInfo{"audioMixer.meterInputsOnly", Scope::AudioMixer, "Audio Track Mixer", "Meter Input(s) Only", "Meter Input(s) Only", "context.audiomixer", "cmd.audiomixer.meterinput", false},
    // Effect Controls
    CommandInfo{"effectControls.removeSelectedEffect", Scope::EffectControls, "Effect Controls", "Remove Selected Effect", "Remove Selected Effect", "context.effectcontrols", "cmd.edit.clear", false},
    CommandInfo{"effectControls.loopAudioPlayback", Scope::EffectControls, "Effect Controls", "Loop During Audio-Only Playback", "Loop During Audio-Only Playback", "context.effectcontrols", "cmd.effectcontrols.loop.audio", false},
    // Effects
    CommandInfo{"effects.newCustomBin", Scope::Effects, "Effects", "New Custom Bin", "New Custom Bin", "context.effects", "cmd.effects.createcustomfolder", false},
    CommandInfo{"effects.deleteCustomItem", Scope::Effects, "Effects", "Delete Custom Item", "Delete Custom Item", "context.effects", "cmd.edit.clear", false},
    // History
    CommandInfo{"history.stepBackward", Scope::History, "History", "Step Backward", "Step Backward", "context.history", "cmd.history.step.backward", true},
    CommandInfo{"history.stepForward", Scope::History, "History", "Step Forward", "Step Forward", "context.history", "cmd.history.step.forward", true},
    CommandInfo{"history.delete", Scope::History, "History", "Delete", "Delete", "context.history", "cmd.edit.clear", false},
    // Media Browser
    CommandInfo{"mediaBrowser.openInSourceMonitor", Scope::MediaBrowser, "Media Browser", "Open in Source Monitor", "Open in Source Monitor", "context.mediabrowser", "cmd.mediabrowser.openinsourcemonitor", false},
    CommandInfo{"mediaBrowser.selectDirectoryList", Scope::MediaBrowser, "Media Browser", "Select Directory List", "Select Directory List", "context.mediabrowser", "cmd.mediabrowser.selectdirectorylist", false},
    CommandInfo{"mediaBrowser.selectMediaList", Scope::MediaBrowser, "Media Browser", "Select Media List", "Select Media List", "context.mediabrowser", "cmd.mediabrowser.selectmedialist", false},
    // Metadata
    CommandInfo{"metadata.loop", Scope::Metadata, "Metadata", "Loop", "Loop", "context.metadata_editor", "cmd.metadataeditor.loop", false},
    CommandInfo{"metadata.play", Scope::Metadata, "Metadata", "Play", "Play", "context.metadata_editor", "cmd.metadataeditor.play", false},
    // Program Monitor
    CommandInfo{"programMonitor.showRulers", Scope::ProgramMonitor, "Program Monitor", "Show Rulers", "Show Rulers", "context.program.monitor", "cmd.monitor.rulers", false},
    CommandInfo{"programMonitor.showGuides", Scope::ProgramMonitor, "Program Monitor", "Show Guides", "Show Guides", "context.program.monitor", "cmd.monitor.guides", false},
    CommandInfo{"programMonitor.snap", Scope::ProgramMonitor, "Program Monitor", "Snap in Program Monitor", "Snap in Program Monitor", "context.program.monitor", "cmd.monitor.snapping", false},
    CommandInfo{"programMonitor.lockGuides", Scope::ProgramMonitor, "Program Monitor", "Lock Guides", "Lock Guides", "context.program.monitor", "cmd.monitor.lockguides", false},
    CommandInfo{"programMonitor.nudgeUpFive", Scope::ProgramMonitor, "Program Monitor", "Nudge Selected Object Up by Five", "Nudge Selected Object up by five frames", "context.program.monitor", "cmd.monitor.nudge.up.five", true},
    CommandInfo{"programMonitor.nudgeRightFive", Scope::ProgramMonitor, "Program Monitor", "Nudge Selected Object Right by Five", "Nudge Selected Object right by five frames", "context.program.monitor", "cmd.monitor.nudge.right.five", true},
    CommandInfo{"programMonitor.nudgeLeftFive", Scope::ProgramMonitor, "Program Monitor", "Nudge Selected Object Left by Five", "Nudge Selected Object left by five frames", "context.program.monitor", "cmd.monitor.nudge.left.five", true},
    CommandInfo{"programMonitor.nudgeDownFive", Scope::ProgramMonitor, "Program Monitor", "Nudge Selected Object Down by Five", "Nudge Selected Object down by five frames", "context.program.monitor", "cmd.monitor.nudge.down.five", true},
    CommandInfo{"programMonitor.nudgeUp", Scope::ProgramMonitor, "Program Monitor", "Nudge Selected Object Up by One", "Nudge Selected Object up by one frame", "context.program.monitor", "cmd.monitor.nudge.up.one", true},
    CommandInfo{"programMonitor.nudgeRight", Scope::ProgramMonitor, "Program Monitor", "Nudge Selected Object Right by One", "Nudge Selected Object right by one frame", "context.program.monitor", "cmd.monitor.nudge.right.one", true},
    CommandInfo{"programMonitor.nudgeLeft", Scope::ProgramMonitor, "Program Monitor", "Nudge Selected Object Left by One", "Nudge Selected Object left by one frame", "context.program.monitor", "cmd.monitor.nudge.left.one", true},
    CommandInfo{"programMonitor.nudgeDown", Scope::ProgramMonitor, "Program Monitor", "Nudge Selected Object Down by One", "Nudge Selected Object down by one frame", "context.program.monitor", "cmd.monitor.nudge.down.one", true},
    // Project
    CommandInfo{"project.newBin", Scope::Project, "Project", "New Bin", "New Bin", "context.project", "cmd.file.new.bin", false},
    CommandInfo{"project.delete", Scope::Project, "Project", "Delete", "Delete", "context.project", "cmd.edit.clear", false},
    CommandInfo{"project.listView", Scope::Project, "Project", "List View", "List", "context.project", "cmd.project.wingtip.view.list", false},
    CommandInfo{"project.iconView", Scope::Project, "Project", "Icon View", "Icon", "context.project", "cmd.project.wingtip.view.icon", false},
    CommandInfo{"project.hoverScrub", Scope::Project, "Project", "Hover Scrub", "Hover Scrub", "context.project", "cmd.project.enablehover", false},
    CommandInfo{"project.deleteWithOptions", Scope::Project, "Project", "Delete Selection with Options", "Delete Selection with Options", "context.project", "cmd.project.deletewithoptions", false},
    CommandInfo{"project.extendSelectionDown", Scope::Project, "Project", "Extend Selection Down", "Extend Selection Down", "context.project", "cmd.project.moveextend.down", true},
    CommandInfo{"project.extendSelectionLeft", Scope::Project, "Project", "Extend Selection Left", "Extend Selection Left", "context.project", "cmd.project.moveextend.left", true},
    CommandInfo{"project.extendSelectionRight", Scope::Project, "Project", "Extend Selection Right", "Extend Selection Right", "context.project", "cmd.project.moveextend.right", true},
    CommandInfo{"project.extendSelectionUp", Scope::Project, "Project", "Extend Selection Up", "Extend Selection Up", "context.project", "cmd.project.moveextend.up", true},
    CommandInfo{"project.moveSelectionDown", Scope::Project, "Project", "Move Selection Down", "Move Selection Down", "context.project", "cmd.project.move.down", true},
    CommandInfo{"project.moveSelectionEnd", Scope::Project, "Project", "Move Selection End", "Move Selection End", "context.project", "cmd.project.move.end", false},
    CommandInfo{"project.moveSelectionHome", Scope::Project, "Project", "Move Selection Home", "Move Selection Home", "context.project", "cmd.project.move.home", false},
    CommandInfo{"project.moveSelectionLeft", Scope::Project, "Project", "Move Selection Left", "Move Selection Left", "context.project", "cmd.project.movel.eft", true},
    CommandInfo{"project.moveSelectionPageDown", Scope::Project, "Project", "Move Selection Page Down", "Move Selection Page Down", "context.project", "cmd.project.move.pagedown", true},
    CommandInfo{"project.moveSelectionPageUp", Scope::Project, "Project", "Move Selection Page Up", "Move Selection Page Up", "context.project", "cmd.project.move.pageup", true},
    CommandInfo{"project.moveSelectionRight", Scope::Project, "Project", "Move Selection Right", "Move Selection Right", "context.project", "cmd.project.move.right", true},
    CommandInfo{"project.moveSelectionUp", Scope::Project, "Project", "Move Selection Up", "Move Selection Up", "context.project", "cmd.project.move.up", true},
    CommandInfo{"project.nextColumnField", Scope::Project, "Project", "Next Column Field", "Next Column Field", "context.project", "cmd.project.nextcolumnfield", false},
    CommandInfo{"project.nextRowField", Scope::Project, "Project", "Next Row Field", "Next Row Field", "context.project", "cmd.project.nextrowfield", false},
    CommandInfo{"project.openInSourceMonitor", Scope::Project, "Project", "Open in Source Monitor", "Open in Source Monitor", "context.project", "cmd.project.openinsource", false},
    CommandInfo{"project.previousColumnField", Scope::Project, "Project", "Previous Column Field", "Previous Column Field", "context.project", "cmd.project.previouscolumnfield", false},
    CommandInfo{"project.previousRowField", Scope::Project, "Project", "Previous Row Field", "Previous Row Field", "context.project", "cmd.project.previousrowfield", false},
    CommandInfo{"project.thumbnailSizeNext", Scope::Project, "Project", "Thumbnail Size Next", "Thumbnail Size Next", "context.project", "cmd.project.next.thumbnail.size.large", false},
    CommandInfo{"project.thumbnailSizePrevious", Scope::Project, "Project", "Thumbnail Size Previous", "Thumbnail Size Previous", "context.project", "cmd.project.previous.thumbnail.size.large", false},
    CommandInfo{"project.toggleView", Scope::Project, "Project", "Toggle View", "Toggle View", "context.project", "cmd.project.toggle.view", false},
    CommandInfo{"project.zoomIn", Scope::Project, "Project", "Zoom In", "Zoom In", "context.project", "cmd.project.zoom.in", true},
    CommandInfo{"project.zoomOut", Scope::Project, "Project", "Zoom Out", "Zoom Out", "context.project", "cmd.project.zoom.out", true},
    // Timeline
    CommandInfo{"timeline.clearSelection", Scope::Timeline, "Timeline", "Clear Selection", "Clear Selection", "context.timeline", "cmd.edit.clear", false},
    CommandInfo{"timeline.decreaseAudioTrackHeight", Scope::Timeline, "Timeline", "Decrease Audio Tracks Height", "Decrease Audio Tracks Height", "context.timeline", "cmd.timeline.decrease.audio.tracks.height", true},
    CommandInfo{"timeline.decreaseVideoTrackHeight", Scope::Timeline, "Timeline", "Decrease Video Tracks Height", "Decrease Video Tracks Height", "context.timeline", "cmd.timeline.decrease.video.tracks.height", true},
    CommandInfo{"timeline.expandAllTracks", Scope::Application, "Timeline", "Expand All Tracks", "Expand All Tracks", "context.global", "cmd.timeline.expand.all.tracks", false},
    CommandInfo{"timeline.increaseAudioTrackHeight", Scope::Timeline, "Timeline", "Increase Audio Tracks Height", "Increase Audio Tracks Height", "context.timeline", "cmd.timeline.increase.audio.tracks.height", true},
    CommandInfo{"timeline.increaseVideoTrackHeight", Scope::Timeline, "Timeline", "Increase Video Tracks Height", "Increase Video Tracks Height", "context.timeline", "cmd.timeline.increase.video.tracks.height", true},
    CommandInfo{"timeline.minimizeAllTracks", Scope::Application, "Timeline", "Minimize All Tracks", "Minimize All Tracks", "context.global", "cmd.timeline.minimize.all.tracks", false},
    CommandInfo{"timeline.nextSequenceTab", Scope::Timeline, "Timeline", "Next Sequence Tab", "", "", "", true},
    CommandInfo{"timeline.nudgeClipLeftFive", Scope::Timeline, "Timeline", "Nudge Clip Selection Left Five Frames", "Nudge Clip Selection Left Five Frames", "context.timeline", "cmd.timeline.nudge.left.several", true},
    CommandInfo{"timeline.nudgeClipLeft", Scope::Timeline, "Timeline", "Nudge Clip Selection Left One Frame", "Nudge Clip Selection Left One Frame", "context.timeline", "cmd.timeline.nudge.left.one", true},
    CommandInfo{"timeline.nudgeClipRightFive", Scope::Timeline, "Timeline", "Nudge Clip Selection Right Five Frames", "Nudge Clip Selection Right Five Frames", "context.timeline", "cmd.timeline.nudge.right.several", true},
    CommandInfo{"timeline.nudgeClipRight", Scope::Timeline, "Timeline", "Nudge Clip Selection Right One Frame", "Nudge Clip Selection Right One Frame", "context.timeline", "cmd.timeline.nudge.right.one", true},
    CommandInfo{"timeline.previousSequenceTab", Scope::Timeline, "Timeline", "Previous Sequence Tab", "", "", "", true},
    CommandInfo{"timeline.rippleDelete", Scope::Timeline, "Timeline", "Ripple Delete", "Ripple Delete", "context.timeline", "cmd.timeline.ripple.delete", false},
    CommandInfo{"timeline.setWorkAreaIn", Scope::Timeline, "Timeline", "Set Work Area Bar In Point", "Set Work Area Bar In Point", "context.timeline", "cmd.timeline.workbar.set.in", false},
    CommandInfo{"timeline.setWorkAreaOut", Scope::Timeline, "Timeline", "Set Work Area Bar Out Point", "Set Work Area Bar Out Point", "context.timeline", "cmd.timeline.workbar.set.out", false},
    CommandInfo{"timeline.showNextScreen", Scope::Timeline, "Timeline", "Show Next Screen", "Show Next Screen", "context.timeline", "cmd.timeline.show.next.screen", true},
    CommandInfo{"timeline.showPreviousScreen", Scope::Timeline, "Timeline", "Show Previous Screen", "Show Previous Screen", "context.timeline", "cmd.timeline.show.previous.screen", true},
    CommandInfo{"timeline.slideClipLeftFive", Scope::Timeline, "Timeline", "Slide Clip Selection Left Five Frames", "Slide Clip Selection Left Five Frames", "context.timeline", "cmd.timeline.slide.left.several", true},
    CommandInfo{"timeline.slideClipLeft", Scope::Timeline, "Timeline", "Slide Clip Selection Left One Frame", "Slide Clip Selection Left One Frame", "context.timeline", "cmd.timeline.slide.left.one", true},
    CommandInfo{"timeline.slideClipRightFive", Scope::Timeline, "Timeline", "Slide Clip Selection Right Five Frames", "Slide Clip Selection Right Five Frames", "context.timeline", "cmd.timeline.slide.right.several", true},
    CommandInfo{"timeline.slideClipRight", Scope::Timeline, "Timeline", "Slide Clip Selection Right One Frame", "Slide Clip Selection Right One Frame", "context.timeline", "cmd.timeline.slide.right.one", true},
    CommandInfo{"timeline.slipClipLeftFive", Scope::Timeline, "Timeline", "Slip Clip Selection Left Five Frames", "Slip Clip Selection Left Five Frames", "context.timeline", "cmd.timeline.slip.left.several", true},
    CommandInfo{"timeline.slipClipLeft", Scope::Timeline, "Timeline", "Slip Clip Selection Left One Frame", "Slip Clip Selection Left One Frame", "context.timeline", "cmd.timeline.slip.left.one", true},
    CommandInfo{"timeline.slipClipRightFive", Scope::Timeline, "Timeline", "Slip Clip Selection Right Five Frames", "Slip Clip Selection Right Five Frames", "context.timeline", "cmd.timeline.slip.right.several", true},
    CommandInfo{"timeline.slipClipRight", Scope::Timeline, "Timeline", "Slip Clip Selection Right One Frame", "Slip Clip Selection Right One Frame", "context.timeline", "cmd.timeline.slip.right.one", true},
};
// clang-format on

} // namespace

CommandRegistry::CommandRegistry(std::span<const CommandInfo> commands) : m_commands(commands) {
    for (std::size_t i = 0; i < m_commands.size(); ++i) {
        const CommandInfo& c = m_commands[i];
        m_byId.try_emplace(c.id, i);
        if (!c.kysName.empty()) {
            m_byKys.try_emplace(std::pair{c.kysContext, c.kysName}, i);
        }
    }
}

const CommandRegistry& CommandRegistry::builtin() {
    static const CommandRegistry registry{kBuiltinCommands};
    return registry;
}

const CommandInfo* CommandRegistry::find(std::string_view id) const {
    const auto it = m_byId.find(id);
    return it == m_byId.end() ? nullptr : &m_commands[it->second];
}

const CommandInfo* CommandRegistry::findKys(std::string_view kysContext, std::string_view kysName) const {
    const auto it = m_byKys.find(std::pair{kysContext, kysName});
    return it == m_byKys.end() ? nullptr : &m_commands[it->second];
}

} // namespace wabcut::keymap
