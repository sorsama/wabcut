pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Color Management (0.2, I.9; Premiere Pro's Sequence Settings > Color and the Lumetri Settings tab): the project's
// Color Setup - Direct Rec.709 (SDR, the default: HDR and log media tone-mapped into SDR on input), Direct HLG / PQ
// (HDR), Wide Gamut (Tone Mapped: ACES 1.3) and Wide Gamut (No Tone Mapping), or Disable Color Management - with the
// HDR peak and graphics white, and whether log media is read as the camera log its metadata names. OK applies all of
// it as one undo step (color.management.set).
WabcutDialog {
    id: dialog
    objectName: "colorManagementDialog"

    property string mode: "rec709"
    property real peakNits: 1000
    property real graphicsWhite: 203
    property bool autoDetectLog: true
    readonly property bool hdr: mode === "hdr-pq" || mode === "hdr-hlg"

    function openFromProject() {
        const cm = Color.colorManagement
        mode = cm.mode ?? "rec709"
        peakNits = cm.peakNits ?? 1000
        graphicsWhite = cm.referenceWhiteNits ?? 203
        autoDetectLog = cm.autoDetectLog ?? true
        open()
    }
    function apply() {
        if (Color.setColorManagement({ mode: dialog.mode, peakNits: dialog.peakNits,
                                       referenceWhiteNits: dialog.graphicsWhite, autoDetectLog: dialog.autoDetectLog }))
            close()
    }

    title: qsTr("Color Management")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(150), Theme.space(150))

    Text {
        text: qsTr("Color setup")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
        font.letterSpacing: Theme.labelTracking
    }

    // One row per preset: its name and what it does; the chosen one is outlined in the accent.
    Repeater {
        model: Color.colorManagementModes
        delegate: Rectangle {
            id: choice
            required property var modelData
            readonly property bool chosen: dialog.mode === modelData.id
            objectName: "colorSetup_" + modelData.id
            Layout.fillWidth: true
            implicitHeight: choiceColumn.implicitHeight + Theme.space(3)
            radius: Theme.radiusField
            color: choiceArea.containsMouse ? Theme.hover : chosen ? Theme.sunken : "transparent"
            border.width: chosen ? Theme.hairline * 2 : Theme.hairline
            border.color: chosen ? Theme.accentStroke : Theme.border
            activeFocusOnTab: true
            Accessible.role: Accessible.RadioButton
            Accessible.name: modelData.name
            Accessible.checked: chosen
            Keys.onSpacePressed: dialog.mode = modelData.id

            ColumnLayout {
                id: choiceColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.space(3)
                anchors.rightMargin: Theme.space(3)
                spacing: Theme.space(0.5)
                Text {
                    Layout.fillWidth: true
                    text: choice.modelData.name
                    color: choice.chosen ? Theme.textPrimary : Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: choice.chosen ? Font.DemiBold : Font.Normal
                }
                Text {
                    Layout.fillWidth: true
                    text: choice.modelData.description
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    wrapMode: Text.Wrap
                }
            }
            MouseArea {
                id: choiceArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: dialog.mode = choice.modelData.id
            }
            FocusRing {
                targetRadius: choice.radius
                visible: choice.activeFocus
            }
        }
    }

    // HDR: the mastering peak (the HDR10 metadata and the preview's knee) and the graphics white (where SDR white,
    // titles and captions land; ITU-R BT.2408's 203 nits by default).
    RowLayout {
        Layout.fillWidth: true
        visible: dialog.hdr
        spacing: Theme.space(3)
        Text {
            Layout.fillWidth: true
            text: qsTr("HDR peak")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        NumberField {
            objectName: "colorPeakNits"
            Layout.preferredWidth: Theme.numberFieldWidth
            from: 100
            to: 10000
            stepSize: 100
            dragStep: 10
            decimals: 0
            value: dialog.peakNits
            suffix: qsTr("nits")
            Accessible.name: qsTr("HDR peak")
            onValueModified: value => dialog.peakNits = value
        }
    }
    RowLayout {
        Layout.fillWidth: true
        visible: dialog.hdr
        spacing: Theme.space(3)
        Text {
            Layout.fillWidth: true
            text: qsTr("Graphics white")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        NumberField {
            objectName: "colorGraphicsWhite"
            Layout.preferredWidth: Theme.numberFieldWidth
            from: 80
            to: 400
            stepSize: 1
            dragStep: 1
            decimals: 0
            value: dialog.graphicsWhite
            suffix: qsTr("nits")
            Accessible.name: qsTr("Graphics white")
            onValueModified: value => dialog.graphicsWhite = value
        }
    }

    Toggle {
        objectName: "colorAutoDetectLog"
        Layout.fillWidth: true
        enabled: dialog.mode !== "off"
        text: qsTr("Color manage auto-detected log media")
        tooltip: qsTr("Clips whose camera metadata names a log curve (Sony S-Log3, ARRI LogC, ...) are read through it")
        checked: dialog.autoDetectLog
        onToggled: dialog.autoDetectLog = checked
    }

    footerData: [
        PillButton {
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "colorManagementOk"
            variant: "primary"
            text: qsTr("OK")
            onClicked: dialog.apply()
        }
    ]
}
