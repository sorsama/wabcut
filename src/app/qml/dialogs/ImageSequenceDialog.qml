pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts

// Import Image Sequence (0.2; Premiere's File > Import with "Image Sequence" ticked): the first of a run of numbered
// stills (PNG, JPEG, TIFF, DPX, Targa, OpenEXR, BMP) and the frame rate they play at become one clip in the current
// bin (Library.importImageSequence -> media.importImageSequence). 29.97 fps is Premiere's Indeterminate Media Timebase
// default. Opened from the Library's tools menu.
WabcutDialog {
    id: dialog
    objectName: "imageSequenceDialog"

    property url file: ""
    property int rateIndex: 3
    property string errorText: ""
    readonly property var rates: [23.976, 24, 25, 29.97, 30, 50, 59.94, 60]

    function openDialog() {
        file = ""
        errorText = ""
        open()
    }
    function apply() {
        const key = Library.importImageSequence(file, rates[rateIndex])
        if (key === "") {
            errorText = Library.message !== "" ? Library.message : qsTr("The sequence could not be imported")
        } else {
            close()
        }
    }

    title: qsTr("Import Image Sequence")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(140), Theme.space(140))

    Text {
        Layout.fillWidth: true
        text: qsTr("Choose the first numbered still. It and the files numbered after it, up to the first gap, become one clip.")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(3)

        Text {
            objectName: "imageSequenceFile"
            Layout.fillWidth: true
            text: dialog.file.toString() === "" ? qsTr("No file chosen")
                                                : decodeURIComponent(dialog.file.toString().split("/").pop())
            color: dialog.file.toString() === "" ? Theme.textTertiary : Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            elide: Text.ElideMiddle
        }
        PillButton {
            objectName: "imageSequenceChoose"
            variant: "ghost"
            text: qsTr("Choose…")
            onClicked: fileDialog.open()
        }
    }

    Text {
        Layout.fillWidth: true
        text: qsTr("Frame rate (fps)")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    PillTabs {
        objectName: "imageSequenceRate"
        Layout.fillWidth: true
        compact: true
        model: dialog.rates.map(r => String(r))
        currentIndex: dialog.rateIndex
        onActivated: index => dialog.rateIndex = index
    }

    Text {
        objectName: "imageSequenceError"
        Layout.fillWidth: true
        visible: text !== ""
        text: dialog.errorText
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    FileDialog {
        id: fileDialog
        title: qsTr("The first image of the sequence")
        nameFilters: [qsTr("Still images (*.png *.jpg *.jpeg *.tif *.tiff *.dpx *.tga *.exr *.bmp)"), qsTr("All files (*)")]
        onAccepted: {
            dialog.file = selectedFile
            dialog.errorText = ""
        }
    }

    footerData: [
        PillButton {
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "imageSequenceOk"
            variant: "primary"
            text: qsTr("Import")
            enabled: dialog.file.toString() !== ""
            onClicked: dialog.apply()
        }
    ]
}
