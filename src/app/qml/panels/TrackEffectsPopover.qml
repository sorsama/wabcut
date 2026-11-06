pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic as QC // qualified: unqualified, its ContextMenu would hide the app's

// Video Track Effects (0.7, card #139; docs/effects.md "Effects on a whole video track"): the effect stack of one video
// track, opened from its header's menu (Video Track Effects...). Effects here apply to the composite of EVERY clip on the
// track, after the clips' own effects - a colour grade or a blur for the whole track at once, without an adjustment layer.
//   * the stack, bottom to top: an On switch (bypass), the effect's name and its first settings, Move up / down, Remove;
//   * Add Effect...: a filter field and the video effects by category; the new effect is selected;
//   * the selected effect's settings (TrackEffects.effectSettings): a slider with a number field and its unit, a switch or
//     a menu, a stopwatch for settings that can be animated (the key goes at the playhead: a track's keys are in timeline
//     time, so the playhead can stay where it is) and Reset to defaults.
// Changes are live and each one is one undo step (a slider drag or a scrub merges until it ends). It stays open while the
// timeline is used (it is not modal); Escape or Done closes it. The values follow undo, redo and the playhead.
Popover {
    id: root
    objectName: "trackEffectsPopover"

    /// The track ("t12") and how the timeline calls it ("V2").
    property string trackKey: ""
    property string trackName: ""
    /// The effect whose settings show ("e5"), the stack, its settings.
    property string selected: ""
    property var effects: []
    property var settings: ({})
    property string message: ""
    /// Add Effect... shows the picker.
    property bool picking: false
    readonly property var params: root.settings.params ?? []
    /// The rows only change with another effect: the delegates stay while a slider is dragged.
    property int paramCount: 0
    readonly property bool animated: root.params.some(p => p.animated === true)

    function openFor(track: string, name: string) {
        root.trackKey = track
        root.trackName = name
        root.selected = ""
        root.message = ""
        root.picking = false
        root.refresh()
        root.open()
    }
    function refresh() {
        const list = TrackEffects.effectsOf(root.trackKey)
        root.effects = list
        if (!list.some(e => e.key === root.selected))
            root.selected = list.length > 0 ? list[list.length - 1].key : ""
        const next = root.selected !== "" ? TrackEffects.effectSettings(root.trackKey, root.selected) : ({})
        root.settings = next
        root.paramCount = (next.params ?? []).length
    }
    function setParam(key: string, value: real, gesture: bool) {
        root.message = TrackEffects.setEffectParam(root.trackKey, root.selected, key, value, gesture)
    }
    function add(effectId: string) {
        const before = root.effects.map(e => e.key)
        root.message = TrackEffects.addEffect(root.trackKey, effectId)
        root.picking = false
        if (root.message !== "")
            return
        const list = TrackEffects.effectsOf(root.trackKey)
        const added = list.find(e => before.indexOf(e.key) < 0)
        if (added)
            root.selected = added.key
        root.refresh()
    }

    title: qsTr("Effects on %1").arg(root.trackName)
    parent: QC.Overlay.overlay
    anchors.centerIn: parent
    closePolicy: QC.Popup.CloseOnEscape
    onClosed: TrackEffects.endEffectGesture()

    Connections {
        target: ProjectController
        enabled: root.visible
        function onRevisionChanged() {
            root.refresh()
        }
        function onPlayheadChanged() {
            if (root.animated)
                root.refresh()
        }
    }

    // Number boxes fit the widest value of their range and the unit.
    FontMetrics {
        id: digits
        font.family: Theme.fontMono
        font.pixelSize: Theme.textMd
    }
    FontMetrics {
        id: units
        font.family: Theme.fontMono
        font.pixelSize: Theme.textSm
    }

    ColumnLayout {
        id: body
        width: Math.min(Theme.space(116), (root.parent ? root.parent.width : Theme.space(116)) - Theme.space(16))
        spacing: Theme.space(2)

        Text {
            objectName: "trackEffectsHint"
            Layout.fillWidth: true
            text: qsTr("These effects apply to every clip on %1 together, after the clips' own effects.").arg(root.trackName)
            wrapMode: Text.Wrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        // The stack.
        Flickable {
            id: stackList
            objectName: "trackEffectsList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(stackRows.implicitHeight, Theme.space(36))
            visible: root.effects.length > 0
            contentHeight: stackRows.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height
            QC.ScrollBar.vertical: ThinScrollBar {}

            ColumnLayout {
                id: stackRows
                width: stackList.width - (stackList.interactive ? Theme.space(2) : 0)
                spacing: Theme.space(0.5)

                Repeater {
                    model: root.effects
                    delegate: Rectangle {
                        id: item
                        required property var modelData
                        required property int index
                        objectName: "trackEffect_" + item.modelData.key
                        readonly property bool current: item.modelData.key === root.selected
                        Layout.fillWidth: true
                        implicitHeight: Theme.controlHeight + Theme.space(1)
                        radius: Theme.radiusField
                        color: item.current ? Theme.raised : itemHover.hovered ? Theme.hover : "transparent"
                        border.width: item.current ? Theme.hairline : 0
                        border.color: Theme.accentStroke
                        Accessible.role: Accessible.ListItem
                        Accessible.name: item.modelData.name
                        Accessible.selected: item.current
                        Accessible.onPressAction: root.selected = item.modelData.key

                        HoverHandler {
                            id: itemHover
                        }
                        TapHandler {
                            onTapped: {
                                root.selected = item.modelData.key
                                root.refresh()
                            }
                        }
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.space(2)
                            anchors.rightMargin: Theme.space(1)
                            spacing: Theme.space(1.5)

                            Toggle {
                                objectName: "trackEffectOn"
                                Layout.alignment: Qt.AlignVCenter
                                checked: item.modelData.enabled
                                Accessible.name: qsTr("%1 on").arg(item.modelData.name)
                                onToggled: root.message = TrackEffects.setEffectEnabled(root.trackKey, item.modelData.key, checked)
                            }
                            Text {
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignVCenter
                                text: item.modelData.summary !== "" ? qsTr("%1  ·  %2").arg(item.modelData.name).arg(item.modelData.summary)
                                                                    : item.modelData.name
                                elide: Text.ElideRight
                                color: item.modelData.enabled ? Theme.textPrimary : Theme.textTertiary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                            }
                            IconButton {
                                objectName: "trackEffectUp"
                                compact: true
                                iconName: "chevron-up"
                                iconSize: Theme.iconSm
                                tooltip: qsTr("Move %1 down in the stack (it applies earlier)").arg(item.modelData.name)
                                enabled: item.index > 0
                                onClicked: root.message = TrackEffects.moveEffect(root.trackKey, item.modelData.key, item.index - 1)
                            }
                            IconButton {
                                objectName: "trackEffectDown"
                                compact: true
                                iconName: "chevron-down"
                                iconSize: Theme.iconSm
                                tooltip: qsTr("Move %1 up in the stack (it applies later)").arg(item.modelData.name)
                                enabled: item.index < root.effects.length - 1
                                onClicked: root.message = TrackEffects.moveEffect(root.trackKey, item.modelData.key, item.index + 1)
                            }
                            IconButton {
                                objectName: "trackEffectRemove"
                                compact: true
                                iconName: "trash-2"
                                iconSize: Theme.iconSm
                                tooltip: qsTr("Remove %1 from the track").arg(item.modelData.name)
                                onClicked: root.message = TrackEffects.removeEffect(root.trackKey, item.modelData.key)
                            }
                        }
                    }
                }
            }
        }
        Text {
            objectName: "trackEffectsEmpty"
            Layout.fillWidth: true
            visible: root.effects.length === 0
            text: qsTr("No effects on this track yet. Add one to grade, blur or otherwise change everything on %1 at once.").arg(root.trackName)
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }

        // Add Effect...: filter, then the effects by category.
        PillButton {
            objectName: "trackEffectsAdd"
            variant: root.picking ? "ghost" : "secondary"
            compact: true
            iconName: root.picking ? "x" : "plus"
            text: root.picking ? qsTr("Cancel") : qsTr("Add Effect…")
            onClicked: {
                root.picking = !root.picking
                if (root.picking) {
                    pickFilter.text = ""
                    pickFilter.forceActiveFocus(Qt.PopupFocusReason)
                }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            visible: root.picking
            spacing: Theme.space(1)

            FieldInput {
                id: pickFilter
                objectName: "trackEffectsFilter"
                Layout.fillWidth: true
                iconName: "search"
                clearOnEscape: true
                placeholderText: qsTr("Find an effect")
                Accessible.name: qsTr("Find an effect")
                Keys.onDownPressed: pickList.currentIndex = Math.min(pickList.count - 1, pickList.currentIndex + 1)
                Keys.onUpPressed: pickList.currentIndex = Math.max(0, pickList.currentIndex - 1)
                onAccepted: {
                    const hit = pickList.model[Math.max(0, pickList.currentIndex)]
                    if (hit)
                        root.add(hit.id)
                }
            }
            ListView {
                id: pickList
                objectName: "trackEffectsPicker"
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.space(46)
                clip: true
                model: root.picking && TrackEffects ? TrackEffects.available(pickFilter.text) : []
                currentIndex: 0
                boundsBehavior: Flickable.StopAtBounds
                highlightMoveDuration: 0
                QC.ScrollBar.vertical: ThinScrollBar {}
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Video effects")
                section.property: "category"
                section.delegate: Text {
                    required property string section
                    width: ListView.view ? ListView.view.width : 0
                    height: Theme.controlHeight
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: Theme.space(2)
                    text: section
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    font.weight: Font.DemiBold
                }

                delegate: Rectangle {
                    id: pick
                    required property var modelData
                    required property int index
                    objectName: "trackEffectPick_" + pick.modelData.id
                    width: ListView.view ? ListView.view.width : 0
                    height: Theme.controlHeight
                    radius: Theme.radiusField
                    color: pickHover.hovered || pick.ListView.isCurrentItem ? Theme.hover : "transparent"
                    Accessible.role: Accessible.ListItem
                    Accessible.name: pick.modelData.name
                    Accessible.onPressAction: root.add(pick.modelData.id)

                    Text {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: Theme.space(3)
                        anchors.rightMargin: Theme.space(2)
                        anchors.verticalCenter: parent.verticalCenter
                        text: pick.modelData.name
                        elide: Text.ElideRight
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                    }
                    HoverHandler {
                        id: pickHover
                    }
                    TapHandler {
                        onTapped: root.add(pick.modelData.id)
                    }
                }
            }
        }

        // The selected effect's settings.
        Text {
            Layout.fillWidth: true
            visible: root.selected !== "" && root.settings.known === false
            text: qsTr("Wabcut cannot run this effect on this computer (a newer version's effect), so it is bypassed. Its settings are kept.")
            wrapMode: Text.Wrap
            color: Theme.warning
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        SectionHeader {
            Layout.fillWidth: true
            visible: root.selected !== "" && !root.picking
            text: qsTr("%1 settings").arg(root.settings.name ?? "")
        }
        Flickable {
            id: paramList
            objectName: "trackEffectParams"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(rows.implicitHeight, Theme.space(52))
            visible: root.paramCount > 0 && !root.picking
            contentHeight: rows.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height
            QC.ScrollBar.vertical: ThinScrollBar {}

            ColumnLayout {
                id: rows
                width: paramList.width - (paramList.interactive ? Theme.space(2) : 0)
                spacing: Theme.space(1)

                Repeater {
                    model: root.paramCount
                    delegate: Item {
                        id: row
                        required property int index
                        readonly property var param: root.params[row.index] ?? ({})
                        readonly property string kind: row.param.kind ?? "slider"
                        readonly property bool modified: Math.abs((row.param.value ?? 0) - (row.param.defaultValue ?? 0))
                                                         > Math.pow(10, -(row.param.decimals ?? 0)) / 2
                        objectName: "trackEffectParam_" + (row.param.key ?? "")
                        Layout.fillWidth: true
                        implicitHeight: Theme.controlHeight

                        RowLayout {
                            anchors.fill: parent
                            spacing: Theme.space(1.5)

                            // Stopwatch: animate this setting (a key at the playhead).
                            IconButton {
                                objectName: "trackEffectStopwatch"
                                Layout.alignment: Qt.AlignVCenter
                                compact: true
                                checkable: true
                                checked: row.param.animated === true
                                visible: row.param.animatable === true
                                iconName: "timer"
                                iconSize: Theme.iconSm
                                tooltip: row.param.animated === true ? qsTr("Stop animating %1 (deletes its keyframes)").arg(row.param.name ?? "")
                                                                     : qsTr("Animate %1: keys go at the playhead").arg(row.param.name ?? "")
                                onClicked: root.message = TrackEffects.setAnimated(root.trackKey, root.selected, row.param.key,
                                                                                    row.param.animated !== true)
                            }
                            Item {
                                visible: row.param.animatable !== true
                                Layout.preferredWidth: Theme.space(7)
                                Layout.preferredHeight: 1
                            }

                            Text {
                                id: paramLabel
                                Layout.preferredWidth: Theme.propLabelWidth + Theme.space(4)
                                Layout.alignment: Qt.AlignVCenter
                                text: row.param.name ?? ""
                                elide: Text.ElideRight
                                color: row.modified ? Theme.textPrimary : Theme.textSecondary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                                HoverHandler {
                                    id: labelHover
                                }
                                Tooltip {
                                    visible: labelHover.hovered && paramLabel.truncated
                                    text: row.param.name ?? ""
                                }
                            }

                            // A slider and its number box with the unit.
                            ValueSlider {
                                objectName: "trackEffectSlider"
                                Layout.fillWidth: true
                                Layout.minimumWidth: Theme.space(10)
                                Layout.alignment: Qt.AlignVCenter
                                visible: row.kind === "slider"
                                from: row.param.from ?? 0
                                to: row.param.to ?? 1
                                origin: row.param.origin ?? 0
                                value: row.param.value ?? 0
                                stepSize: row.param.step ?? 1
                                Accessible.name: row.param.name ?? ""
                                onMoved: root.setParam(row.param.key, value, true)
                                onPressedChanged: {
                                    if (!pressed)
                                        TrackEffects.endEffectGesture()
                                }
                            }
                            NumberField {
                                objectName: "trackEffectNumber"
                                Layout.alignment: Qt.AlignVCenter
                                Layout.preferredWidth: {
                                    const d = row.param.decimals ?? 0
                                    const widest = Math.max(digits.advanceWidth(Number(row.param.from ?? 0).toFixed(d)),
                                                            digits.advanceWidth(Number(row.param.to ?? 0).toFixed(d)))
                                    const unit = (row.param.unit ?? "") !== "" ? units.advanceWidth(row.param.unit) + Theme.space(1) : 0
                                    return Math.max(Theme.numberFieldWidth, Math.ceil(widest + unit) + Theme.space(2.5) * 2 + Theme.space(1))
                                }
                                visible: row.kind === "slider"
                                from: row.param.from ?? 0
                                to: row.param.to ?? 1
                                value: row.param.value ?? 0
                                stepSize: row.param.step ?? 1
                                dragStep: ((row.param.to ?? 1) - (row.param.from ?? 0)) / 400
                                decimals: row.param.decimals ?? 0
                                suffix: row.param.unit ?? ""
                                Accessible.name: row.param.name ?? ""
                                onValueModified: v => root.setParam(row.param.key, v, true)
                                onEditingFinished: TrackEffects.endEffectGesture()
                                onScrubFinished: TrackEffects.endEffectGesture()
                            }

                            // A switch.
                            Item {
                                Layout.fillWidth: true
                                visible: row.kind === "toggle"
                                implicitHeight: switchControl.implicitHeight
                                Toggle {
                                    id: switchControl
                                    objectName: "trackEffectSwitch"
                                    anchors.verticalCenter: parent.verticalCenter
                                    checked: (row.param.value ?? 0) >= 0.5
                                    Accessible.name: row.param.name ?? ""
                                    onToggled: root.setParam(row.param.key, checked ? 1 : 0, false)
                                }
                            }

                            // A menu.
                            Item {
                                Layout.fillWidth: true
                                visible: row.kind === "choice"
                                implicitHeight: choice.implicitHeight
                                EditingChoice {
                                    id: choice
                                    objectName: "trackEffectChoice"
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: Math.min(implicitWidth, parent.width)
                                    options: row.param.options ?? []
                                    current: row.param.current ?? ""
                                    Accessible.name: row.param.name ?? ""
                                    onChosen: id => root.setParam(row.param.key, Number(id), false)
                                }
                            }

                            IconButton {
                                objectName: "trackEffectParamReset"
                                Layout.alignment: Qt.AlignVCenter
                                compact: true
                                iconName: "rotate-ccw"
                                iconSize: Theme.iconSm
                                tooltip: qsTr("Reset %1").arg(row.param.name ?? "")
                                enabled: row.modified
                                opacity: row.modified ? 1 : 0
                                onClicked: {
                                    TrackEffects.endEffectGesture()
                                    root.setParam(row.param.key, row.param.defaultValue ?? 0, false)
                                }
                            }
                        }
                    }
                }
            }
        }
        Text {
            Layout.fillWidth: true
            visible: root.selected !== "" && root.paramCount === 0 && root.settings.known !== false && !root.picking
            text: qsTr("This effect has no settings.")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Text {
            Layout.fillWidth: true
            visible: root.animated && !root.picking
            text: qsTr("Animated settings change at the playhead (their key there).")
            wrapMode: Text.Wrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Text {
            objectName: "trackEffectsMessage"
            Layout.fillWidth: true
            visible: text !== ""
            text: root.message
            wrapMode: Text.Wrap
            color: Theme.danger
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        Flow {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            PillButton {
                objectName: "trackEffectsReset"
                variant: "ghost"
                compact: true
                iconName: "rotate-ccw"
                text: qsTr("Reset to defaults")
                visible: root.selected !== ""
                enabled: root.paramCount > 0
                onClicked: root.message = TrackEffects.resetEffect(root.trackKey, root.selected)
            }
            PillButton {
                objectName: "trackEffectsDone"
                variant: "primary"
                compact: true
                text: qsTr("Done")
                onClicked: root.close()
            }
        }
    }
}
