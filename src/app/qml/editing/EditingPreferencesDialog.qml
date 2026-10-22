pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Preferences (P.2, E5): Premiere's Preferences > Media and > Timeline pages that decide how edits come out.
//   * Media: Default Media Scaling - how media imported from now on is sized in sequences of another frame size (None,
//     Scale to frame size, Set to frame size, Set to fill frame). Items already imported keep theirs, as in Premiere.
//   * Timeline: the Still Image Default Duration (for stills imported from now on) and the Video and Audio Transition
//     Default Durations (Ctrl+D, Ctrl+Shift+D, Shift+D).
// Saved at once (Editing bridge, QSettings). Opened from the File menu and Edit > Preferences (the palette, keys).
WabcutDialog {
    id: dialog
    objectName: "editingPreferencesDialog"

    /// The page shown: "media" or "timeline".
    property string page: "media"

    function openAt(which) {
        dialog.page = which === "timeline" ? "timeline" : "media"
        open()
    }

    title: qsTr("Preferences")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(170), Theme.space(170))

    component Label: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }
    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }
    component Seconds: NumberField {
        Layout.preferredWidth: Theme.numberFieldWidth
        from: 0.04
        to: 3600
        stepSize: 1
        dragStep: 0.1
        decimals: 2
        suffix: qsTr("s")
    }

    PillTabs {
        objectName: "preferencesPageTabs"
        Layout.fillWidth: true
        compact: true
        readonly property var values: ["media", "timeline"]
        model: [qsTr("Media"), qsTr("Timeline")]
        currentIndex: values.indexOf(dialog.page)
        onActivated: index => dialog.page = values[index]
    }

    // ---- Media ----
    ColumnLayout {
        Layout.fillWidth: true
        visible: dialog.page === "media"
        spacing: Theme.space(3)

        Label { text: qsTr("Default media scaling") }
        PillTabs {
            objectName: "defaultMediaScalingTabs"
            Layout.fillWidth: true
            compact: true
            readonly property var values: ["none", "scaleToFrameSize", "setToFrameSize", "setToFillFrame"]
            model: [qsTr("None"), qsTr("Scale to frame size"), qsTr("Set to frame size"), qsTr("Set to fill frame")]
            currentIndex: values.indexOf(Editing.defaultMediaScaling)
            onActivated: index => {
                Editing.defaultMediaScaling = values[index]
                currentIndex = Qt.binding(() => values.indexOf(Editing.defaultMediaScaling))
            }
        }
        Note {
            text: qsTr("How media you import from now on is sized when it goes into a sequence of another frame size. Scale to frame size fits the picture and keeps Motion > Scale at 100 %; Set to frame size and Set to fill frame set Motion > Scale so the picture fits or fills the frame. Media already imported keeps its setting; change single clips with Video Options in the clip menu.")
        }
        // 0.2 (P.3): Premiere's Enable Project Locking (projects in a production are always locked).
        Toggle {
            objectName: "lockProjectsToggle"
            text: qsTr("Lock projects while I have them open")
            checked: ProjectFile.lockProjects
            onToggled: {
                ProjectFile.lockProjects = checked
                checked = Qt.binding(() => ProjectFile.lockProjects)
            }
        }
        Note {
            text: qsTr("Someone else who opens a project you have open (in a shared folder) gets it read-only, and you see their lock the same way. Projects in a production are always locked.")
        }
        // 0.2 (P.3): XMP into the media files themselves (off: sidecars next to them).
        Toggle {
            objectName: "embedXmpToggle"
            text: qsTr("Write XMP metadata into MP4 and MOV files")
            checked: Library.embedXmp
            onToggled: {
                Library.embedXmp = checked
                checked = Qt.binding(() => Library.embedXmp)
            }
        }
        Note {
            text: qsTr("Off, metadata you write goes into an .xmp file next to the media and the media is never touched. On, MP4 and MOV files take it themselves: a checked copy replaces the file, and if anything fails the file stays as it was. Other formats always use the .xmp file.")
        }
    }

    // ---- Timeline ----
    GridLayout {
        Layout.fillWidth: true
        visible: dialog.page === "timeline"
        columns: 2
        columnSpacing: Theme.space(4)
        rowSpacing: Theme.space(2)

        Label { text: qsTr("Video transition default duration") }
        Seconds {
            objectName: "videoTransitionDuration"
            value: Editing.videoTransitionDuration
            Accessible.name: qsTr("Video transition default duration")
            onValueModified: value => Editing.videoTransitionDuration = value
        }
        Label { text: qsTr("Audio transition default duration") }
        Seconds {
            objectName: "audioTransitionDuration"
            value: Editing.audioTransitionDuration
            Accessible.name: qsTr("Audio transition default duration")
            onValueModified: value => Editing.audioTransitionDuration = value
        }
        Label { text: qsTr("Still image default duration") }
        Seconds {
            objectName: "stillImageDuration"
            value: Editing.stillDuration
            Accessible.name: qsTr("Still image default duration")
            onValueModified: value => Editing.stillDuration = value
        }
    }
    Note {
        visible: dialog.page === "timeline"
        text: qsTr("Transitions you add with Apply Video Transition, Apply Audio Transition and Apply Default Transitions to Selection take these lengths (shortened to the media there is). Stills you import from now on last the still image duration when you edit them in.")
    }

    footerData: [
        PillButton {
            objectName: "preferencesClose"
            variant: "primary"
            text: qsTr("Done")
            onClicked: dialog.close()
        }
    ]
}
