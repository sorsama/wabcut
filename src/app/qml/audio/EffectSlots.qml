pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The effect stack of a mixer strip (M5g): one slot per effect (dot on = active, dimmed = bypassed) and an "Add"
// slot. A slot's menu bypasses, moves or removes it; Add lists the audio effects (AudioMixer.effectTypes). Every
// change is an audio.* command on the track or the master (one undo step).
// M5m (A.6): audio plugins too: in the Add menu, dropped from the Effects panel onto the strip, a plugin slot's name,
// badge and "Edit plugin…" (bridge `Plugins`).
// r07: "Settings…" in a slot's menu (or a double-click) opens the effect's settings (MixerEffectSettings: every
// parameter, live, one undo step each); hovering a slot shows its main values; a slot drags up or down to reorder the
// stack. Enhance Speech (rendered per clip, so it goes on the track's clips through its dialog) shows as a slot of its
// own once the clips have it (`clipEffects`: Settings… reopens the dialog, Bypass and Remove act on every clip). What
// goes wrong - an empty track, a model still to download, a plugin that fails - shows under the slots, with the action
// to take when there is one. Menu actions run after their menu closed (a dialog opened from a closing menu loses focus), from the event loop:
// a change remakes the strips (AudioMixerPanel's Repeaters), menus included.
ColumnLayout {
    id: root

    /// "t12" (a track or submix) or "master".
    property string target: ""
    /// The strip's name for the settings header ("A1").
    property string stripName: target === "master" ? qsTr("Master") : ""
    property var effects: []
    /// r07: clip effects on the track's clips (AudioMixer strips' `clipEffects`).
    property var clipEffects: []
    property string message: ""
    /// r07: what the message's button does: "" (none) or "enhance" (open Enhance Speech for the track's clips).
    property string messageAction: ""
    /// r07, drag to reorder: the slot being dragged and where it would go (-1: no drag).
    property int dragFrom: -1
    property int dropIndex: -1

    spacing: Theme.space(1) / 2

    function run(error) {
        root.message = error
        root.messageAction = ""
    }
    /// Adds a built-in effect or a plugin from the Add menu. Enhance Speech opens its dialog for the track's clips; while
    /// its model still has to be downloaded the strip says so (closing the dialog leaves that step undone).
    function addChosen(id: string) {
        if (id.startsWith("plugin.")) {
            root.run(Plugins.apply(id, root.target))
            return
        }
        const error = AudioMixer.addEffect(root.target, id)
        root.run(error)
        if (error === "" && id === "wabcut.audio.enhanceSpeech" && ClipAudio.models.speechReady !== true) {
            root.message = qsTr("Enhance Speech downloads its speech model (%1) the first time: finish it in the Enhance Speech window.")
                               .arg(ClipAudio.models.speechSize ?? "")
            root.messageAction = "enhance"
        }
    }
    /// 0.7 (card 101): the Presets menu's choice: save the chain (the dialog is the mixer panel's), or add a preset.
    function presetChosen(id: string) {
        if (id === "save") {
            AudioMixer.openSavePreset(root.target, root.stripName !== "" ? root.stripName : qsTr("Effect chain"))
        } else if (id.startsWith("apply:")) {
            root.run(EffectPresets.applyToTrack(id.slice(6), root.target))
        }
    }
    /// The effect's settings: the mixer panel's popup (AudioMixerPanel), as the strips are made again on every change.
    function openSettings(key: string) {
        AudioMixer.openEffectSettings(root.target, key, root.stripName)
    }
    /// The effect slot a point of this item (y) is over, for a drag (clamped to the stack).
    function slotIndexAt(y: real): int {
        for (let i = 0; i < slots.count; ++i) {
            const item = slots.itemAt(i)
            if (item && y < item.y + item.height / 2)
                return i
        }
        return Math.max(0, slots.count - 1)
    }

    onClipEffectsChanged: {
        if (root.messageAction === "enhance" && root.clipEffects.length > 0)
            root.run("")
    }

    // r07: clip effects on the track's clips (Enhance Speech), before the track's own stack (clips play first).
    Repeater {
        model: root.clipEffects
        delegate: AbstractButton {
            id: clipSlot
            required property var modelData
            required property int index
            property string pending: ""
            objectName: "clipEffectSlot_" + index
            Layout.fillWidth: true
            implicitHeight: Theme.controlHeightSmall - Theme.space(1)
            hoverEnabled: true
            focusPolicy: Qt.StrongFocus
            Accessible.name: modelData.enabled ? qsTr("%1 on the track's clips").arg(modelData.name)
                                               : qsTr("%1 on the track's clips (bypassed)").arg(modelData.name)
            // A click opens the menu once it is not a double-click (that opens Enhance Speech).
            onClicked: clipMenuDelay.restart()
            onDoubleClicked: {
                clipMenuDelay.stop()
                ClipAudio.openEnhance(clipSlot.modelData.clips)
            }
            Timer {
                id: clipMenuDelay
                interval: Application.styleHints.mouseDoubleClickInterval
                onTriggered: clipMenu.popup(clipSlot, 0, clipSlot.height)
            }

            function act(id: string) {
                if (id === "settings")
                    ClipAudio.openEnhance(clipSlot.modelData.clips)
                else if (id === "toggle")
                    root.run(AudioMixer.setClipEffectEnabled(root.target, clipSlot.modelData.effect, !clipSlot.modelData.enabled))
                else if (id === "remove")
                    root.run(AudioMixer.removeClipEffect(root.target, clipSlot.modelData.effect))
            }

            background: Rectangle {
                radius: Theme.radiusBadge
                color: clipSlot.hovered ? Theme.hover : clipSlot.modelData.enabled ? Theme.raised : Theme.sunken
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
                    color: clipSlot.modelData.enabled ? Theme.accentStroke : Theme.textDisabled
                }
                Text {
                    Layout.fillWidth: true
                    text: clipSlot.modelData.name
                    elide: Text.ElideRight
                    color: clipSlot.modelData.enabled ? Theme.textPrimary : Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                    font.strikeout: !clipSlot.modelData.enabled
                }
                // "2/3": on how many of the track's clips.
                Text {
                    Layout.rightMargin: Theme.space(1)
                    text: qsTr("%1/%2").arg(clipSlot.modelData.count).arg(clipSlot.modelData.total)
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
            }
            FocusRing {
                targetRadius: Theme.radiusBadge
                visible: clipSlot.visualFocus
            }
            HoverHandler {
                enabled: clipSlot.enabled
                cursorShape: Qt.PointingHandCursor
            }
            Loader {
                active: clipSlot.hovered
                sourceComponent: Tooltip {
                    parent: clipSlot
                    visible: true
                    text: qsTr("%1 · %2 · %3").arg(clipSlot.modelData.name).arg(clipSlot.modelData.summary).arg(clipSlot.modelData.details)
                }
            }
            OptionsMenu {
                id: clipMenu
                objectName: "clipEffectSlotMenu"
                options: [
                    { id: "settings", name: qsTr("Settings…") },
                    { id: "toggle", name: clipSlot.modelData.enabled ? qsTr("Bypass") : qsTr("Turn on") },
                    { id: "remove", name: qsTr("Remove from the clips") }
                ]
                onChosen: id => {
                    clipSlot.pending = id
                    if (!clipMenu.visible)
                        clipMenu.closed()
                }
                onClosed: {
                    const id = clipSlot.pending
                    clipSlot.pending = ""
                    if (id !== "")
                        Qt.callLater(clipSlot.act, id) // out of the closing menu: the change remakes the strips
                }
            }
        }
    }

    Repeater {
        id: slots
        model: root.effects
        delegate: AbstractButton {
            id: slot
            required property var modelData
            required property int index
            readonly property var pluginInfo: String(modelData.effect ?? "").startsWith("plugin.") && Plugins.revision >= 0
                                              ? Plugins.effectInfo(modelData.key) : ({})
            readonly property bool isPlugin: pluginInfo.plugin === true
            readonly property string slotName: isPlugin ? (pluginInfo.name ?? modelData.name) : modelData.name
            readonly property bool dragging: reorder.active
            /// The menu's choice, run once the menu has closed.
            property string pending: ""
            objectName: "effectSlot_" + index
            Layout.fillWidth: true
            implicitHeight: Theme.controlHeightSmall - Theme.space(1)
            z: dragging ? 2 : 0
            hoverEnabled: true
            focusPolicy: Qt.StrongFocus
            Accessible.name: modelData.enabled ? qsTr("%1 effect").arg(slotName) : qsTr("%1 effect (bypassed)").arg(slotName)
            Accessible.description: qsTr("Double-click for its settings")
            // A click opens the menu once it is not a double-click (that opens the settings).
            onClicked: menuDelay.restart()
            onDoubleClicked: {
                menuDelay.stop()
                root.openSettings(slot.modelData.key)
            }

            function act(id: string) {
                if (id === "settings")
                    root.openSettings(slot.modelData.key)
                else if (id === "editPlugin")
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

            Timer {
                id: menuDelay
                interval: Application.styleHints.mouseDoubleClickInterval
                onTriggered: slotMenu.popup(slot, 0, slot.height)
            }

            transform: Translate {
                y: slot.dragging ? reorder.activeTranslation.y : 0
            }
            // r07: drag a slot up or down to move it in the stack (one undo step).
            DragHandler {
                id: reorder
                target: null
                xAxis.enabled: false
                cursorShape: Qt.ClosedHandCursor
                onActiveChanged: {
                    if (active) {
                        menuDelay.stop()
                        root.dragFrom = slot.index
                        root.dropIndex = slot.index
                        return
                    }
                    const to = root.dropIndex
                    root.dragFrom = -1
                    root.dropIndex = -1
                    if (to >= 0 && to !== slot.index)
                        root.run(AudioMixer.moveEffect(root.target, slot.modelData.key, to))
                }
                onActiveTranslationChanged: {
                    if (active)
                        root.dropIndex = root.slotIndexAt(slot.y + slot.height / 2 + activeTranslation.y)
                }
            }

            background: Rectangle {
                radius: Theme.radiusBadge
                color: slot.hovered || slot.dragging ? Theme.hover : slot.modelData.enabled ? Theme.raised : Theme.sunken
                border.width: slot.dragging ? Theme.selectionWidth : Theme.hairline
                border.color: slot.dragging ? Theme.accentStroke : Theme.border
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
            // Where a dragged slot would land: a line above (moving up) or below (moving down) the slot it is over.
            Rectangle {
                visible: root.dragFrom >= 0 && root.dropIndex === slot.index && root.dragFrom !== slot.index
                anchors.left: parent.left
                anchors.right: parent.right
                y: root.dropIndex < root.dragFrom ? -height - Theme.space(0.25) : parent.height + Theme.space(0.25)
                height: Theme.selectionWidth
                radius: height / 2
                color: Theme.accentStroke
            }
            FocusRing {
                targetRadius: Theme.radiusBadge
                visible: slot.visualFocus
            }
            HoverHandler {
                enabled: slot.enabled
                cursorShape: Qt.PointingHandCursor
            }
            // The main values while the pointer rests on the slot ("Threshold -20.0 dB · Ratio 4.00 · …").
            Loader {
                active: slot.hovered && !slot.dragging && ((slot.modelData.details ?? "") !== "" || (slot.modelData.summary ?? "") !== "")
                sourceComponent: Tooltip {
                    parent: slot
                    visible: true
                    text: qsTr("%1 · %2").arg(slot.slotName).arg((slot.modelData.details ?? "") !== "" ? slot.modelData.details : slot.modelData.summary)
                }
            }

            OptionsMenu {
                id: slotMenu
                objectName: "effectSlotMenu"
                options: [{ id: "settings", name: qsTr("Settings…") }]
                         .concat(slot.isPlugin && slot.pluginInfo.hasEditor === true ? [{ id: "editPlugin", name: qsTr("Edit plugin…") }] : [])
                         .concat([
                    { id: "toggle", name: slot.modelData.enabled ? qsTr("Bypass") : qsTr("Turn on") },
                    { id: "up", name: qsTr("Move up") },
                    { id: "down", name: qsTr("Move down") },
                    { id: "remove", name: qsTr("Remove") }
                ])
                onChosen: id => {
                    slot.pending = id
                    if (!slotMenu.visible)
                        slotMenu.closed()
                }
                onClosed: {
                    const id = slot.pending
                    slot.pending = ""
                    if (id !== "")
                        Qt.callLater(slot.act, id) // out of the closing menu: the change remakes the strips
                }
            }
        }
    }

    // Add an effect
    AbstractButton {
        id: addSlot
        objectName: "effectSlotAdd"
        /// The Add menu's choice, run once the menu has closed.
        property string pending: ""
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
            onChosen: id => {
                addSlot.pending = id
                if (!addMenu.visible)
                    addMenu.closed()
            }
            onClosed: {
                const id = addSlot.pending
                addSlot.pending = ""
                if (id !== "")
                    Qt.callLater(root.addChosen, id) // out of the closing menu: the change remakes the strips
            }
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

    // 0.7 (card 101): presets of the chain. "Save effects as preset…" keeps the chain (the mixer panel's Save Preset
    // dialog); each audio preset in the library adds its effects after the chain's own (one undo step). Presets show in
    // the Effects panel's Presets bin too.
    AbstractButton {
        id: presetsSlot
        objectName: "effectPresetsSlot"
        /// The menu's choice, run once the menu has closed.
        property string pending: ""
        Layout.fillWidth: true
        implicitHeight: Theme.controlHeightSmall - Theme.space(1)
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.name: qsTr("Effect presets")
        function menuOptions(): var {
            const rows = root.effects.length > 0 ? [{ id: "save", name: qsTr("Save effects as preset…") }] : []
            const presets = EffectPresets.audioPresets()
            for (let i = 0; i < presets.length; ++i)
                rows.push({ id: "apply:" + presets[i].name, name: qsTr("Add “%1”").arg(presets[i].name) })
            if (rows.length === 0)
                rows.push({ id: "none", name: qsTr("No presets yet: add effects, then save them") })
            return rows
        }
        onClicked: {
            presetsMenu.options = presetsSlot.menuOptions()
            presetsMenu.popup(presetsSlot, 0, presetsSlot.height)
        }

        background: Rectangle {
            radius: Theme.radiusBadge
            color: presetsSlot.hovered ? Theme.hover : "transparent"
            border.width: Theme.hairline
            border.color: Theme.border
        }
        contentItem: RowLayout {
            spacing: Theme.space(1)
            Icon {
                Layout.leftMargin: Theme.space(1)
                name: "bookmark"
                size: Theme.iconXs
                color: Theme.textTertiary
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Presets")
                elide: Text.ElideRight
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
        }
        FocusRing {
            targetRadius: Theme.radiusBadge
            visible: presetsSlot.visualFocus
        }
        HoverHandler {
            enabled: presetsSlot.enabled
            cursorShape: Qt.PointingHandCursor
        }
        OptionsMenu {
            id: presetsMenu
            objectName: "effectPresetsMenu"
            onChosen: id => {
                presetsSlot.pending = id
                if (!presetsMenu.visible)
                    presetsMenu.closed()
            }
            onClosed: {
                const id = presetsSlot.pending
                presetsSlot.pending = ""
                if (id !== "")
                    Qt.callLater(root.presetChosen, id) // out of the closing menu: a change remakes the strips
            }
        }
    }

    // r07: what went wrong (it used to be kept here unseen), and the action to take when there is one.
    Rectangle {
        objectName: "effectSlotsMessage"
        Layout.fillWidth: true
        visible: root.message !== ""
        implicitHeight: messageColumn.implicitHeight + Theme.space(2)
        radius: Theme.radiusBadge
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: root.messageAction !== "" ? Theme.warning : Theme.danger

        ColumnLayout {
            id: messageColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.space(1)
            spacing: Theme.space(1)

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(0.5)
                Text {
                    objectName: "effectSlotsMessageText"
                    Layout.fillWidth: true
                    text: root.message
                    wrapMode: Text.Wrap
                    color: root.messageAction !== "" ? Theme.textSecondary : Theme.danger
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                    Accessible.role: Accessible.AlertMessage
                    Accessible.name: text
                }
                IconButton {
                    objectName: "effectSlotsMessageClose"
                    Layout.alignment: Qt.AlignTop
                    compact: true
                    iconName: "x"
                    iconSize: Theme.iconXs
                    tooltip: qsTr("Dismiss")
                    onClicked: root.run("")
                }
            }
            PillButton {
                objectName: "effectSlotsMessageAction"
                Layout.fillWidth: true
                visible: root.messageAction === "enhance"
                compact: true
                variant: "secondary"
                text: qsTr("Enhance Speech…")
                onClicked: AudioMixer.addEffect(root.target, "wabcut.audio.enhanceSpeech")
            }
        }
    }
}
