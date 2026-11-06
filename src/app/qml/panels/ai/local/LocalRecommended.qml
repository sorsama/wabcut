pragma ComponentBehavior: Bound
import QtQuick

// Settings > AI > Local models > Recommended: the curated assistant models Wabcut was tested with, the one this
// computer runs best first. One click downloads the model and its engine and starts the assistant with it; the card's
// Details show its quantization, fit and published benchmarks.
Column {
    id: root
    objectName: "localRecommended"

    signal openDetails()

    spacing: Theme.space(3)

    Text {
        width: parent.width
        wrapMode: Text.WordWrap
        text: qsTr("Tested with Wabcut's assistant. Pick one and it downloads once, with the engine that runs it; after that it works offline.")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    Repeater {
        model: LocalModels.recommended

        delegate: LocalModelCard {
            id: card
            required property var modelData
            row: modelData
            width: root.width
            primaryText: card.download !== null && card.download.status !== "failed" ? ""
                         : modelData.installed ? (modelData.inUse ? "" : qsTr("Use for assistant"))
                         : modelData.fit === "no" ? qsTr("Download anyway")
                         : qsTr("Download and use")
            primaryEnabled: modelData.installed || !LocalModels.offline
            onPrimary: {
                const message = card.modelData.installed ? LocalModels.useForAssistant(card.modelData.id)
                                                         : LocalModels.downloadCurated(card.modelData.id, card.modelData.fit !== "no")
                if (message !== "")
                    LocalModels.notice(message)
            }
            onSecondary: {
                LocalModels.openCurated(card.modelData.id)
                root.openDetails()
            }
        }
    }
}
