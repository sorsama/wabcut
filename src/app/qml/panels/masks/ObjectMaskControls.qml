pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// ● OBJECT (AI-14, docs/masks.md "Object masks") in Effect Controls' mask section: the AI Object Mask for the clip and
// target of Masks. Arm the tool and click the person or object on the program monitor (Alt-click, or the minus toggle:
// "not this"), or drag a box around it; more clicks refine it. Then track it through the shot - backward, both ways or
// forward - as a job with its progress, cancel and a per-frame confidence strip. The matte preview shows what the
// model sees: the object tinted, or the matte in grey. SAM 2.1 runs on this computer; it is downloaded once.
ColumnLayout {
    id: root
    objectName: "objectMaskControls"
    spacing: Theme.space(1)

    readonly property bool hasObject: ObjectMasks.mask !== ""

    // The model, when it is not here yet: one row; its licence card is shown before the download.
    property bool cardShown: false
    RowLayout {
        Layout.fillWidth: true
        visible: !ObjectMasks.available
        spacing: Theme.space(1)

        Icon {
            name: "sparkles"
            size: Theme.iconSm
            color: Theme.accentStroke
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("Object (AI): click a person or thing, it is followed through the shot")
            elide: Text.ElideRight
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        PillButton {
            objectName: "objectGetModel"
            visible: !ObjectMasks.downloading && !root.cardShown
            compact: true
            iconName: "download"
            text: qsTr("Get SAM 2.1")
            tooltip: qsTr("The model runs on this computer (190 MB, downloaded once): see its licence first")
            onClicked: root.cardShown = true
        }
    }
    Rectangle {
        Layout.fillWidth: true
        visible: !ObjectMasks.available && (root.cardShown || ObjectMasks.downloading)
        implicitHeight: modelColumn.implicitHeight + Theme.space(3)
        radius: Theme.radiusField
        color: Theme.raised
        border.width: Theme.hairline
        border.color: Theme.border

        ColumnLayout {
            id: modelColumn
            anchors.fill: parent
            anchors.margins: Theme.space(1.5)
            spacing: Theme.space(1)

            Text {
                objectName: "objectModelCard"
                Layout.fillWidth: true
                text: ObjectMasks.modelCard
                wrapMode: Text.Wrap
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
            RowLayout {
                visible: !ObjectMasks.downloading
                spacing: Theme.space(1)
                PillButton {
                    objectName: "objectDownload"
                    compact: true
                    variant: "primary"
                    iconName: "download"
                    text: qsTr("Download")
                    tooltip: qsTr("Download the model once; no picture leaves this computer")
                    onClicked: {
                        ObjectMasks.downloadModel()
                        root.cardShown = false
                    }
                }
                PillButton {
                    compact: true
                    variant: "ghost"
                    text: qsTr("Not now")
                    onClicked: root.cardShown = false
                }
            }
            BeamProgress {
                Layout.fillWidth: true
                visible: ObjectMasks.downloading
                active: ObjectMasks.downloading
                value: ObjectMasks.downloadProgress
                label: qsTr("Downloading SAM 2.1: %1 %").arg(Math.round(ObjectMasks.downloadProgress * 100))
            }
        }
    }

    // The tool.
    RowLayout {
        Layout.fillWidth: true
        visible: ObjectMasks.available
        spacing: Theme.space(1)

        IconButton {
            objectName: "objectTool"
            compact: true
            checkable: true
            checked: ObjectMasks.armed
            iconName: "sparkles"
            tooltip: qsTr("Object (AI): click the person or object on the program monitor, or drag a box around it")
            onClicked: {
                ObjectMasks.armed = checked
                checked = Qt.binding(() => ObjectMasks.armed)
            }
        }
        IconButton {
            objectName: "objectPositive"
            compact: true
            checkable: true
            checked: ObjectMasks.positive
            iconName: "plus"
            tooltip: qsTr("Clicks add to the object")
            onClicked: {
                ObjectMasks.positive = true
                checked = Qt.binding(() => ObjectMasks.positive)
            }
        }
        IconButton {
            objectName: "objectNegative"
            compact: true
            checkable: true
            checked: !ObjectMasks.positive
            iconName: "minus"
            tooltip: qsTr("Clicks take away from it (or Alt-click)")
            onClicked: {
                ObjectMasks.positive = false
                checked = Qt.binding(() => !ObjectMasks.positive)
            }
        }
        Text {
            Layout.fillWidth: true
            text: root.hasObject ? qsTr("Refining the selected object") : qsTr("Object (AI)")
            elide: Text.ElideRight
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        PillButton {
            objectName: "objectNew"
            compact: true
            variant: "ghost"
            visible: root.hasObject
            text: qsTr("New object")
            tooltip: qsTr("The next click starts a new object")
            onClicked: ObjectMasks.newObject()
        }
    }

    Text {
        Layout.fillWidth: true
        visible: ObjectMasks.available && ObjectMasks.armed && !root.hasObject
        text: qsTr("Click the person or object on the program monitor, or drag a box around it.")
        wrapMode: Text.Wrap
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    // Tracking through the shot.
    RowLayout {
        Layout.fillWidth: true
        visible: ObjectMasks.available && root.hasObject
        spacing: Theme.space(1)

        Text {
            text: qsTr("Track")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Item {
            Layout.fillWidth: true
        }
        IconButton {
            objectName: "objectTrackBackward"
            compact: true
            enabled: !ObjectMasks.busy
            iconName: "skip-back"
            tooltip: qsTr("Track backward to the shot's start")
            onClicked: ObjectMasks.propagate("backward")
        }
        IconButton {
            objectName: "objectTrackBoth"
            compact: true
            enabled: !ObjectMasks.busy
            iconName: "arrow-left-right"
            tooltip: qsTr("Track both ways through the shot")
            onClicked: ObjectMasks.propagate("both")
        }
        IconButton {
            objectName: "objectTrackForward"
            compact: true
            enabled: !ObjectMasks.busy
            iconName: "skip-forward"
            tooltip: qsTr("Track forward to the shot's end")
            onClicked: ObjectMasks.propagate("forward")
        }
        IconButton {
            objectName: "objectCancel"
            visible: ObjectMasks.busy
            compact: true
            iconName: "x"
            tooltip: qsTr("Stop (nothing is changed)")
            onClicked: ObjectMasks.cancel()
        }
    }
    Toggle {
        objectName: "objectAcrossShots"
        visible: ObjectMasks.available && root.hasObject
        text: qsTr("Across shot cuts")
        tooltip: qsTr("Keep following the object past the cuts of the clip")
        checked: ObjectMasks.acrossShots
        onToggled: ObjectMasks.acrossShots = checked
    }
    BeamProgress {
        objectName: "objectProgress"
        Layout.fillWidth: true
        visible: ObjectMasks.busy
        active: ObjectMasks.busy
        indeterminate: ObjectMasks.progress <= 0
        value: ObjectMasks.progress
        label: ObjectMasks.activity !== "" ? ObjectMasks.activity : qsTr("Working %1 %").arg(Math.round(ObjectMasks.progress * 100))
    }
    ConfidenceStrip {
        objectName: "objectConfidence"
        Layout.fillWidth: true
        visible: ObjectMasks.confidence.length > 1
        values: ObjectMasks.confidence
        total: ObjectMasks.total
    }
    Text {
        objectName: "objectStatus"
        Layout.fillWidth: true
        visible: text !== ""
        text: ObjectMasks.status
        wrapMode: Text.Wrap
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    // The matte preview over the monitor.
    RowLayout {
        Layout.fillWidth: true
        visible: ObjectMasks.available && root.hasObject
        spacing: Theme.space(1)
        Text {
            text: qsTr("Preview")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Item {
            Layout.fillWidth: true
        }
        PillTabs {
            objectName: "objectPreviewTabs"
            compact: true
            model: [qsTr("Off"), qsTr("Colour"), qsTr("Alpha")]
            currentIndex: ["off", "colour", "alpha"].indexOf(ObjectMasks.preview)
            onActivated: index => ObjectMasks.preview = ["off", "colour", "alpha"][index]
        }
    }
}
