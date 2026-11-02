pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// 0.6 (P1 round, #53): the script prompter's part of Settings > Audio. A narration script - a .txt / .md / .fountain
// file (Open script…) or pasted text (Paste…) - is shown on the Program Monitor while a voice-over records, one paragraph at
// a time, so an essayist does not read from another window and lose the place between takes. Here: the script and where it
// is, the text size, the mirror for a teleprompter glass, whether the script also shows while a track is armed, and a
// marker per paragraph read while recording. The script is not part of the project: the VoiceOver bridge keeps it, and the
// paragraph reached, in the settings by project file.
ColumnLayout {
    id: root
    objectName: "scriptPrompterSection"

    /// The paste area is open.
    property bool pasting: false
    /// The last refusal ("The script is larger than 256 KB"), or "".
    property string error: ""

    readonly property int count: VoiceOver.paragraphs.length
    readonly property string summary: root.count === 0
        ? qsTr("No script")
        : qsTr("%1 · %2").arg(VoiceOver.scriptPath !== "" ? root.fileName(VoiceOver.scriptPath) : qsTr("Pasted text"))
                         .arg(root.count === 1 ? qsTr("1 paragraph") : qsTr("%1 paragraphs").arg(root.count))

    function fileName(path: string): string {
        const parts = path.split(/[\\/]/)
        return parts.length > 0 ? parts[parts.length - 1] : path
    }
    function openPaste() {
        pasteArea.text = VoiceOver.scriptText
        root.error = ""
        root.pasting = true
        pasteArea.forceActiveFocus()
    }
    function usePasted() {
        root.error = VoiceOver.setScriptText(pasteArea.text)
        if (root.error === "")
            root.pasting = false
    }

    spacing: Theme.space(2)

    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }
    component Caption: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }

    FileDialog {
        id: openScript
        title: qsTr("Open a script")
        nameFilters: [qsTr("Scripts (*.txt *.md *.markdown *.fountain)")]
        onAccepted: {
            root.error = VoiceOver.loadScript(selectedFile)
            if (root.error === "")
                root.pasting = false
        }
    }

    SectionHeader {
        Layout.fillWidth: true
        text: qsTr("Script prompter")
    }
    Note {
        text: qsTr("Show your narration script on the monitor while you record: the paragraph you are reading, large, and the next one dimmed. Right or Page Down moves on and Left or Page Up goes back while a take runs, so a presentation clicker works.")
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)
        PillButton {
            objectName: "openScriptButton"
            compact: true
            iconName: "folder-open"
            text: qsTr("Open script…")
            enabled: !VoiceOver.recording
            onClicked: openScript.open()
        }
        PillButton {
            objectName: "pasteScriptButton"
            compact: true
            iconName: "clipboard-paste"
            text: root.pasting ? qsTr("Close paste") : qsTr("Paste…")
            enabled: !VoiceOver.recording
            onClicked: {
                if (root.pasting)
                    root.pasting = false
                else
                    root.openPaste()
            }
        }
        PillButton {
            objectName: "clearScriptButton"
            compact: true
            variant: "ghost"
            iconName: "x"
            text: qsTr("Remove")
            enabled: root.count > 0 && !VoiceOver.recording
            onClicked: {
                VoiceOver.clearScript()
                root.error = ""
            }
        }
        Text {
            objectName: "scriptSummary"
            Layout.fillWidth: true
            text: root.summary
            elide: Text.ElideMiddle
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    // Paste: the text area and what uses it.
    ColumnLayout {
        objectName: "scriptPasteArea"
        Layout.fillWidth: true
        visible: root.pasting
        spacing: Theme.space(1)
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.space(40)
            radius: Theme.radiusField
            color: Theme.sunken
            border.width: Theme.hairline
            border.color: pasteArea.activeFocus ? Theme.focus : Theme.border
            ScrollView {
                anchors.fill: parent
                anchors.margins: Theme.space(1)
                TextArea {
                    id: pasteArea
                    objectName: "scriptPasteText"
                    wrapMode: TextEdit.Wrap
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    selectByMouse: true
                    placeholderText: qsTr("Paste your script here. A blank line starts a new paragraph.")
                    placeholderTextColor: Theme.textTertiary
                    background: null
                    Accessible.name: qsTr("Script text")
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            PillButton {
                compact: true
                text: qsTr("Cancel")
                onClicked: root.pasting = false
            }
            PillButton {
                objectName: "useScriptTextButton"
                compact: true
                variant: "primary"
                text: qsTr("Use this text")
                enabled: pasteArea.text.trim() !== ""
                onClicked: root.usePasted()
            }
        }
    }

    Text {
        objectName: "scriptError"
        Layout.fillWidth: true
        visible: text !== ""
        text: root.error
        wrapMode: Text.Wrap
        color: Theme.dangerText
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    GridLayout {
        Layout.fillWidth: true
        columns: 4
        columnSpacing: Theme.space(4)
        rowSpacing: Theme.space(2)

        Caption { text: qsTr("Text size") }
        NumberField {
            objectName: "prompterSizeField"
            from: 16
            to: 96
            stepSize: 2
            value: VoiceOver.prompterTextSize
            suffix: qsTr("px")
            Accessible.name: qsTr("Prompter text size")
            onValueModified: v => VoiceOver.prompterTextSize = Math.round(v)
        }
        Caption { text: qsTr("Mirror") }
        Toggle {
            objectName: "prompterMirrorToggle"
            checked: VoiceOver.mirror
            tooltip: qsTr("Flip the script left to right for a teleprompter's glass")
            onToggled: {
                VoiceOver.mirror = checked
                checked = Qt.binding(() => VoiceOver.mirror)
            }
        }

        Caption { text: qsTr("Show when armed") }
        Toggle {
            objectName: "prompterShowToggle"
            checked: VoiceOver.showScript
            tooltip: qsTr("Also show the script while a track is armed for recording, before a take starts")
            onToggled: {
                VoiceOver.showScript = checked
                checked = Qt.binding(() => VoiceOver.showScript)
            }
        }
        Caption { text: qsTr("Marker per paragraph") }
        Toggle {
            objectName: "prompterMarkerToggle"
            checked: VoiceOver.markerPerParagraph
            tooltip: qsTr("Drop a marker, named after its first words, at each paragraph you move on to while recording")
            onToggled: {
                VoiceOver.markerPerParagraph = checked
                checked = Qt.binding(() => VoiceOver.markerPerParagraph)
            }
        }
    }

    // The script as the monitor will show it: the paragraph reached, at this size, mirrored when asked.
    Rectangle {
        objectName: "prompterPreview"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(previewText.implicitHeight + Theme.space(4), Theme.space(34))
        visible: root.count > 0
        radius: Theme.radiusField
        color: Theme.scrim
        clip: true
        Text {
            id: previewText
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.space(2)
            text: VoiceOver.paragraphs[Math.min(VoiceOver.paragraphIndex, root.count - 1)] ?? ""
            wrapMode: Text.Wrap
            color: Theme.scrimText
            font.family: Theme.fontSans
            font.pixelSize: VoiceOver.prompterTextSize
            font.weight: Font.DemiBold
            lineHeight: 1.15
            transform: Scale {
                origin.x: previewText.width / 2
                xScale: VoiceOver.mirror ? -1 : 1
            }
            Accessible.ignored: true
        }
        Accessible.role: Accessible.StaticText
        Accessible.name: qsTr("Preview of the prompter text")
    }
}
