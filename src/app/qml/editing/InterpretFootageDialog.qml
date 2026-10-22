pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Interpret Footage (P.2, E5): Premiere's Clip > Modify > Interpret Footage on the selected Library items - use the
// file's frame rate or assume another (a 120, 144 or 240 fps capture as 30 or 60 plays every frame in slow motion),
// the pixel aspect ratio and field order from the file or conformed, and the alpha channel ignored, inverted or
// conformed to straight or premultiplied. One undo step (media.interpretFootage).
WabcutDialog {
    id: dialog
    objectName: "interpretFootageDialog"

    property var mediaKeys: []
    property var info: ({})
    property bool assumeRate: false
    property real rate: 30
    property bool conformAspect: false
    property real aspect: 1
    property string fieldOrder: ""
    property bool ignoreAlpha: false
    property bool invertAlpha: false
    property string premultiplication: ""
    property string errorText: ""
    readonly property var rateOptions: [
        { id: "23.976", name: qsTr("23.976 fps") }, { id: "24", name: qsTr("24 fps") }, { id: "25", name: qsTr("25 fps") },
        { id: "29.97", name: qsTr("29.97 fps") }, { id: "30", name: qsTr("30 fps") }, { id: "50", name: qsTr("50 fps") },
        { id: "59.94", name: qsTr("59.94 fps") }, { id: "60", name: qsTr("60 fps") }, { id: "120", name: qsTr("120 fps") }
    ]
    readonly property var aspectOptions: [
        { id: "1", name: qsTr("Square Pixels (1.0)") }, { id: "0.9091", name: qsTr("D1/DV NTSC (0.9091)") },
        { id: "1.2121", name: qsTr("D1/DV NTSC Widescreen 16:9 (1.2121)") }, { id: "1.094", name: qsTr("D1/DV PAL (1.0940)") },
        { id: "1.4587", name: qsTr("D1/DV PAL Widescreen 16:9 (1.4587)") }, { id: "2", name: qsTr("Anamorphic 2:1 (2.0)") },
        { id: "1.3333", name: qsTr("HD Anamorphic 1080 (1.333)") }
    ]
    readonly property var fieldOptions: [
        { id: "", name: qsTr("Use Field Order from File") }, { id: "progressive", name: qsTr("No Fields (Progressive Scan)") },
        { id: "upperFirst", name: qsTr("Upper Field First") }, { id: "lowerFirst", name: qsTr("Lower Field First") }
    ]
    readonly property var premultiplicationOptions: [
        { id: "", name: qsTr("Use Alpha Premultiplication from File") }, { id: "straight", name: qsTr("Straight") },
        { id: "premultiplied", name: qsTr("Premultiplied") }
    ]

    function nearest(options, value) {
        let best = options[0].id
        for (const o of options) {
            if (Math.abs(Number(o.id) - value) < Math.abs(Number(best) - value))
                best = o.id
        }
        return best
    }
    function openFor(keys) {
        dialog.mediaKeys = keys
        dialog.info = Editing.interpretation(keys)
        dialog.errorText = dialog.info.name === undefined ? qsTr("Select clips in the Library") : ""
        dialog.assumeRate = (dialog.info.frameRate ?? 0) > 0
        // A high frame rate capture: the slow-motion conform (30 fps) is the likely choice.
        const file = dialog.info.fileRate ?? 0
        dialog.rate = dialog.assumeRate ? dialog.info.frameRate : file >= 100 ? 30 : file > 0 ? file : 30
        dialog.conformAspect = (dialog.info.pixelAspect ?? 0) > 0
        dialog.aspect = dialog.conformAspect ? dialog.info.pixelAspect : 1
        dialog.fieldOrder = dialog.info.fieldOrder ?? ""
        dialog.ignoreAlpha = dialog.info.ignoreAlpha === true
        dialog.invertAlpha = dialog.info.invertAlpha === true
        dialog.premultiplication = dialog.info.premultiplication ?? ""
        open()
    }
    function apply() {
        const error = Editing.applyInterpretation(dialog.mediaKeys, {
            frameRate: dialog.assumeRate ? dialog.rate : 0, pixelAspect: dialog.conformAspect ? dialog.aspect : 0,
            fieldOrder: dialog.fieldOrder, ignoreAlpha: dialog.ignoreAlpha, invertAlpha: dialog.invertAlpha,
            premultiplication: dialog.premultiplication
        })
        if (error === "")
            dialog.close()
        else
            dialog.errorText = error
    }

    title: qsTr("Interpret Footage")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(170), Theme.space(170))

    component Label: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }

    Text {
        Layout.fillWidth: true
        text: (dialog.info.count ?? 0) > 1 ? qsTr("%1 and %n more", "", dialog.info.count - 1).arg(dialog.info.name)
                                           : (dialog.info.name ?? "")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        elide: Text.ElideRight
    }

    SectionHeader { text: qsTr("Frame rate") }
    RowLayout {
        spacing: Theme.space(3)
        Toggle {
            objectName: "interpretAssumeRate"
            text: dialog.assumeRate ? qsTr("Assume this frame rate")
                                    : qsTr("Use frame rate from file (%1 fps)").arg(Math.round((dialog.info.fileRate ?? 0) * 1000) / 1000)
            checked: dialog.assumeRate
            onToggled: dialog.assumeRate = checked
        }
        EditingChoice {
            objectName: "interpretRate"
            visible: dialog.assumeRate
            options: dialog.rateOptions
            current: dialog.nearest(dialog.rateOptions, dialog.rate)
            onChosen: id => dialog.rate = Number(id)
        }
    }
    Label {
        Layout.fillWidth: true
        visible: dialog.assumeRate && (dialog.info.fileRate ?? 0) > dialog.rate + 0.01
        text: qsTr("Every frame plays: %1 % speed, %2 times as long.").arg(Math.round(dialog.rate / dialog.info.fileRate * 1000) / 10)
                                                                    .arg(Math.round(dialog.info.fileRate / dialog.rate * 100) / 100)
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    SectionHeader { text: qsTr("Pixel aspect ratio") }
    RowLayout {
        spacing: Theme.space(3)
        Toggle {
            text: dialog.conformAspect ? qsTr("Conform to")
                                       : qsTr("Use pixel aspect ratio from file (%1)").arg(Math.round((dialog.info.filePixelAspect ?? 1) * 10000) / 10000)
            checked: dialog.conformAspect
            onToggled: dialog.conformAspect = checked
        }
        EditingChoice {
            visible: dialog.conformAspect
            options: dialog.aspectOptions
            current: dialog.nearest(dialog.aspectOptions, dialog.aspect)
            onChosen: id => dialog.aspect = Number(id)
        }
    }

    SectionHeader { text: qsTr("Fields and alpha") }
    GridLayout {
        columns: 2
        columnSpacing: Theme.space(4)
        rowSpacing: Theme.space(2)
        Label { text: qsTr("Field order") }
        EditingChoice {
            options: dialog.fieldOptions
            current: dialog.fieldOrder
            onChosen: id => dialog.fieldOrder = id
        }
        Label { text: qsTr("Alpha premultiplication") }
        EditingChoice {
            options: dialog.premultiplicationOptions
            current: dialog.premultiplication
            onChosen: id => dialog.premultiplication = id
        }
    }
    RowLayout {
        spacing: Theme.space(4)
        Toggle {
            objectName: "interpretIgnoreAlpha"
            text: qsTr("Ignore Alpha Channel")
            checked: dialog.ignoreAlpha
            onToggled: dialog.ignoreAlpha = checked
        }
        Toggle {
            objectName: "interpretInvertAlpha"
            text: qsTr("Invert Alpha Channel")
            checked: dialog.invertAlpha
            onToggled: dialog.invertAlpha = checked
        }
    }
    Text {
        objectName: "interpretError"
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
            objectName: "interpretOk"
            variant: "primary"
            text: qsTr("OK")
            enabled: dialog.info.name !== undefined
            onClicked: dialog.apply()
        }
    ]
}
