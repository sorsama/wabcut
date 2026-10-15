pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
ScrollView {
    id: page
    objectName: "localLlmPage"
    contentWidth: availableWidth
    // The model list is cached by AiController (probing the GPUs is not free): read it again when the page opens.
    Component.onCompleted: AiController.refreshLocalModels()
    onVisibleChanged: if (visible) AiController.refreshLocalModels()
    ColumnLayout {
        width: page.availableWidth
        spacing: Theme.space(4)
        Label { text: qsTr("Local assistant"); color: Theme.textPrimary; font.pixelSize: Theme.textTitle }
        Label { text: qsTr("Your prompts and edits stay on this computer."); color: Theme.textSecondary }
        StatusPill { label: qsTr("Local"); detail: AiController.localStatus; dotColor: AiController.localBusy ? Theme.warning : Theme.accentStroke; Layout.fillWidth: true }
        Repeater {
            model: AiController.localModels
            delegate: Rectangle {
                id: card
                required property var modelData
                Layout.fillWidth: true
                implicitHeight: contents.implicitHeight + Theme.space(8)
                color: Theme.raised; radius: Theme.radiusField
                border.color: Theme.border; border.width: Theme.hairline
                ColumnLayout {
                    id: contents
                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space(4) }
                    Label { text: card.modelData.name; color: Theme.textPrimary; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    Label { text: card.modelData.size + " download · " + card.modelData.license; color: Theme.textSecondary }
                    Label { text: qsTr("RAM %1 · GPU %2 (including playback reserve)").arg(card.modelData.ram).arg(card.modelData.vram); color: Theme.textSecondary; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    Label { text: card.modelData.fit; color: card.modelData.fits ? Theme.success : Theme.warning; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    Label { text: card.modelData.card; color: Theme.textSecondary; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    RowLayout {
                        PillButton { text: card.modelData.installed ? qsTr("Downloaded") : qsTr("Download"); enabled: !card.modelData.installed && !AiController.localBusy && !AiController.offlineMode && !AiController.projectLock; onClicked: AiController.downloadLocal(card.modelData.id) }
                        PillButton { text: qsTr("Start"); enabled: card.modelData.installed && card.modelData.fits && !AiController.localBusy; onClicked: AiController.startLocal(card.modelData.id) }
                        PillButton { text: qsTr("Stop"); onClicked: AiController.stopLocal() }
                    }
                }
            }
        }
        Label { text: qsTr("Download while online first. Offline mode blocks downloads and cloud AI. The assistant unloads while idle and pauses for playback or export."); color: Theme.textSecondary; wrapMode: Text.Wrap; Layout.fillWidth: true }
    }
}
