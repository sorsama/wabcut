pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// r07: the settings of one effect of an Audio Track Mixer strip (a track, a submix or the master), from its slot's
// "Settings…" or a double-click: every parameter with its control - a slider with a number field and its unit, a switch
// or a menu (AudioMixer.effectSettings) - an On switch (bypass), Reset to defaults and, for a plugin with an editor,
// "Edit plugin…". Changes are live (the player follows the project) and each one is one undo step: a slider drag or a
// scrub merges until it ends (AudioMixer.setEffectParam / endEffectGesture). An animated parameter sets its key at the
// playhead, as the fader does. The values follow undo, redo and the playhead.
Popover {
    id: root
    objectName: "mixerEffectSettings"

    /// "t12" (a track or submix) or "master", and the effect's key ("e5").
    property string target: ""
    property string effectKey: ""
    /// Where it sits, for the header ("A1", "Master").
    property string stripName: ""
    property var settings: ({})
    property string message: ""
    readonly property var params: root.settings.params ?? []
    /// The rows only change with another effect: the delegates stay while a slider is dragged.
    property int paramCount: 0
    readonly property bool animated: root.params.some(p => p.animated === true)
    /// 0.7 (card 044): Dynamics Processing shows its graph above the sliders.
    readonly property bool isDynamics: root.settings.effect === "wabcut.audio.dynamicsProcessing" && root.settings.known !== false
    readonly property var dynamicsState: {
        if (!root.isDynamics)
            return ({})
        const v = {}
        for (const p of root.params)
            v[p.key] = p.value
        const bands = []
        for (let b = 1; b <= 4; ++b) {
            const k = name => v["band" + b + name]
            bands.push({ mode: k("Mode"), threshold: k("Threshold"), ratio: k("Ratio"), range: k("Range"), attack: k("Attack"),
                         release: k("Release"), knee: k("Knee"), makeup: k("Makeup"), bypass: k("Bypass"), solo: k("Solo") })
        }
        return { bands: Math.round(v.bandCount ?? 4), crossovers: [v.crossover1, v.crossover2, v.crossover3], band: bands,
                 inputGain: v.inputGain, outputGain: v.outputGain, mix: v.mix, limiter: v.limiterEnable, gate: v.autoGate }
    }
    readonly property var pluginInfo: root.settings.plugin === true && Plugins.revision >= 0 ? Plugins.effectInfo(root.effectKey) : ({})

    function openFor(target: string, effectKey: string, stripName: string) {
        root.target = target
        root.effectKey = effectKey
        root.stripName = stripName
        root.message = ""
        root.refresh()
        root.open()
    }
    function refresh() {
        const next = root.effectKey !== "" ? AudioMixer.effectSettings(root.target, root.effectKey) : ({})
        root.settings = next
        root.paramCount = (next.params ?? []).length
        if (root.opened && next.key === undefined)
            root.close() // the effect went (removed, undone)
    }
    function setParam(key: string, value: real, gesture: bool) {
        root.message = AudioMixer.setEffectParam(root.target, root.effectKey, key, value, gesture)
    }

    title: root.settings.name ?? ""
    onClosed: AudioMixer.endEffectGesture()

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

    // Number boxes fit the widest value of their range and the unit (a long value was cut off in a fixed box).
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
        width: Theme.space(116)
        spacing: Theme.space(2)

        // Where the effect sits, and On / bypassed.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Text {
                objectName: "mixerEffectWhere"
                Layout.fillWidth: true
                text: root.target === "master" ? qsTr("Master effect") : qsTr("Effect on %1").arg(root.stripName)
                elide: Text.ElideRight
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Toggle {
                objectName: "mixerEffectOn"
                text: qsTr("On")
                checked: root.settings.enabled === true
                Accessible.name: qsTr("%1 on").arg(root.settings.name ?? "")
                onToggled: root.message = AudioMixer.setEffectEnabled(root.target, root.effectKey, checked)
            }
        }
        Text {
            Layout.fillWidth: true
            visible: text !== ""
            text: root.settings.known === false
                  ? qsTr("Wabcut cannot run this effect on this computer (a plugin that is not installed here, or a newer version), so it is bypassed. Its settings are kept.")
                  : (root.settings.description ?? "")
            wrapMode: Text.Wrap
            maximumLineCount: 3
            elide: Text.ElideRight
            color: root.settings.known === false ? Theme.warning : Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        // 0.7 (card 044): the Dynamics Processing graph; its edits are this popup's own parameter edits. The popover opens
        // upward from the slot and must stay inside the window, so the graph scrolls when there is no room for all of it.
        DynamicsGraph {
            objectName: "mixerDynamicsGraph"
            Layout.fillWidth: true
            // The width the layout gives it, from the start: the graph's own layout (side by side or stacked) depends on its
            // width, and a width that changes while the popover is laid out would make the layout give up after two passes.
            implicitWidth: body.width
            visible: root.isDynamics
            maxHeight: Math.max(Theme.space(60), Window.height - Theme.space(85))
            fx: root.dynamicsState
            effectEnabled: root.settings.enabled === true
            meterProvider: () => AudioMixer.dynamicsMeters(root.effectKey)
            curveProvider: (mode, threshold, ratio, range, knee, makeup, fromDb, toDb, points) =>
                           AudioMixer.dynamicsCurve(mode, threshold, ratio, range, knee, makeup, fromDb, toDb, points)
            onValueEdited: (key, value) => root.setParam(key, value, true)
            onEditFinished: AudioMixer.endEffectGesture()
        }

        // The parameters.
        Flickable {
            id: paramList
            objectName: "mixerEffectParams"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(rows.implicitHeight, root.isDynamics ? Theme.space(30) : Theme.space(90))
            visible: root.paramCount > 0
            contentHeight: rows.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height

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
                        objectName: "mixerEffectParam_" + (row.param.key ?? "")
                        Layout.fillWidth: true
                        implicitHeight: Theme.controlHeight

                        RowLayout {
                            anchors.fill: parent
                            spacing: Theme.space(1.5)

                            Text {
                                id: paramLabel
                                Layout.preferredWidth: Theme.propLabelWidth + Theme.space(8)
                                Layout.alignment: Qt.AlignVCenter
                                text: row.param.name ?? ""
                                elide: Text.ElideRight
                                color: row.modified ? Theme.textPrimary : Theme.textSecondary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                                HoverHandler { id: labelHover }
                                Tooltip {
                                    visible: labelHover.hovered && paramLabel.truncated
                                    text: row.param.name ?? ""
                                }
                            }

                            // A slider and its number box with the unit.
                            ValueSlider {
                                objectName: "mixerEffectSlider"
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
                                        AudioMixer.endEffectGesture()
                                }
                            }
                            NumberField {
                                objectName: "mixerEffectNumber"
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
                                onEditingFinished: AudioMixer.endEffectGesture()
                                onScrubFinished: AudioMixer.endEffectGesture()
                            }

                            // A switch.
                            Item {
                                Layout.fillWidth: true
                                visible: row.kind === "toggle"
                                implicitHeight: switchControl.implicitHeight
                                Toggle {
                                    id: switchControl
                                    objectName: "mixerEffectSwitch"
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
                                    objectName: "mixerEffectChoice"
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: Math.min(implicitWidth, parent.width)
                                    options: row.param.options ?? []
                                    current: row.param.current ?? ""
                                    Accessible.name: row.param.name ?? ""
                                    onChosen: id => root.setParam(row.param.key, Number(id), false)
                                }
                            }

                            IconButton {
                                objectName: "mixerEffectParamReset"
                                Layout.alignment: Qt.AlignVCenter
                                compact: true
                                iconName: "rotate-ccw"
                                iconSize: Theme.iconSm
                                tooltip: qsTr("Reset %1").arg(row.param.name ?? "")
                                enabled: row.modified
                                opacity: row.modified ? 1 : 0
                                onClicked: {
                                    AudioMixer.endEffectGesture()
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
            visible: root.paramCount === 0 && root.settings.known !== false
            text: qsTr("This effect has no settings.")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Text {
            Layout.fillWidth: true
            visible: root.animated
            text: qsTr("Animated settings change at the playhead (their key there).")
            wrapMode: Text.Wrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Text {
            objectName: "mixerEffectMessage"
            Layout.fillWidth: true
            visible: text !== ""
            text: root.message
            wrapMode: Text.Wrap
            color: Theme.danger
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        // Reset, Edit plugin (wraps in a narrow window).
        Flow {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            PillButton {
                objectName: "mixerEffectReset"
                variant: "ghost"
                compact: true
                iconName: "rotate-ccw"
                text: qsTr("Reset to defaults")
                enabled: root.paramCount > 0
                onClicked: root.message = AudioMixer.resetEffect(root.target, root.effectKey)
            }
            PillButton {
                objectName: "mixerEffectEditPlugin"
                visible: root.pluginInfo.hasEditor === true
                variant: "secondary"
                compact: true
                text: qsTr("Edit plugin…")
                onClicked: root.message = Plugins.openEditor(root.effectKey)
            }
        }
    }
}
