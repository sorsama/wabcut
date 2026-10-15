pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Redactions of the Source Monitor's source (I.6, docs/masks.md): how the next marked region looks and sounds, and
// every redaction of this media item - its media time range, style, sound, on/off, "don't redact on the selected
// clip" and delete. Redactions are anchored in the source: they apply to every use of the range in every sequence.
Popover {
    id: root
    objectName: "redactionsPopover"
    title: qsTr("Redactions")
    width: Theme.space(96)

    ColumnLayout {
        width: root.width - 2 * root.padding
        spacing: Theme.space(2)

        Text {
            Layout.fillWidth: true
            text: qsTr("Drag over the picture with the redaction tool to cover a region from In to Out (or the whole clip). It stays covered wherever this source is used.")
            wrapMode: Text.Wrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Text {
                text: qsTr("New")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            PillTabs {
                objectName: "redactStyleTabs"
                compact: true
                Accessible.name: qsTr("New redaction look")
                model: [qsTr("Blur"), qsTr("Mosaic"), qsTr("Solid")]
                currentIndex: ["blur", "mosaic", "solid"].indexOf(Masks.redactStyle)
                onActivated: index => Masks.redactStyle = ["blur", "mosaic", "solid"][index]
            }
            PillTabs {
                objectName: "redactAudioTabs"
                compact: true
                Accessible.name: qsTr("New redaction sound")
                model: [qsTr("Sound"), qsTr("Mute"), qsTr("Bleep")]
                currentIndex: ["none", "mute", "bleep"].indexOf(Masks.redactAudio)
                onActivated: index => Masks.redactAudio = ["none", "mute", "bleep"][index]
            }
        }

        // AI-14: the box marks an object, which SAM 2.1 follows from In to Out (on this computer).
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Toggle {
                objectName: "redactTrackObject"
                text: qsTr("Track an object (AI)")
                tooltip: qsTr("Drag a box around a face, a plate or a screen: it is followed through the shot")
                checked: ObjectMasks.trackRedactions
                enabled: ObjectMasks.available
                onToggled: ObjectMasks.trackRedactions = checked
            }
            Item {
                Layout.fillWidth: true
            }
            PillButton {
                visible: !ObjectMasks.available && !ObjectMasks.downloading
                compact: true
                iconName: "download"
                text: qsTr("Get SAM 2.1")
                tooltip: ObjectMasks.modelCard
                onClicked: ObjectMasks.downloadModel()
            }
        }
        BeamProgress {
            Layout.fillWidth: true
            visible: ObjectMasks.busy || ObjectMasks.downloading
            active: visible
            indeterminate: ObjectMasks.busy && ObjectMasks.progress <= 0
            value: ObjectMasks.downloading ? ObjectMasks.downloadProgress : ObjectMasks.progress
            label: ObjectMasks.downloading ? qsTr("Downloading SAM 2.1") : ObjectMasks.activity
        }
        Text {
            Layout.fillWidth: true
            visible: ObjectMasks.trackRedactions && ObjectMasks.status !== ""
            text: ObjectMasks.status
            wrapMode: Text.Wrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        Text {
            visible: Masks.redactions.length === 0
            text: qsTr("No redactions in this source yet.")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        Repeater {
            model: Masks.redactions
            delegate: Rectangle {
                id: row
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.rowHeight * 2
                radius: Theme.radiusField
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: Theme.border
                Accessible.role: Accessible.ListItem
                Accessible.name: modelData.name

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.space(1)
                    spacing: 0

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(1)
                        Icon {
                            name: "eye-off"
                            size: Theme.iconSm
                            color: row.modelData.enabled ? Theme.danger : Theme.textDisabled
                        }
                        Text {
                            Layout.fillWidth: true
                            text: row.modelData.name
                            elide: Text.ElideRight
                            color: row.modelData.enabled ? Theme.textPrimary : Theme.textDisabled
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                        Text {
                            text: row.modelData.range
                            color: Theme.textTertiary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                        Toggle {
                            checked: row.modelData.enabled
                            tooltip: qsTr("Redaction on or off")
                            onToggled: Masks.setRedactionEnabled(row.modelData.key, checked)
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(1)
                        PillTabs {
                            compact: true
                            Accessible.name: qsTr("Look of %1").arg(row.modelData.name)
                            model: [qsTr("Blur"), qsTr("Mosaic"), qsTr("Solid")]
                            currentIndex: ["blur", "mosaic", "solid"].indexOf(row.modelData.style)
                            onActivated: index => Masks.setRedactionStyle(row.modelData.key, ["blur", "mosaic", "solid"][index])
                        }
                        PillTabs {
                            compact: true
                            Accessible.name: qsTr("Sound of %1").arg(row.modelData.name)
                            model: [qsTr("Sound"), qsTr("Mute"), qsTr("Bleep")]
                            currentIndex: ["none", "mute", "bleep"].indexOf(row.modelData.audio)
                            onActivated: index => Masks.setRedactionAudio(row.modelData.key, ["none", "mute", "bleep"][index])
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                        IconButton {
                            compact: true
                            iconName: "unlink"
                            tooltip: qsTr("Don't redact on the clip selected in the timeline")
                            onClicked: Masks.setRedactionOverride(row.modelData.key, false)
                        }
                        IconButton {
                            compact: true
                            iconName: "trash-2"
                            tooltip: qsTr("Delete the redaction everywhere")
                            onClicked: Masks.removeRedaction(row.modelData.key)
                        }
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: Masks.status !== ""
            text: Masks.status
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }
}
