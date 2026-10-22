pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// RAW Source Settings (0.2): Premiere's Clip > Modify > Source Settings for camera RAW items (ProRes RAW, CinemaDNG) -
// the exposure the mosaic is developed with, and its white balance: the camera's (As Shot) or the colour temperature
// and tint of the light to balance for. Playback, thumbnails and export develop the items so. One undo step
// (media.setRawSourceSettings).
WabcutDialog {
    id: dialog
    objectName: "rawSourceSettingsDialog"

    property var mediaKeys: []
    property var info: ({})
    property real exposure: 0
    property bool custom: false
    property real temperature: 5600
    property real tint: 0
    property string errorText: ""

    function openFor(keys) {
        dialog.mediaKeys = keys
        dialog.info = Editing.rawSourceSettings(keys)
        dialog.errorText = dialog.info.name === undefined
                ? qsTr("Source Settings develop camera RAW clips (ProRes RAW, CinemaDNG)") : ""
        dialog.exposure = dialog.info.exposure ?? 0
        dialog.custom = dialog.info.whiteBalance === "custom"
        dialog.temperature = dialog.info.temperature ?? 5600
        dialog.tint = dialog.info.tint ?? 0
        open()
    }
    function apply() {
        const error = Editing.applyRawSourceSettings(dialog.mediaKeys, {
            exposure: dialog.exposure, whiteBalance: dialog.custom ? "custom" : "asShot",
            temperature: Math.round(dialog.temperature), tint: Math.round(dialog.tint)
        })
        if (error === "")
            dialog.close()
        else
            dialog.errorText = error
    }

    title: qsTr("Source Settings")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(150), Theme.space(150))

    Text {
        Layout.fillWidth: true
        text: (dialog.info.count ?? 0) > 1 ? qsTr("%1 and %n more", "", dialog.info.count - 1).arg(dialog.info.name)
                                           : (dialog.info.name ?? "")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        elide: Text.ElideRight
    }

    SectionHeader { text: qsTr("Exposure") }
    PropSlider {
        objectName: "rawExposure"
        Layout.fillWidth: true
        label: qsTr("Exposure")
        unit: qsTr("stops")
        from: -5
        to: 5
        origin: 0
        defaultValue: 0
        stepSize: 0.05
        decimals: 2
        value: dialog.exposure
        onValueModified: v => dialog.exposure = v
    }

    SectionHeader { text: qsTr("White balance") }
    PillTabs {
        objectName: "rawWhiteBalance"
        Layout.fillWidth: true
        compact: true
        Accessible.name: qsTr("White balance")
        model: [qsTr("As Shot"), qsTr("Custom")]
        currentIndex: dialog.custom ? 1 : 0
        onActivated: index => dialog.custom = index === 1
    }
    PropSlider {
        objectName: "rawTemperature"
        Layout.fillWidth: true
        visible: dialog.custom
        label: qsTr("Temperature")
        unit: qsTr("K")
        from: 2000
        to: 15000
        defaultValue: 5600
        stepSize: 50
        value: dialog.temperature
        onValueModified: v => dialog.temperature = v
    }
    PropSlider {
        objectName: "rawTint"
        Layout.fillWidth: true
        visible: dialog.custom
        label: qsTr("Tint")
        from: -100
        to: 100
        origin: 0
        defaultValue: 0
        value: dialog.tint
        onValueModified: v => dialog.tint = v
    }
    Text {
        Layout.fillWidth: true
        text: dialog.custom ? qsTr("The light to balance for: lower temperatures turn the picture bluer, + tint toward magenta.")
                            : qsTr("The camera's white balance.")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }
    Text {
        objectName: "rawSourceSettingsError"
        Layout.fillWidth: true
        visible: text !== ""
        text: dialog.errorText
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    footerData: [
        PillButton {
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "rawSourceSettingsOk"
            variant: "primary"
            text: qsTr("OK")
            enabled: dialog.info.name !== undefined
            onClicked: dialog.apply()
        }
    ]
}
