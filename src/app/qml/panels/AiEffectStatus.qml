pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// 0.7 (docs/ai.md "AI video effects"): the state of an AI Restore or AI Denoise in the Effect Controls header, and what to do
// about it (the `AiVideo` singleton): download its models after their licence card, Process the clip's pictures in the
// background (progress, Cancel; the playhead frame shows each picture as it is made), or delete them. Invisible for other
// effects. Unprocessed frames play as they are, with a badge on the Program Monitor, and an export makes what is missing.
RowLayout {
    id: root

    /// The effect's section key ("e12").
    required property string section
    readonly property var info: ProjectController.revision >= 0 && AiVideo.revision >= 0 && root.section !== ""
                                ? AiVideo.effectInfo(root.section) : ({})
    readonly property string phase: root.info.state ?? ""
    readonly property bool mine: AiVideo.busy && (AiVideo.section === root.section || AiVideo.section === "*")
    readonly property bool downloadingMine: AiVideo.downloading && AiVideo.downloadSection === root.section

    visible: root.info.ai === true
    spacing: Theme.space(1)

    function statusText() {
        if (root.downloadingMine)
            return qsTr("Downloading %1%").arg(Math.round(AiVideo.downloadProgress * 100))
        if (root.mine)
            return qsTr("Processing %1%").arg(Math.round(AiVideo.progress * 100))
        switch (root.phase) {
        case "ready": return qsTr("Processed")
        case "partial": return qsTr("Processed %1%").arg(root.info.percent ?? 0)
        case "notProcessed": return qsTr("Not processed")
        case "needsModel": return qsTr("Model needed")
        case "noHelper": return qsTr("No AI helper")
        case "inactive": return qsTr("No change")
        case "unsupported": return qsTr("Not available")
        }
        return ""
    }
    function tooltipText() {
        const parts = [root.info.message ?? ""]
        if (root.mine)
            parts[0] = AiVideo.message
        else if ((root.info.estimate ?? "") !== "" && (root.phase === "notProcessed" || root.phase === "partial"))
            parts.push(qsTr("Processing the rest takes %1 on this computer.").arg(root.info.estimate))
        if ((root.info.outputSize ?? "") !== "" && root.phase !== "unsupported" && root.phase !== "inactive")
            parts.push(qsTr("Output: %1 pixels").arg(root.info.outputSize))
        if (!root.mine && AiVideo.message !== "" && !AiVideo.busy)
            parts.push(AiVideo.message)
        return parts.filter(p => p !== "").join("\n")
    }

    Text {
        objectName: "aiVideoStatus"
        Layout.maximumWidth: Theme.space(40)
        Layout.fillWidth: true // in a narrow panel the status is what gives way (it elides), not the effect's name
        Layout.minimumWidth: 0
        elide: Text.ElideRight
        text: root.statusText()
        color: root.mine || root.downloadingMine ? Theme.accentStroke
               : root.phase === "ready" ? Theme.textSecondary
               : root.phase === "needsModel" || root.phase === "noHelper" || root.phase === "partial" ? Theme.warning
               : Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        Accessible.role: Accessible.StaticText
        Accessible.name: text
        Accessible.description: root.tooltipText()
    }

    PillButton {
        objectName: "aiVideoProcess"
        compact: true
        visible: root.phase === "notProcessed" || root.phase === "partial" || root.mine || root.downloadingMine
        variant: root.mine || root.downloadingMine ? "ghost" : "secondary"
        enabled: root.mine || root.downloadingMine || (!AiVideo.busy && !AiVideo.downloading)
        text: root.mine || root.downloadingMine ? qsTr("Cancel") : root.phase === "partial" ? qsTr("Continue") : qsTr("Process")
        tooltip: root.mine ? AiVideo.message : root.tooltipText()
        onClicked: {
            if (root.downloadingMine)
                AiVideo.cancelDownload()
            else if (root.mine)
                AiVideo.cancel()
            else
                AiVideo.process(root.section)
        }
    }

    PillButton {
        objectName: "aiVideoDownload"
        compact: true
        visible: root.phase === "needsModel" && !root.downloadingMine
        variant: "secondary"
        enabled: !AiVideo.downloading && !AiVideo.busy
        text: root.info.downloadText ?? qsTr("Download")
        tooltip: qsTr("The models run on this computer. Shows their licences first.")
        onClicked: card.open()
    }

    PillButton {
        objectName: "aiVideoDiscard"
        compact: true
        visible: root.phase === "ready" || root.phase === "partial"
        variant: "ghost"
        enabled: !AiVideo.busy
        text: qsTr("Delete")
        tooltip: qsTr("Delete the processed pictures of this clip's media (Process makes them again)")
        onClicked: AiVideo.discard(root.section)
    }

    // The licences of the models, before their first download.
    WabcutDialog {
        id: card
        objectName: "aiVideoModelDialog"
        title: qsTr("Download AI models")

        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            text: qsTr("%1 needs %2. They run on this computer: no picture leaves it.")
                      .arg(root.info.label ?? "").arg((root.info.models ?? []).join(", "))
        }
        Rectangle {
            objectName: "aiVideoLicense"
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
                objectName: "aiVideoDownloadConfirm"
                variant: "primary"
                text: root.info.downloadText ?? qsTr("Download")
                onClicked: {
                    AiVideo.downloadModels(root.section)
                    card.close()
                }
            }
        ]
    }
}
