pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The effect stack of a mixer strip (M5g): one slot per effect (dot on = active, dimmed = bypassed) and an "Add"
// slot. A slot's menu bypasses, moves or removes it; Add lists the audio effects (AudioMixer.effectTypes). Every
// change is an audio.* command on the track or the master (one undo step).
// M5m (A.6): audio plugins too: in the Add menu, dropped from the Effects panel onto the strip, a plugin slot's name,
// badge and "Edit plugin…" (bridge `Plugins`).
ColumnLayout {
    id: root

    /// "t12" (a track) or "master".
    property string target: ""
    property var effects: []
    property string message: ""

    spacing: Theme.space(1) / 2

    function run(error) {
        root.message = error
    }

    Repeater {
        model: root.effects
        delegate: AbstractButton {
            id: slot
            required property var modelData
            required property int index
            readonly property var pluginInfo: String(modelData.effect ?? "").startsWith("plugin.") && Plugins.revision >= 0
                                              ? Plugins.effectInfo(modelData.key) : ({})
            readonly property bool isPlugin: pluginInfo.plugin === true
            readonly property string slotName: isPlugin ? (pluginInfo.name ?? modelData.name) : modelData.name
            objectName: "effectSlot_" + index
            Layout.fillWidth: true
            implicitHeight: Theme.controlHeightSmall - Theme.space(1)
            hoverEnabled: true
            focusPolicy: Qt.StrongFocus
            Accessible.name: modelData.enabled ? qsTr("%1 effect").arg(slotName) : qsTr("%1 effect (bypassed)").arg(slotName)
            onClicked: slotMenu.popup(slot, 0, slot.height)

            background: Rectangle {
                radius: Theme.radiusBadge
                color: slot.hovered ? Theme.hover : slot.modelData.enabled ? Theme.raised : Theme.sunken
                border.width: Theme.hairline
                border.color: Theme.border
            }
            contentItem: RowLayout {
                spacing: Theme.space(1)
                Rectangle {
                    Layout.leftMargin: Theme.space(1)
                    implicitWidth: Theme.dotSize
                    implicitHeight: Theme.dotSize
                    radius: width / 2
                    color: slot.modelData.enabled ? Theme.accentStroke : Theme.textDisabled
                }
                Text {
                    Layout.fillWidth: true
                    text: slot.slotName
                    elide: Text.ElideRight
                    color: slot.modelData.enabled ? Theme.textPrimary : Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                    font.strikeout: !slot.modelData.enabled
                }
                PluginBadge {
                    Layout.rightMargin: Theme.space(1)
                    kind: slot.pluginInfo.kind ?? ""
                    detail: slot.pluginInfo.text ?? ""
                    missedBlocks: slot.pluginInfo.missedBlocks ?? 0
                }
            }
            FocusRing {
                targetRadius: Theme.radiusBadge
                visible: slot.visualFocus
            }
            HoverHandler {
                enabled: slot.enabled
                cursorShape: Qt.PointingHandCursor
            }
            Loader {
                active: slot.hovered && slot.modelData.summary !== ""
                sourceComponent: Tooltip {
                    parent: slot
                    visible: true
                    text: qsTr("%1 · %2").arg(slot.slotName).arg(slot.modelData.summary)
                }
            }

            OptionsMenu {
                id: slotMenu
                objectName: "effectSlotMenu"
                options: (slot.isPlugin && slot.pluginInfo.hasEditor === true ? [{ id: "editPlugin", name: qsTr("Edit plugin…") }] : []).concat([
                    { id: "toggle", name: slot.modelData.enabled ? qsTr("Bypass") : qsTr("Turn on") },
                    { id: "up", name: qsTr("Move up") },
                    { id: "down", name: qsTr("Move down") },
                    { id: "remove", name: qsTr("Remove") }
                ])
                onChosen: id => {
                    if (id === "editPlugin")
                        root.run(Plugins.openEditor(slot.modelData.key))
                    else if (id === "toggle")
                        root.run(AudioMixer.setEffectEnabled(root.target, slot.modelData.key, !slot.modelData.enabled))
                    else if (id === "up" && slot.index > 0)
                        root.run(AudioMixer.moveEffect(root.target, slot.modelData.key, slot.index - 1))
                    else if (id === "down" && slot.index + 1 < root.effects.length)
                        root.run(AudioMixer.moveEffect(root.target, slot.modelData.key, slot.index + 1))
                    else if (id === "remove")
                        root.run(AudioMixer.removeEffect(root.target, slot.modelData.key))
                }
            }
        }
    }

    // Add an effect
    AbstractButton {
        id: addSlot
        objectName: "effectSlotAdd"
        Layout.fillWidth: true
        implicitHeight: Theme.controlHeightSmall - Theme.space(1)
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.name: qsTr("Add an effect")
        onClicked: addMenu.popup(addSlot, 0, addSlot.height)

        background: Rectangle {
            radius: Theme.radiusBadge
            color: addSlot.hovered ? Theme.hover : "transparent"
            border.width: Theme.hairline
            border.color: Theme.border
        }
        contentItem: RowLayout {
            spacing: Theme.space(1)
            Icon {
                Layout.leftMargin: Theme.space(1)
                name: "plus"
                size: Theme.iconXs
                color: Theme.textTertiary
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Effect")
                elide: Text.ElideRight
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
        }
        FocusRing {
            targetRadius: Theme.radiusBadge
            visible: addSlot.visualFocus
        }
        HoverHandler {
            enabled: addSlot.enabled
            cursorShape: Qt.PointingHandCursor
        }

        OptionsMenu {
            id: addMenu
            objectName: "effectAddMenu"
            // The built-in audio effects, then the audio plugins ("Name · CLAP").
            options: AudioMixer.effectTypes.concat(Plugins.available ? Plugins.allPlugins.map(p => ({ id: p.id, name: qsTr("%1 · %2").arg(p.name).arg(p.format) })) : [])
            onChosen: id => root.run(id.startsWith("plugin.") ? Plugins.apply(id, root.target) : AudioMixer.addEffect(root.target, id))
        }

        // An audio plugin dragged from the Effects panel onto the strip (covers the whole stack, not just this slot).
        DropArea {
            id: pluginDrop
            objectName: "effectSlotsDropArea"
            x: -addSlot.x
            y: -addSlot.y
            width: root.width
            height: root.height
            keys: ["application/x-wabcut-effect"]
            onEntered: drag => {
                drag.accepted = drag.getDataAsString("application/x-wabcut-effect").startsWith("plugin.")
            }
            onDropped: drop => {
                const error = Plugins.apply(drop.getDataAsString("application/x-wabcut-effect"), root.target)
                root.run(error)
                if (error === "")
                    drop.accept(Qt.CopyAction)
            }
            Rectangle {
                anchors.fill: parent
                visible: pluginDrop.containsDrag
                radius: Theme.radiusBadge
                color: Theme.accentWash
                border.width: Theme.selectionWidth
                border.color: Theme.accentStroke
            }
        }
    }
}
