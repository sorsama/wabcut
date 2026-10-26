pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Scene Edit Detection… (0.5, bridge `SceneDetect`; Premiere's timeline clip menu > Scene Edit Detection…): finds the
// edits inside the selected video clips (hard cuts, fades and dissolves; flashes are not edits) on this computer, off
// the GUI thread with its progress and Cancel, then in ONE undo step applies a cut at each (the linked audio too) and /
// or adds a marker at each. The detector (EditingChoice, the editing dialogs' dropdown) and Sensitivity (the content threshold inverted: higher finds more edits) are
// remembered with the two switches. A finished run closes the dialog; Main.qml shows its toast ("Split 3 clips at 41
// cuts"). Opened by Main.qml (clip.sceneEditDetection: the Clip menu, the clip menu, the palette) with openFor().
WabcutDialog {
    id: dialog
    objectName: "sceneEditDetectionDialog"

    property var clipKeys: []
    /// The sequence the clips are in ("s3"), kept with them: Analyze acts on them there even if another sequence became
    /// current meanwhile.
    property string sequenceKey: ""
    property int nestedCount: 0
    property bool cuts: true
    property bool markers: false
    property string detector: "combined"
    property real sensitivity: 50
    property string errorText: ""
    readonly property bool working: SceneDetect.busy
    readonly property var detectors: [
        { id: "combined", name: qsTr("Cuts, fades and dissolves") },
        { id: "content", name: qsTr("Hard cuts only") },
        { id: "adaptive", name: qsTr("Hard cuts in fast motion") },
        { id: "threshold", name: qsTr("Fades through black") }
    ]
    readonly property bool usesSensitivity: dialog.detector === "combined" || dialog.detector === "content"

    function openFor(keys: var, nested: int) {
        clipKeys = keys
        sequenceKey = SceneDetect.availability().sequence ?? ""
        nestedCount = nested
        const o = SceneDetect.options
        cuts = o.cuts !== false
        markers = o.markers === true
        detector = o.detector ?? "combined"
        sensitivity = o.sensitivity ?? 50
        errorText = keys.length === 0 ? qsTr("Select a video clip first") : ""
        open()
    }
    function analyze() {
        errorText = ""
        const error = SceneDetect.run(dialog.clipKeys, { cuts: dialog.cuts, markers: dialog.markers,
                                                        detector: dialog.detector, sensitivity: dialog.sensitivity,
                                                        sequence: dialog.sequenceKey })
        if (error !== "")
            errorText = error
    }

    title: qsTr("Scene Edit Detection")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(130), Theme.space(130))

    Connections {
        target: SceneDetect
        function onFinished(ok: bool, cancelled: bool, message: string) {
            if (!dialog.opened)
                return
            if (ok)
                dialog.close()
            else
                dialog.errorText = cancelled ? qsTr("Cancelled: nothing was changed") : message
        }
    }

    component Note: Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        text: qsTr("Finds the edits inside the footage: hard cuts, fades and dissolves (flashes are not edits). It runs on this computer.")
    }
    Note {
        objectName: "sceneEditClipCount"
        text: dialog.clipKeys.length === 1 ? qsTr("1 video clip selected")
                                           : qsTr("%1 video clips selected").arg(dialog.clipKeys.length)
    }

    // What to do at each edit.
    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.space(1)
        enabled: !dialog.working
        Toggle {
            objectName: "sceneEditCuts"
            text: qsTr("Apply a cut at each detected edit")
            tooltip: qsTr("Razors the clips where the shot changes; the linked audio is cut too")
            checked: dialog.cuts
            onToggled: {
                dialog.cuts = checked
                checked = Qt.binding(() => dialog.cuts)
            }
        }
        Toggle {
            objectName: "sceneEditMarkers"
            text: qsTr("Add a marker at each cut")
            tooltip: qsTr("A sequence marker where each shot starts")
            checked: dialog.markers
            onToggled: {
                dialog.markers = checked
                checked = Qt.binding(() => dialog.markers)
            }
        }
        Note {
            objectName: "sceneEditNestedNote"
            visible: dialog.markers && dialog.nestedCount > 0
            text: qsTr("Nested and multicam clips are cut but get no markers.")
        }
    }

    GridLayout {
        Layout.fillWidth: true
        columns: 3
        columnSpacing: Theme.space(3)
        rowSpacing: Theme.space(2)
        enabled: !dialog.working

        Text {
            text: qsTr("Detector")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        // The app's dialog dropdown (EditingChoice): the chosen detector on a pill, a click lists them with a check mark.
        EditingChoice {
            id: detectorChoice
            objectName: "sceneEditDetector"
            Layout.columnSpan: 2
            Layout.alignment: Qt.AlignLeft
            options: dialog.detectors
            current: dialog.detector
            tooltip: qsTr("What counts as an edit")
            Accessible.name: qsTr("Detector: %1").arg(text)
            onChosen: id => dialog.detector = id
        }

        Text {
            text: qsTr("Sensitivity")
            color: dialog.usesSensitivity ? Theme.textSecondary : Theme.textDisabled
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        ValueSlider {
            objectName: "sceneEditSensitivity"
            Layout.fillWidth: true
            Accessible.name: qsTr("Sensitivity")
            enabled: dialog.usesSensitivity
            from: 0
            to: 100
            stepSize: 1
            value: dialog.sensitivity
            onMoved: dialog.sensitivity = value
        }
        Text {
            objectName: "sceneEditSensitivityValue"
            Layout.preferredWidth: Theme.space(12)
            horizontalAlignment: Text.AlignRight
            text: qsTr("%1 %").arg(Math.round(dialog.sensitivity))
            color: dialog.usesSensitivity ? Theme.textPrimary : Theme.textDisabled
            font.family: Theme.fontMono
            font.pixelSize: Theme.textSm
        }
        Item { implicitWidth: 1; implicitHeight: 1 }
        Note {
            objectName: "sceneEditSensitivityHint"
            Layout.columnSpan: 2
            text: dialog.usesSensitivity ? qsTr("Higher finds more edits; lower only the clearest ones.")
                                         : qsTr("This detector has its own settings: Sensitivity does not apply.")
        }
    }

    BeamProgress {
        objectName: "sceneEditProgress"
        Layout.fillWidth: true
        visible: dialog.working
        indeterminate: SceneDetect.progress < 0
        value: Math.max(0, SceneDetect.progress)
        active: dialog.working
        label: SceneDetect.phase
    }
    Text {
        objectName: "sceneEditError"
        Layout.fillWidth: true
        visible: text !== "" && !dialog.working
        wrapMode: Text.Wrap
        text: dialog.errorText !== "" ? dialog.errorText
              : !dialog.cuts && !dialog.markers ? qsTr("Choose cuts, markers or both.") : ""
        color: dialog.errorText !== "" ? Theme.dangerText : Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    footerData: [
        PillButton {
            objectName: "sceneEditCancel"
            variant: "ghost"
            text: dialog.working ? qsTr("Cancel") : qsTr("Close")
            tooltip: dialog.working ? qsTr("Stop now: nothing is changed") : ""
            onClicked: {
                if (dialog.working)
                    SceneDetect.cancel()
                else
                    dialog.close()
            }
        },
        PillButton {
            objectName: "sceneEditAnalyze"
            variant: "primary"
            iconName: "clapperboard"
            text: qsTr("Analyze")
            enabled: !dialog.working && dialog.clipKeys.length > 0 && (dialog.cuts || dialog.markers)
            onClicked: dialog.analyze()
        }
    ]
}
