pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// Clip Speed / Duration (Ctrl/Cmd+R; the clip menu): speed and duration stay linked (changing one recomputes the
// other for the selected clip, ProjectController.durationForSpeed / speedForDuration), plus Reverse Speed, Maintain
// Audio Pitch and Ripple Edit. Every change is dry-run through the typed command clip.speedDuration
// (CommandBridge.preview: what would change, nothing committed); OK applies it as one undo step.
WabcutDialog {
    id: dialog
    objectName: "speedDurationDialog"

    property var info: ({})
    property real speed: 100
    property string duration: ""
    property bool reverse: false
    property bool maintainPitch: false
    property bool ripple: false
    /// P.2: Time Interpolation of the video clips ("frameSampling", "frameBlending", "opticalFlow"; docs/motion.md).
    property string interpolation: "frameSampling"
    readonly property var interpolationIds: ["frameSampling", "frameBlending", "opticalFlow"]
    /// The field typed last decides the request: "speed" or "duration".
    property string driver: "speed"
    property var preview: ({})

    title: qsTr("Clip Speed / Duration")

    function openDialog() {
        info = ProjectController.speedDurationInfo()
        speed = info.speed ?? 100
        duration = info.durationText ?? ""
        reverse = info.reversed ?? false
        maintainPitch = info.maintainPitch ?? false
        ripple = false
        interpolation = info.timeInterpolation ?? "frameSampling"
        driver = "speed"
        refreshPreview()
        open()
    }
    function params() {
        const p = { reverse: dialog.reverse, maintainPitch: dialog.maintainPitch, ripple: dialog.ripple }
        if (dialog.info.hasVideo ?? false)
            p.timeInterpolation = dialog.interpolation
        if (driver === "duration")
            p.duration = dialog.duration
        else
            p.speed = dialog.speed
        return p
    }
    function refreshPreview() {
        preview = info.available ? CommandBridge.preview("clip.speedDuration", params()) : ({})
    }
    function setSpeed(v) {
        speed = v
        driver = "speed"
        const d = ProjectController.durationForSpeed(v)
        if (d !== "")
            duration = d
        refreshPreview()
    }
    function setDuration(text) {
        const normalized = ProjectController.normalizeTimecode(text)
        if (normalized === "")
            return false
        duration = normalized
        driver = "duration"
        const s = ProjectController.speedForDuration(normalized)
        if (!Number.isNaN(s))
            speed = Math.round(s * 100) / 100
        refreshPreview()
        return true
    }
    function apply() {
        const result = CommandBridge.apply("clip.speedDuration", params())
        if (result.ok)
            close()
        else
            preview = result
    }

    Text {
        Layout.fillWidth: true
        visible: !(dialog.info.available ?? false)
        text: dialog.info.reason ?? ""
        wrapMode: Text.Wrap
        color: Theme.warning
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }

    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: Theme.space(3)
        rowSpacing: Theme.space(2)
        enabled: dialog.info.available ?? false

        Text {
            text: qsTr("Speed")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        RowLayout {
            spacing: Theme.space(2)
            NumberField {
                id: speedField
                objectName: "speedField"
                Layout.preferredWidth: Theme.numberFieldWidth + Theme.space(4)
                from: 0.01
                to: 10000
                decimals: 2
                stepSize: 1
                dragStep: 0.5
                suffix: qsTr("%")
                value: dialog.speed
                onValueModified: v => dialog.setSpeed(v)
                Accessible.name: qsTr("Speed")
            }
            // Quick picks: the speeds people reach for.
            Repeater {
                model: [50, 100, 200]
                delegate: PillButton {
                    required property int modelData
                    compact: true
                    text: qsTr("%1%").arg(modelData)
                    variant: Math.abs(dialog.speed - modelData) < 0.005 ? "primary" : "secondary"
                    onClicked: dialog.setSpeed(modelData)
                }
            }
        }

        Text {
            text: qsTr("Duration")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        TextField {
            id: durationField
            objectName: "durationField"
            Layout.preferredWidth: Theme.numberFieldWidth + Theme.space(10)
            implicitHeight: Theme.controlHeightSmall
            text: dialog.duration
            horizontalAlignment: TextInput.AlignRight
            verticalAlignment: TextInput.AlignVCenter
            leftPadding: Theme.space(3.5)
            rightPadding: Theme.space(3.5)
            selectByMouse: true
            font.family: Theme.fontMono
            font.pixelSize: Theme.textMd
            color: Theme.textPrimary
            selectionColor: Theme.accentFill
            selectedTextColor: Theme.accentOnFill
            Accessible.name: qsTr("Duration")
            onEditingFinished: {
                if (!dialog.setDuration(text))
                    text = Qt.binding(() => dialog.duration)
            }
            background: Rectangle {
                radius: height / 2
                color: Theme.raised
                border.width: durationField.activeFocus ? Theme.focusRingWidth : 0
                border.color: Theme.focus
            }
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.space(1)
        enabled: dialog.info.available ?? false

        Toggle {
            objectName: "reverseToggle"
            text: qsTr("Reverse Speed")
            checked: dialog.reverse
            onToggled: {
                dialog.reverse = checked
                dialog.refreshPreview()
            }
        }
        Toggle {
            objectName: "maintainPitchToggle"
            text: qsTr("Maintain Audio Pitch")
            enabled: dialog.info.hasAudio ?? false
            checked: dialog.maintainPitch
            onToggled: {
                dialog.maintainPitch = checked
                dialog.refreshPreview()
            }
        }
        Toggle {
            objectName: "rippleToggle"
            text: qsTr("Ripple Edit, Shifting Trailing Clips")
            checked: dialog.ripple
            onToggled: {
                dialog.ripple = checked
                dialog.refreshPreview()
            }
        }
    }

    // P.2: Time Interpolation (Premiere's dialog section): how retimed video makes the frames between its own.
    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.space(1)
        enabled: (dialog.info.available ?? false) && (dialog.info.hasVideo ?? false)

        Text {
            text: qsTr("Time Interpolation")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        PillTabs {
            objectName: "timeInterpolationTabs"
            Layout.fillWidth: true
            compact: true
            model: [qsTr("Frame Sampling"), qsTr("Frame Blending"), qsTr("Optical Flow")]
            currentIndex: Math.max(0, dialog.interpolationIds.indexOf(dialog.interpolation))
            Accessible.name: qsTr("Time Interpolation")
            onActivated: index => {
                dialog.interpolation = dialog.interpolationIds[index]
                dialog.refreshPreview()
            }
        }
    }

    // Dry run of the request: what would change, or why it cannot.
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: previewText.implicitHeight + Theme.space(4)
        visible: dialog.info.available ?? false
        radius: Theme.radiusField
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: (dialog.preview.ok ?? false) ? Theme.border : Theme.warning

        Text {
            id: previewText
            objectName: "speedPreview"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.margins: Theme.space(3)
            wrapMode: Text.Wrap
            text: (dialog.preview.ok ?? false) ? qsTr("Preview: %1").arg(dialog.preview.summary)
                                               : (dialog.preview.error ?? "")
            color: (dialog.preview.ok ?? false) ? Theme.textSecondary : Theme.warning
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    footerData: [
        PillButton {
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "speedApply"
            variant: "primary"
            text: qsTr("OK")
            enabled: (dialog.info.available ?? false) && (dialog.preview.ok ?? false)
            onClicked: dialog.apply()
        }
    ]
}
