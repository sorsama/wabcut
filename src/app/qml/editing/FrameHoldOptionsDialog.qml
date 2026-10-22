pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Frame Hold Options (P.2, E5): Premiere's Clip > Video Options > Frame Hold Options. Hold On a source timecode, a
// sequence timecode, the In point, the Out point or the playhead, with or without Hold Filters (the clip's effects hold
// at that frame too). Off plays the clip normally again from the frame it held. One undo step (clip.frameHoldOptions).
WabcutDialog {
    id: dialog
    objectName: "frameHoldOptionsDialog"

    property string clipKey: ""
    property var info: ({})
    property bool holdEnabled: true
    property string holdOn: "inPoint"
    property bool holdFilters: false
    property string errorText: ""
    readonly property bool needsTime: dialog.holdOn === "sourceTimecode" || dialog.holdOn === "sequenceTimecode"
    readonly property var holdOptions: [
        { id: "sourceTimecode", name: qsTr("Source Timecode") }, { id: "sequenceTimecode", name: qsTr("Sequence Timecode") },
        { id: "inPoint", name: qsTr("In Point") }, { id: "outPoint", name: qsTr("Out Point") },
        { id: "playhead", name: qsTr("Playhead") }
    ]

    function openFor(key) {
        dialog.clipKey = key
        dialog.info = Editing.frameHoldInfo(key)
        dialog.errorText = dialog.info.available === true ? "" : qsTr("Select a video clip")
        dialog.holdEnabled = true
        dialog.holdOn = "inPoint"
        dialog.holdFilters = dialog.info.holdFilters === true
        timeField.text = dialog.info.playhead ?? ""
        open()
    }
    function apply() {
        const options = { enabled: dialog.holdEnabled, holdOn: dialog.holdOn, holdFilters: dialog.holdFilters }
        if (dialog.needsTime)
            options.time = timeField.text.trim()
        const error = Editing.applyFrameHoldOptions(dialog.clipKey, options)
        if (error === "")
            dialog.close()
        else
            dialog.errorText = error
    }

    title: qsTr("Frame Hold Options")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(140), Theme.space(140))

    Text {
        Layout.fillWidth: true
        text: dialog.info.name ?? ""
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        elide: Text.ElideRight
    }
    Toggle {
        objectName: "frameHoldEnabled"
        text: qsTr("Hold On")
        checked: dialog.holdEnabled
        onToggled: dialog.holdEnabled = checked
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)
        enabled: dialog.holdEnabled
        EditingChoice {
            objectName: "frameHoldOn"
            options: dialog.holdOptions
            current: dialog.holdOn
            onChosen: id => dialog.holdOn = id
        }
        FieldInput {
            id: timeField
            objectName: "frameHoldTime"
            Layout.preferredWidth: Theme.space(40)
            visible: dialog.needsTime
            font.family: Theme.fontMono
            Accessible.name: qsTr("Timecode to hold")
        }
    }
    Toggle {
        objectName: "frameHoldFilters"
        text: qsTr("Hold Filters")
        enabled: dialog.holdEnabled
        checked: dialog.holdFilters
        onToggled: dialog.holdFilters = checked
    }
    Text {
        objectName: "frameHoldError"
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
            objectName: "frameHoldOk"
            variant: "primary"
            text: qsTr("OK")
            enabled: dialog.info.available === true
            onClicked: dialog.apply()
        }
    ]
}
