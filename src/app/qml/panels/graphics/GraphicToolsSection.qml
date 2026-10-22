pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// Essential Graphics' 0.2 tools (GraphicTools, docs/graphics.md) for the graphic clip in Callouts > Edit: Align (the
// selected layers - Ctrl-click adds layers - to the video frame or to the selection) and Distribute (the selected
// layers, three or more, else every top-level layer), Text Styles of a selected text layer
// (Create Text Style…, the styles to apply, Push to Text Style), and Roll / Crawl with Premiere's options. Each change
// is one command and one undo step.
ColumnLayout {
    id: root
    objectName: "graphicToolsSection"

    readonly property string clipKey: Graphics.clip
    readonly property int layerId: Graphics.selectedLayer
    readonly property var sel: Graphics.layer
    readonly property bool isText: (sel.kind ?? "") === "text"
    /// The layers Align and Distribute move: every selected one (Ctrl-click in the layer list adds layers).
    readonly property var chosen: Graphics.selectedLayers
    /// Premiere's "Align to Video Frame" / "Align to Selection" (two or more layers).
    property bool toSelection: false
    readonly property var rollOptions: GraphicTools.message.length >= 0 && Graphics.graphic !== undefined ? GraphicTools.roll(clipKey) : ({})
    readonly property string followed: GraphicTools.textStyles.length >= 0 && Graphics.layer !== undefined
        ? GraphicTools.layerTextStyle(clipKey, layerId) : ""

    spacing: Theme.space(1)
    visible: clipKey !== ""

    function setRoll(changes) {
        const r = Object.assign({}, root.rollOptions, changes)
        GraphicTools.setRoll(root.clipKey, r)
    }

    // ---- Align and Distribute ----
    GraphicGroupLabel {
        text: qsTr("Align and distribute")
    }
    PillTabs {
        objectName: "graphicAlignTo"
        Layout.fillWidth: true
        compact: true
        model: [qsTr("To video frame"), qsTr("To selection")]
        currentIndex: root.toSelection && root.chosen.length >= 2 ? 1 : 0
        enabled: root.chosen.length >= 2
        Accessible.name: qsTr("Align to")
        onActivated: index => root.toSelection = index === 1
    }
    Flow {
        Layout.fillWidth: true
        spacing: Theme.space(1)
        Repeater {
            model: [{ how: "left", label: qsTr("Left") }, { how: "horizontalCenter", label: qsTr("Center") },
                { how: "right", label: qsTr("Right") }, { how: "top", label: qsTr("Top") },
                { how: "verticalCenter", label: qsTr("Middle") }, { how: "bottom", label: qsTr("Bottom") }]
            delegate: PillButton {
                required property var modelData
                objectName: "graphicAlign_" + modelData.how
                compact: true
                enabled: root.layerId > 0
                text: modelData.label
                tooltip: root.toSelection && root.chosen.length >= 2
                         ? qsTr("Align the selected layers to the selection: %1").arg(modelData.label)
                         : qsTr("Align the selected layers to the video frame: %1").arg(modelData.label)
                onClicked: GraphicTools.align(root.clipKey, root.chosen, modelData.how,
                                              root.toSelection && root.chosen.length >= 2)
            }
        }
        PillButton {
            objectName: "graphicDistributeH"
            compact: true
            text: qsTr("Distribute ↔")
            tooltip: root.chosen.length >= 3 ? qsTr("Space the selected layers evenly from left to right")
                                             : qsTr("Space the layers evenly from left to right")
            enabled: root.chosen.length >= 3 || GraphicTools.topLayers(root.clipKey).length >= 3
            onClicked: GraphicTools.align(root.clipKey, root.chosen.length >= 3 ? root.chosen : GraphicTools.topLayers(root.clipKey),
                                          "distributeHorizontal", true)
        }
        PillButton {
            objectName: "graphicDistributeV"
            compact: true
            text: qsTr("Distribute ↕")
            tooltip: root.chosen.length >= 3 ? qsTr("Space the selected layers evenly from top to bottom")
                                             : qsTr("Space the layers evenly from top to bottom")
            enabled: root.chosen.length >= 3 || GraphicTools.topLayers(root.clipKey).length >= 3
            onClicked: GraphicTools.align(root.clipKey, root.chosen.length >= 3 ? root.chosen : GraphicTools.topLayers(root.clipKey),
                                          "distributeVertical", true)
        }
    }

    // ---- Text Styles ----
    GraphicGroupLabel {
        visible: root.isText
        text: qsTr("Text style")
    }
    RowLayout {
        Layout.fillWidth: true
        visible: root.isText
        spacing: Theme.space(1)
        GraphicChoicePill {
            objectName: "graphicTextStyle"
            Layout.fillWidth: true
            options: [{ id: "", name: qsTr("None") }].concat(GraphicTools.textStyles.map(s => ({ id: s, name: s })))
            current: root.followed
            placeholder: qsTr("No text style")
            Accessible.name: qsTr("Text style")
            onChosen: id => {
                if (id !== "")
                    GraphicTools.applyTextStyle(root.clipKey, root.layerId, id)
            }
        }
        PillButton {
            objectName: "graphicCreateTextStyle"
            compact: true
            text: qsTr("Create…")
            tooltip: qsTr("Create Text Style from this layer")
            onClicked: names.ask(qsTr("Create Text Style"), qsTr("Text Style"), qsTr("Create"),
                                 text => GraphicTools.createTextStyle(root.clipKey, root.layerId, text))
        }
        PillButton {
            objectName: "graphicPushTextStyle"
            compact: true
            visible: root.followed !== ""
            text: qsTr("Push")
            tooltip: qsTr("Push to Text Style: every layer that follows %1 takes this layer's look").arg(root.followed)
            onClicked: GraphicTools.pushTextStyle(root.clipKey, root.layerId)
        }
    }

    // ---- Roll / Crawl ----
    GraphicGroupLabel {
        text: qsTr("Roll / crawl")
    }
    Toggle {
        objectName: "graphicRoll"
        text: qsTr("Roll or crawl over the clip")
        checked: root.rollOptions.enabled === true
        onToggled: root.setRoll({ enabled: checked })
    }
    ColumnLayout {
        Layout.fillWidth: true
        visible: root.rollOptions.enabled === true
        spacing: Theme.space(1)
        PillTabs {
            objectName: "graphicRollKind"
            Layout.fillWidth: true
            compact: true
            model: [qsTr("Roll"), qsTr("Crawl Left"), qsTr("Crawl Right")]
            currentIndex: Math.max(0, ["roll", "crawlLeft", "crawlRight"].indexOf(root.rollOptions.kind ?? "roll"))
            onActivated: index => root.setRoll({ kind: ["roll", "crawlLeft", "crawlRight"][index] })
        }
        Toggle {
            objectName: "graphicRollStartOff"
            text: qsTr("Start Off Screen")
            checked: root.rollOptions.startOffScreen !== false
            onToggled: root.setRoll({ startOffScreen: checked })
        }
        Toggle {
            objectName: "graphicRollEndOff"
            text: qsTr("End Off Screen")
            checked: root.rollOptions.endOffScreen !== false
            onToggled: root.setRoll({ endOffScreen: checked })
        }
        Repeater {
            model: [{ key: "preroll", name: qsTr("Preroll") }, { key: "postroll", name: qsTr("Postroll") },
                { key: "easeIn", name: qsTr("Ease-In") }, { key: "easeOut", name: qsTr("Ease-Out") }]
            delegate: RowLayout {
                id: rollRow
                required property var modelData
                Layout.fillWidth: true
                spacing: Theme.space(1)
                Text {
                    Layout.fillWidth: true
                    text: rollRow.modelData.name
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                NumberField {
                    objectName: "graphicRoll_" + rollRow.modelData.key
                    Layout.preferredWidth: Theme.numberFieldWidth
                    from: 0
                    to: 3600
                    stepSize: 0.1
                    decimals: 2
                    suffix: qsTr("s")
                    value: Number(root.rollOptions[rollRow.modelData.key] ?? 0)
                    Accessible.name: rollRow.modelData.name
                    onValueModified: v => {
                        const change = {}
                        change[rollRow.modelData.key] = v
                        root.setRoll(change)
                    }
                }
            }
        }
    }
    Text {
        Layout.fillWidth: true
        visible: GraphicTools.message !== ""
        wrapMode: Text.Wrap
        text: GraphicTools.message
        color: Theme.warning
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    EffectNameDialog {
        id: names
    }
}
