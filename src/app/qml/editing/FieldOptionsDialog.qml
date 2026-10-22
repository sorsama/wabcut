pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Field Options (0.2): Premiere's Clip > Video Options > Field Options for interlaced footage. Reverse Field Dominance
// swaps the clip's field order; Processing Options: None, Interlace Consecutive Frames, Always Deinterlace, Flicker
// Removal. Interlaced footage in a progressive sequence is deinterlaced without any of them (model/FieldOptions.h).
// One undo step over the selection (clip.setFieldOptions).
WabcutDialog {
    id: dialog
    objectName: "fieldOptionsDialog"

    property string clipKey: ""
    property var info: ({})
    property bool reverseDominance: false
    property string processing: "none"
    property string errorText: ""
    readonly property var processingOptions: [
        { id: "none", name: qsTr("None") },
        { id: "interlaceConsecutiveFrames", name: qsTr("Interlace Consecutive Frames") },
        { id: "alwaysDeinterlace", name: qsTr("Always Deinterlace") },
        { id: "flickerRemoval", name: qsTr("Flicker Removal") }
    ]
    readonly property string footageText: dialog.info.fieldOrder === "upperFirst" ? qsTr("Footage: Upper Field First")
                                          : dialog.info.fieldOrder === "lowerFirst" ? qsTr("Footage: Lower Field First")
                                          : qsTr("Footage: No Fields (Progressive Scan)")

    function openFor(key) {
        dialog.clipKey = key
        dialog.info = Editing.fieldOptionsInfo(key)
        dialog.errorText = dialog.info.available === true ? "" : qsTr("Select a video clip")
        dialog.reverseDominance = dialog.info.reverseFieldDominance === true
        dialog.processing = dialog.info.processing ?? "none"
        open()
    }
    function apply() {
        const error = Editing.applyFieldOptions(dialog.clipKey,
                                                { reverseFieldDominance: dialog.reverseDominance, processing: dialog.processing })
        if (error === "")
            dialog.close()
        else
            dialog.errorText = error
    }

    title: qsTr("Field Options")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(140), Theme.space(140))

    Text {
        Layout.fillWidth: true
        text: dialog.info.name ?? ""
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        elide: Text.ElideRight
    }
    Text {
        objectName: "fieldOptionsFootage"
        Layout.fillWidth: true
        text: dialog.footageText
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        elide: Text.ElideRight
    }
    Toggle {
        objectName: "fieldOptionsReverse"
        text: qsTr("Reverse Field Dominance")
        checked: dialog.reverseDominance
        onToggled: dialog.reverseDominance = checked
    }
    SectionHeader { text: qsTr("Processing Options") }
    EditingChoice {
        objectName: "fieldOptionsProcessing"
        options: dialog.processingOptions
        current: dialog.processing
        onChosen: id => dialog.processing = id
        Accessible.name: qsTr("Processing Options")
    }
    Text {
        objectName: "fieldOptionsError"
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
            objectName: "fieldOptionsOk"
            variant: "primary"
            text: qsTr("OK")
            enabled: dialog.info.available === true
            onClicked: dialog.apply()
        }
    ]
}
