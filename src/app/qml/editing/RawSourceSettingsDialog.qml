pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// RAW Source Settings (0.2): Premiere's Clip > Modify > Source Settings for camera RAW items (ProRes RAW, CinemaDNG) -
// the exposure the mosaic is developed with, the ISO (0.7: As Shot, or the ISO it was shot at and the one to develop for:
// a stop per doubling) and its white balance: the camera's (As Shot) or the colour temperature and tint of the light to
// balance for. Playback, thumbnails and export develop the items so. One undo step (media.setRawSourceSettings).
WabcutDialog {
    id: dialog
    objectName: "rawSourceSettingsDialog"

    property var mediaKeys: []
    property var info: ({})
    property real exposure: 0
    property bool custom: false
    property real temperature: 5600
    property real tint: 0
    // 0.7 (card #108): ISO. Wabcut cannot read the ISO a camera recorded, so "Custom" says which ISO the clip was shot at
    // and which to develop it for: a stop of exposure per doubling.
    property bool isoCustom: false
    property real isoShot: 800
    property real isoDevelop: 800
    readonly property real isoStops: isoShot > 0 && isoDevelop > 0 ? Math.log(isoDevelop / isoShot) / Math.LN2 : 0
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
        dialog.isoCustom = dialog.info.iso === "custom"
        dialog.isoShot = dialog.info.isoShot ?? 800
        dialog.isoDevelop = dialog.info.isoDevelop ?? 800
        open()
    }
    function apply() {
        const error = Editing.applyRawSourceSettings(dialog.mediaKeys, {
            exposure: dialog.exposure, whiteBalance: dialog.custom ? "custom" : "asShot",
            temperature: Math.round(dialog.temperature), tint: Math.round(dialog.tint),
            iso: dialog.isoCustom ? "custom" : "asShot",
            isoShot: Math.round(dialog.isoShot), isoDevelop: Math.round(dialog.isoDevelop)
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

    SectionHeader { text: qsTr("ISO") }
    PillTabs {
        objectName: "rawIso"
        Layout.fillWidth: true
        compact: true
        Accessible.name: qsTr("ISO")
        model: [qsTr("As Shot"), qsTr("Custom")]
        currentIndex: dialog.isoCustom ? 1 : 0
        onActivated: index => dialog.isoCustom = index === 1
    }
    PropSlider {
        objectName: "rawIsoShot"
        Layout.fillWidth: true
        visible: dialog.isoCustom
        label: qsTr("Shot at")
        unit: qsTr("ISO")
        from: 50
        to: 25600
        defaultValue: 800
        stepSize: 50
        value: dialog.isoShot
        onValueModified: v => dialog.isoShot = v
    }
    PropSlider {
        objectName: "rawIsoDevelop"
        Layout.fillWidth: true
        visible: dialog.isoCustom
        label: qsTr("Develop for")
        unit: qsTr("ISO")
        from: 50
        to: 25600
        defaultValue: 800
        stepSize: 50
        value: dialog.isoDevelop
        onValueModified: v => dialog.isoDevelop = v
    }
    Text {
        objectName: "rawIsoHint"
        Layout.fillWidth: true
        text: !dialog.isoCustom ? qsTr("The camera's own ISO: the picture is developed as it was recorded.")
            : qsTr("The camera's ISO is not stored in the file: say what the clip was shot at. Each doubling is one stop (%1 stops now).")
                  .arg((dialog.isoStops >= 0 ? "+" : "") + dialog.isoStops.toFixed(2))
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
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
