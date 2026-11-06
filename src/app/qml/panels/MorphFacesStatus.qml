pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// 0.7 (docs/motion.md "Morph Cut and faces"): the state of a Morph Cut's face detector in its Effect Controls header, and what to do about it
// (the `AiVideo` singleton): download the detector (YuNet, 0.2 MB) after its licence card, or see that it is ready. Invisible for every
// other transition. Without the detector, or with Face Aware off, the Morph Cut follows the whole picture, as it always did.
RowLayout {
    id: root

    readonly property var info: ProjectController.revision >= 0 && AiVideo.revision >= 0 ? AiVideo.morphInfo() : ({})
    readonly property string phase: root.info.state ?? ""
    readonly property bool downloadingMine: AiVideo.downloading && AiVideo.downloadSection === "morph"

    visible: root.info.morphCut === true
    spacing: Theme.space(1)

    Text {
        objectName: "morphFacesStatus"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        elide: Text.ElideRight
        text: root.downloadingMine ? qsTr("Downloading %1%").arg(Math.round(AiVideo.downloadProgress * 100))
              : root.phase === "ready" ? qsTr("Faces: on")
              : root.phase === "needsModel" ? qsTr("Face detector needed")
              : root.phase === "noHelper" ? qsTr("No AI helper")
              : root.phase === "off" ? qsTr("Faces: off")
              : ""
        color: root.downloadingMine ? Theme.accentStroke
               : root.phase === "ready" ? Theme.textSecondary
               : root.phase === "needsModel" || root.phase === "noHelper" ? Theme.warning
               : Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        Accessible.role: Accessible.StaticText
        Accessible.name: text
        Accessible.description: root.info.message ?? ""
    }

    PillButton {
        objectName: "morphFacesDownload"
        compact: true
        visible: root.phase === "needsModel" && !root.downloadingMine
        variant: "secondary"
        enabled: !AiVideo.downloading && !AiVideo.busy
        text: root.info.downloadText ?? qsTr("Download")
        tooltip: root.info.message ?? ""
        onClicked: card.open()
    }

    PillButton {
        objectName: "morphFacesCancel"
        compact: true
        visible: root.downloadingMine
        variant: "ghost"
        text: qsTr("Cancel")
        onClicked: AiVideo.cancelDownload()
    }

    // The licence of the detector, before its download.
    WabcutDialog {
        id: card
        objectName: "morphFacesDialog"
        title: qsTr("Download the face detector")

        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            text: qsTr("Morph Cut uses a small face detector to line the faces on both sides of the cut up. It runs on this computer: no picture leaves it.")
        }
        Rectangle {
            objectName: "morphFacesLicense"
            Layout.fillWidth: true
            implicitHeight: licenseText.implicitHeight + Theme.space(4)
            radius: Theme.radiusCard
            color: Theme.sunken
            border.width: Theme.hairline
            border.color: Theme.border
            Text {
                id: licenseText
                anchors.fill: parent
                anchors.margins: Theme.space(2)
                wrapMode: Text.Wrap
                text: root.info.card ?? ""
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
        }
        footerData: [
            PillButton {
                variant: "ghost"
                text: qsTr("Cancel")
                onClicked: card.close()
            },
            PillButton {
                objectName: "morphFacesDownloadConfirm"
                variant: "primary"
                text: root.info.downloadText ?? qsTr("Download")
                onClicked: {
                    AiVideo.downloadModels("morph")
                    card.close()
                }
            }
        ]
    }
}
