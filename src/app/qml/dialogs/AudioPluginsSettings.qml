pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts

// Settings > Audio plugins (A.6, bridge `Plugins`): the folders scanned for CLAP and VST3 effects (the standard ones and
// the user's, which can be removed), Rescan, and the blocklist: plugin files that crashed or hung while they were
// scanned, each with its reason and "Try again". Scanning runs every file in its own helper process, so a bad plugin
// never takes the editor down.
WabcutDialog {
    id: dialog
    objectName: "audioPluginsSettings"

    title: qsTr("Audio plugins")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(150), Theme.space(150))

    component Caption: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.Medium
    }

    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        text: Plugins.available
              ? qsTr("Wabcut uses CLAP and VST3 effects. Each plugin runs in a helper process: one that crashes is disabled with a badge, and the editor carries on.")
              : qsTr("Audio plugins (CLAP and VST3) are not available in this build.")
    }

    // ---- Folders ----
    Caption {
        visible: Plugins.available
        text: qsTr("Folders")
    }
    Repeater {
        model: Plugins.available ? Plugins.folders : []
        delegate: RowLayout {
            id: folderRow
            required property var modelData
            required property int index
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Icon {
                name: "folder"
                size: Theme.iconSm
                color: Theme.textTertiary
            }
            Text {
                Layout.fillWidth: true
                text: folderRow.modelData.path
                elide: Text.ElideMiddle
                color: Theme.textPrimary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textSm
            }
            Text {
                visible: !folderRow.modelData.user
                text: qsTr("Standard")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
            IconButton {
                objectName: "removePluginFolder_" + folderRow.index
                visible: folderRow.modelData.user
                compact: true
                iconName: "x"
                iconSize: Theme.iconSm
                tooltip: qsTr("Stop scanning this folder")
                onClicked: dialog.show(Plugins.removeFolder(folderRow.modelData.path))
            }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        visible: Plugins.available
        spacing: Theme.space(2)
        PillButton {
            objectName: "addPluginFolder"
            compact: true
            variant: "ghost"
            iconName: "folder-plus"
            text: qsTr("Add folder…")
            onClicked: folderDialog.open()
        }
        Item { Layout.fillWidth: true }
        PillButton {
            objectName: "rescanAllPlugins"
            compact: true
            variant: "ghost"
            enabled: !Plugins.scanning
            text: qsTr("Scan all again")
            tooltip: qsTr("Scan every plugin file again, also the unchanged ones")
            onClicked: dialog.show(Plugins.rescan(true))
        }
        PillButton {
            objectName: "rescanPlugins"
            compact: true
            iconName: "rotate-cw"
            enabled: !Plugins.scanning
            text: qsTr("Rescan")
            onClicked: dialog.show(Plugins.rescan())
        }
    }
    BeamProgress {
        Layout.fillWidth: true
        Layout.preferredHeight: Theme.hairline * 3
        visible: Plugins.scanning
        indeterminate: Plugins.progress < 0
        value: Math.max(0, Plugins.progress)
        active: Plugins.scanning
        label: Plugins.scanText
    }

    // ---- The blocklist ----
    Caption {
        visible: Plugins.available
        text: Plugins.blocked.length > 0 ? qsTr("Blocked · %1").arg(Plugins.blocked.length) : qsTr("Blocked")
    }
    Text {
        Layout.fillWidth: true
        visible: Plugins.available && Plugins.blocked.length === 0
        text: qsTr("No plugin is blocked.")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    Repeater {
        model: Plugins.available ? Plugins.blocked : []
        delegate: Rectangle {
            id: blockedRow
            required property var modelData
            required property int index
            Layout.fillWidth: true
            implicitHeight: blockedColumn.implicitHeight + Theme.space(3)
            radius: Theme.radiusField
            color: Theme.sunken
            border.width: Theme.hairline
            border.color: Theme.border
            RowLayout {
                anchors.fill: parent
                anchors.margins: Theme.space(1.5)
                spacing: Theme.space(2)
                Icon {
                    Layout.alignment: Qt.AlignTop
                    name: "triangle-alert"
                    size: Theme.iconSm
                    color: Theme.warning
                }
                ColumnLayout {
                    id: blockedColumn
                    Layout.fillWidth: true
                    spacing: 0
                    Text {
                        Layout.fillWidth: true
                        text: blockedRow.modelData.name
                        elide: Text.ElideMiddle
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        font.weight: Font.Medium
                    }
                    Text {
                        Layout.fillWidth: true
                        text: blockedRow.modelData.reason
                        wrapMode: Text.Wrap
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                }
                PillButton {
                    objectName: "unblockPlugin_" + blockedRow.index
                    compact: true
                    variant: "ghost"
                    enabled: !Plugins.scanning
                    text: qsTr("Try again")
                    tooltip: qsTr("Take it off the blocklist and scan it again")
                    onClicked: dialog.show(Plugins.unblock(blockedRow.modelData.file))
                }
            }
        }
    }

    Text {
        id: statusText
        objectName: "audioPluginsMessage"
        Layout.fillWidth: true
        visible: text !== ""
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        text: Plugins.message
    }

    /// An action's error, or the bridge's last message.
    function show(error) {
        if (error !== "")
            statusText.text = error
        else
            statusText.text = Qt.binding(() => Plugins.message)
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Add a plugin folder")
        onAccepted: dialog.show(Plugins.addFolder(selectedFolder))
    }

    footerData: [
        PillButton {
            objectName: "closeAudioPluginsSettings"
            variant: "primary"
            text: qsTr("Close")
            onClicked: dialog.close()
        }
    ]
}
