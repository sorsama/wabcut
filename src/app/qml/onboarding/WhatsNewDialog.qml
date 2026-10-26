pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// What's new (M5j onboarding; refreshed in M5n): the highlights of this build, from docs/roadmap.md and the milestones
// merged since the last refresh. Static and offline: nothing is fetched. Opened from Help and the palette.
WabcutDialog {
    id: dialog
    objectName: "whatsNewDialog"

    // Keep in step with docs/roadmap.md when a milestone lands. This build: 0.5's caption lanes and nested waveforms,
    // Audio Meters and scrubbing, Scene Edit Detection and the dead-time review, chapter markers and the source
    // transcript, the hybrid-laptop GPU and opt-in updates first, then 0.4's timeline copy and clip facts, Premiere
    // keys, Effect Controls and transcript tools, gaming Shorts and safe quitting, 0.3's Home, menu bar, Effects panel
    // and everyday editing, 0.2's Premiere parity, the gaming wave (M5o), M5k-M5n, C.7-C.12, A.3-A.9, AI-9-AI-13.
    readonly property var highlights: [
        { key: "captionLanes", icon: "captions", title: qsTr("Captions on the timeline"),
          text: qsTr("Caption tracks are rows above the video tracks: trim, move, split and delete captions there like clips, and Ctrl+K cuts the targeted caption rows with the clips. Nested and multicam clips show the waveform of their mix.") },
        { key: "audioMeters", icon: "audio-lines", title: qsTr("Audio Meters and audio scrubbing"),
          text: qsTr("The Audio Meters panel sits right of the timeline, with a clip light. While playback is stopped, dragging the playhead or stepping frames plays the sound there; Shift+S turns it on and off.") },
        { key: "sceneDeadTime", icon: "scissors", title: qsTr("Scene Edit Detection and a dead-time review"),
          text: qsTr("Clip > Scene Edit Detection cuts or marks the shot changes in your clips. Dead time is listed stretch by stretch in Highlights: keep some, and remove the rest in one undo step.") },
        { key: "chaptersSource", icon: "bookmark", title: qsTr("Chapter markers and source transcripts"),
          text: qsTr("Chapter markers you add while recording in OBS become clip markers when you import the recording. The Transcript's Source mode reads the Source Monitor's clip: select a soundbite, then Insert or Overwrite it.") },
        { key: "gpuUpdates", icon: "download", title: qsTr("The fast GPU, and updates if you want them"),
          text: qsTr("Laptops with two GPUs preview and export on the discrete one, or on the one you choose in Settings. Wabcut can check for updates and install them when you quit, once you turn that on.") },
        { key: "timelineCopy", icon: "copy", title: qsTr("Copy and rearrange on the timeline"),
          text: qsTr("Alt-drag copies a clip and Ctrl+Alt-drag rearranges shots, as in Premiere. Clips show their speed and an fx badge, and pointing at one shows its times and source.") },
        { key: "premiereKeys", icon: "keyboard", title: qsTr("Premiere's keys and transport"),
          text: qsTr("Shift+J and Shift+L slow shuttle, Shift+K Play Around, Alt+Up and Alt+Down, ] and [ for clip volume, Page Up and Page Down, timecodes typed on the numpad, and markers that move with ripple edits.") },
        { key: "effectsTranscript", icon: "clipboard-paste", title: qsTr("Copy effects, work from the transcript"),
          text: qsTr("Copy and paste effects between clips in Effect Controls and apply a grade to many clips. The transcript follows the playhead, has a word menu, and Transcribe picks your voice tracks.") },
        { key: "gamingShorts", icon: "gamepad-2", title: qsTr("Gaming Shorts in one go"),
          text: qsTr("Highlights makes a Short of every moment you accept, with a lead-in, a tail and the Shorts caption style, and Export all queues them. File > Import Folder brings a capture folder in as bins.") },
        { key: "safeWork", icon: "shield-check", title: qsTr("Safe quitting and background work"),
          text: qsTr("Quitting asks while an export or a render runs, a Background tasks pill shows their progress and the recent messages, failed autosaves and exports say why, and Windows stays awake until the export is done.") },
        { key: "homeStart", icon: "house", title: qsTr("A new Home"),
          text: qsTr("Home, Projects and Learn pages, one Import for files, folders and timelines, and a New project page with Advanced settings that can start from your media.") },
        { key: "menuBar", icon: "columns-3", title: qsTr("A menu bar and a Window menu"),
          text: qsTr("Premiere's menus in the title bar, each command with its shortcut. Window opens a panel in the group you clicked last, and Maximize Frame gives a group the whole work area.") },
        { key: "effectsFolders", icon: "folders", title: qsTr("Effects in Premiere's folders"),
          text: qsTr("Favorites, Presets, Video Effects and the rest in a List, Compact or Icons view; presets import and export per bin, and a double-click applies to every selected clip.") },
        { key: "everyday", icon: "check", title: qsTr("Quicker everyday editing"),
          text: qsTr("Type a timecode to jump, ripple-delete a gap, add or delete tracks from a header's menu, read marker names on the ruler, search Settings, and export with your last choices.") },
        { key: "premiereTools", icon: "film", title: qsTr("More of Premiere"),
          text: qsTr("Premiere's 41 transitions and about 70 effects, Optical Flow slow motion, Morph Cut, Warp Stabilizer, clip groups and fade handles.") },
        { key: "exportQueue", icon: "list", title: qsTr("A Media Encoder queue"),
          text: qsTr("Quick Export, your own presets, alpha and image sequences, and a queue you start, pause and resume, with watch folders.") },
        { key: "monitorMotion", icon: "play", title: qsTr("Move it in the monitor"),
          text: qsTr("Drag, scale and rotate in the Program Monitor, with rulers, guides, loop playback and a full-screen output.") },
        { key: "projectPanel", icon: "tag", title: qsTr("Project panel and media"),
          text: qsTr("Metadata with XMP, search bins, a freeform view, productions, several open projects, and the Media Browser for camera cards.") },
        { key: "audioPost", icon: "sliders-horizontal", title: qsTr("Submixes and audio post"),
          text: qsTr("Submixes and sends, the Audio Clip Mixer, Premiere's audio effects, and AAF or OMF for Pro Tools.") },
        { key: "aiNested", icon: "sparkles", title: qsTr("AI through nested clips"),
          text: qsTr("Highlights, Auto Reframe and object masks read through multicam and nested clips; Redact finds the faces; sound effects come from a prompt with your own key.") },
        { key: "gamingStart", icon: "gamepad-2", title: qsTr("Made for gaming videos"),
          text: qsTr("Drop an OBS or ShadowPlay recording on Home: the New project page matches its frame rate, gives each audio stream its own named track, and 4K, high-fps or long files get proxies.") },
        { key: "highlights", icon: "zap", title: qsTr("Find the highlights"),
          text: qsTr("Wabcut ranks the loudest, busiest moments of a VOD on this computer, then builds a highlight reel or a Short from the ones you keep.") },
        { key: "layouts", icon: "layout-grid", title: qsTr("Facecam layouts"),
          text: qsTr("Corner facecam, vertical split or blurred fill for Shorts, in one click from the clip menu.") },
        { key: "callouts", icon: "sparkles", title: qsTr("Callouts and stickers"),
          text: qsTr("Subscribe buttons, kill-streak banners, arrows and lower thirds you type on right in the monitor, plus Lottie stickers.") },
        { key: "moments", icon: "timer", title: qsTr("Slow-mo and punch-ins"),
          text: qsTr("Slow down the moment under the playhead or punch in on it with one click; the keys stay editable.") },
        { key: "gamingExport", icon: "upload", title: qsTr("Export for every platform"),
          text: qsTr("YouTube 60 fps, Shorts and TikTok, Twitch and Discord size targets, with the file size and export time before you start.") },
        { key: "multicam", icon: "layers", title: qsTr("Multi-camera and nested sequences"),
          text: qsTr("Cut between angles live with the number keys, nest clips into their own sequence, and let Wabcut switch to whoever is speaking.") },
        { key: "library", icon: "library", title: qsTr("A pro Library"),
          text: qsTr("Bins, a sortable list view with metadata columns, subclips, find and filters, and unused or duplicate media at a glance.") },
        { key: "captions", icon: "captions", title: qsTr("The caption workshop"),
          text: qsTr("Styles and presets, speakers, fixes from the transcript, and translation into other languages on this computer.") },
        { key: "essentialSound", icon: "audio-lines", title: qsTr("Essential Sound and Remix"),
          text: qsTr("Tag a clip as dialogue, music, effects or ambience for one-click repair, and remix music to any length on its own bars.") },
        { key: "mixer", icon: "sliders-horizontal", title: qsTr("Mixer automation and plugins"),
          text: qsTr("Track faders with Read, Touch, Latch and Write, and your CLAP and VST3 plugins in their own process, so a crash never takes the edit down.") },
        { key: "voiceOver", icon: "mic", title: qsTr("Voice-over recording"),
          text: qsTr("Arm a track, count in and record over the program; every pass is a take you can switch back to.") },
        { key: "channels", icon: "volume-2", title: qsTr("Audio channels and 5.1"),
          text: qsTr("Split two microphones recorded on one file, fill or swap channels, and mix and export in 5.1.") },
        { key: "history", icon: "clock", title: qsTr("Version history and review notes"),
          text: qsTr("Go back to any saved version or compare two, and gather frame-accurate notes into a review package a client opens in a browser.") },
        { key: "paste", icon: "clipboard-paste", title: qsTr("Paste that keeps everything"),
          text: qsTr("Copy and paste keeps speed, effects and keyframes; Paste Attributes chooses what carries over.") },
        { key: "extensions", icon: "puzzle", title: qsTr("Extensions"),
          text: qsTr("Add your own commands with sandboxed scripts that ask before they get a permission.") },
        { key: "ai", icon: "sparkles", title: qsTr("More AI, still your call"),
          text: qsTr("Enhance Speech, stem separation, search, Auto Reframe and object masks that follow a person through the shot run on this computer; cloud features use your own key and ask first.") }
    ]

    title: qsTr("What's new in Wabcut")

    ListView {
        id: list
        objectName: "whatsNewList"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, Theme.space(110))
        clip: true
        spacing: Theme.space(3)
        model: dialog.highlights
        boundsBehavior: Flickable.StopAtBounds
        Accessible.role: Accessible.List
        Accessible.name: dialog.title
        ScrollBar.vertical: ThinScrollBar {}

        delegate: RowLayout {
            id: row
            required property var modelData
            width: ListView.view.width
            spacing: Theme.space(3)
            objectName: "whatsNew_" + modelData.key
            Accessible.role: Accessible.ListItem
            Accessible.name: modelData.title + ". " + modelData.text

            // The feature's icon on a round accent tint (design language v3: the kit's soft icon button).
            Rectangle {
                Layout.alignment: Qt.AlignTop
                implicitWidth: Theme.space(10)
                implicitHeight: Theme.space(10)
                radius: width / 2
                color: Theme.accentWash

                Icon {
                    anchors.centerIn: parent
                    name: row.modelData.icon
                    size: Theme.iconMd
                    color: Theme.accentStroke
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space(0.5)
                Text {
                    Layout.fillWidth: true
                    text: row.modelData.title
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textLg
                    font.weight: Theme.weightLabel

                    wrapMode: Text.Wrap
                }
                Text {
                    Layout.fillWidth: true
                    text: row.modelData.text
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    footerData: [
        PillButton {
            objectName: "whatsNewCloseButton"
            variant: "primary"
            text: qsTr("Got it")
            onClicked: dialog.close()
        }
    ]

    onAboutToShow: list.positionViewAtBeginning()
}
