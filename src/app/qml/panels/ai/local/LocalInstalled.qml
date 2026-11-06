pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs

// Settings > AI > Local models > Downloaded: where models are kept (the storage folder, with what they use and what
// is free there; Change, Default, Open), and every downloaded model with its size: Use, Remove (asks first), and for
// assistant models an Advanced section with the launch settings (context size, GPU layers, CPU threads), which apply
// the next time the assistant starts with that model.
Column {
    id: root
    objectName: "localInstalled"

    property string confirmRemove: ""
    property string advancedFor: ""

    spacing: Theme.space(3)

    // ---- Storage folder ----
    Rectangle {
        objectName: "localStorage"
        width: parent.width
        implicitHeight: storageColumn.implicitHeight + Theme.space(6)
        radius: Theme.radiusCard
        color: Theme.section
        border.width: Theme.hairline
        border.color: Theme.border

        Column {
            id: storageColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.space(3)
            spacing: Theme.space(2)

            Text {
                text: qsTr("Storage folder")
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Font.Bold
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }
            Text {
                objectName: "localStoragePath"
                width: parent.width
                wrapMode: Text.WrapAnywhere
                text: LocalModels.storageFolder
                color: Theme.textSecondary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
            }
            Text {
                objectName: "localStorageText"
                width: parent.width
                wrapMode: Text.WordWrap
                text: LocalModels.storageText
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Flow {
                width: parent.width
                spacing: Theme.space(2)

                PillButton {
                    objectName: "localStorageChange"
                    compact: true
                    iconName: "folder-open"
                    text: qsTr("Change…")
                    onClicked: folderDialog.open()
                }
                PillButton {
                    objectName: "localStorageDefault"
                    compact: true
                    visible: LocalModels.customStorage
                    text: qsTr("Use the default folder")
                    onClicked: LocalModels.setStorageFolder("")
                }
                PillButton {
                    objectName: "localStorageOpen"
                    compact: true
                    variant: "ghost"
                    iconName: "external-link"
                    text: qsTr("Open folder")
                    onClicked: Qt.openUrlExternally(LocalModels.storageFolderUrl())
                }
            }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("New downloads go to this folder. Models already downloaded stay in the folder they were downloaded to: move them there yourself, or download them again.")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Folder for local AI models")
        onAccepted: {
            if (!LocalModels.setStorageFolder(selectedFolder.toString()))
                LocalModels.notice(qsTr("Wabcut cannot write to that folder. Choose another one."))
        }
    }

    Text {
        visible: LocalModels.installed.length === 0
        width: parent.width
        wrapMode: Text.WordWrap
        text: qsTr("No local model is downloaded yet. Recommended and Browse download them; Transcription picks the speech model.")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    Repeater {
        model: LocalModels.installed

        delegate: Rectangle {
            id: installedItem
            required property var modelData
            readonly property bool assistant: modelData.kind === "assistant"
            property var launch: ({ context: 0, gpuLayers: -1, threads: 0 })

            objectName: "localInstalled_" + modelData.id
            width: root.width
            implicitHeight: installedColumn.implicitHeight + Theme.space(6)
            radius: Theme.radiusCard
            color: Theme.section
            border.width: Theme.hairline
            border.color: modelData.missing ? Theme.warning : Theme.border
            Accessible.role: Accessible.Grouping
            Accessible.name: modelData.name

            Column {
                id: installedColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Theme.space(3)
                spacing: Theme.space(1.5)

                Flow {
                    width: parent.width
                    spacing: Theme.space(1.5)

                    Text {
                        width: Math.min(implicitWidth, installedColumn.width)
                        wrapMode: Text.WordWrap
                        text: installedItem.modelData.name
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        font.weight: Font.DemiBold
                    }
                    LocalTag {
                        text: installedItem.assistant ? qsTr("Assistant") : qsTr("Transcription")
                    }
                    LocalTag {
                        text: installedItem.modelData.size
                    }
                    LocalTag {
                        visible: installedItem.modelData.inUse
                        text: qsTr("In use")
                        tone: "success"
                    }
                    LocalTag {
                        visible: installedItem.modelData.vision
                        text: qsTr("Vision")
                    }
                    LocalTag {
                        visible: installedItem.modelData.missing
                        text: qsTr("Files missing")
                        tone: "warning"
                    }
                }
                Text {
                    width: parent.width
                    elide: Text.ElideMiddle
                    text: installedItem.modelData.detail + " · " + installedItem.modelData.license
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                }

                Flow {
                    width: parent.width
                    spacing: Theme.space(2)

                    PillButton {
                        objectName: "localInstalledUse"
                        compact: true
                        visible: !installedItem.modelData.inUse && !installedItem.modelData.missing
                        text: installedItem.assistant ? qsTr("Use for assistant") : qsTr("Use for transcription")
                        Accessible.name: text + " " + installedItem.modelData.name
                        onClicked: {
                            const message = installedItem.assistant ? LocalModels.useForAssistant(installedItem.modelData.id)
                                                            : LocalModels.useForTranscription(installedItem.modelData.id)
                            if (message !== "")
                                LocalModels.notice(message)
                        }
                    }
                    PillButton {
                        objectName: "localInstalledAdvanced"
                        compact: true
                        variant: "ghost"
                        visible: installedItem.assistant
                        iconName: root.advancedFor === installedItem.modelData.id ? "chevron-up" : "sliders-horizontal"
                        text: qsTr("Advanced")
                        Accessible.name: qsTr("Advanced settings of %1").arg(installedItem.modelData.name)
                        onClicked: {
                            root.advancedFor = root.advancedFor === installedItem.modelData.id ? "" : installedItem.modelData.id
                            installedItem.launch = LocalModels.launchOptions(installedItem.modelData.id)
                        }
                    }
                    PillButton {
                        objectName: "localInstalledRemove"
                        compact: true
                        variant: "ghost"
                        visible: root.confirmRemove !== installedItem.modelData.id
                        iconName: "trash-2"
                        text: qsTr("Remove")
                        Accessible.name: qsTr("Remove %1").arg(installedItem.modelData.name)
                        onClicked: root.confirmRemove = installedItem.modelData.id
                    }
                }

                // Remove: asks first, with what it frees.
                Flow {
                    visible: root.confirmRemove === installedItem.modelData.id
                    width: parent.width
                    spacing: Theme.space(2)

                    Text {
                        width: Math.min(implicitWidth, installedColumn.width)
                        wrapMode: Text.WordWrap
                        text: qsTr("Delete %1 from this computer? It frees %2.").arg(installedItem.modelData.name).arg(installedItem.modelData.size)
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    PillButton {
                        objectName: "localInstalledRemoveConfirm"
                        compact: true
                        variant: "danger"
                        text: qsTr("Delete")
                        Accessible.name: qsTr("Delete %1").arg(installedItem.modelData.name)
                        onClicked: {
                            const message = LocalModels.remove(installedItem.modelData.id)
                            root.confirmRemove = ""
                            if (message !== "")
                                LocalModels.notice(message)
                        }
                    }
                    PillButton {
                        compact: true
                        text: qsTr("Keep")
                        onClicked: root.confirmRemove = ""
                    }
                }

                // Advanced launch settings.
                Column {
                    objectName: "localLaunchOptions"
                    visible: root.advancedFor === installedItem.modelData.id
                    width: parent.width
                    spacing: Theme.space(2)

                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: qsTr("Empty means automatic. They apply the next time the assistant starts with this model.")
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                    }
                    Flow {
                        width: parent.width
                        spacing: Theme.space(3)

                        Column {
                            spacing: Theme.space(1)
                            Text {
                                text: qsTr("Context (tokens)")
                                color: Theme.textTertiary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textXs
                            }
                            FieldInput {
                                id: contextField
                                objectName: "localContextField"
                                width: Theme.space(32)
                                text: installedItem.launch.context > 0 ? String(installedItem.launch.context) : ""
                                placeholderText: qsTr("Automatic")
                                validator: IntValidator { bottom: 0; top: 131072 }
                                Accessible.name: qsTr("Context size in tokens")
                            }
                        }
                        Column {
                            spacing: Theme.space(1)
                            Text {
                                text: qsTr("GPU layers (0 = CPU only)")
                                color: Theme.textTertiary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textXs
                            }
                            FieldInput {
                                id: layersField
                                objectName: "localGpuLayersField"
                                width: Theme.space(32)
                                text: installedItem.launch.gpuLayers >= 0 ? String(installedItem.launch.gpuLayers) : ""
                                placeholderText: qsTr("Automatic")
                                validator: IntValidator { bottom: 0; top: 999 }
                                Accessible.name: qsTr("Layers on the GPU")
                            }
                        }
                        Column {
                            spacing: Theme.space(1)
                            Text {
                                text: qsTr("CPU threads")
                                color: Theme.textTertiary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textXs
                            }
                            FieldInput {
                                id: threadsField
                                objectName: "localThreadsField"
                                width: Theme.space(32)
                                text: installedItem.launch.threads > 0 ? String(installedItem.launch.threads) : ""
                                placeholderText: qsTr("Automatic")
                                validator: IntValidator { bottom: 0; top: 256 }
                                Accessible.name: qsTr("CPU threads")
                            }
                        }
                    }
                    Flow {
                        width: parent.width
                        spacing: Theme.space(2)
                        PillButton {
                            objectName: "localLaunchSave"
                            compact: true
                            variant: "primary"
                            text: qsTr("Save")
                            onClicked: {
                                const ok = LocalModels.setLaunchOptions(installedItem.modelData.id,
                                                                        contextField.text === "" ? 0 : parseInt(contextField.text),
                                                                        layersField.text === "" ? -1 : parseInt(layersField.text),
                                                                        threadsField.text === "" ? 0 : parseInt(threadsField.text))
                                installedItem.launch = LocalModels.launchOptions(installedItem.modelData.id)
                                LocalModels.notice(ok ? qsTr("Saved. The assistant uses them the next time it starts with %1.").arg(installedItem.modelData.name)
                                                      : qsTr("The settings could not be saved."))
                            }
                        }
                        PillButton {
                            compact: true
                            text: qsTr("Reset to automatic")
                            onClicked: {
                                LocalModels.setLaunchOptions(installedItem.modelData.id, 0, -1, 0)
                                installedItem.launch = LocalModels.launchOptions(installedItem.modelData.id)
                            }
                        }
                    }
                }
            }
        }
    }
}
