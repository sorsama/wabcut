pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// Callouts > Edit: the graphic clip selected on the timeline (Graphics.clip). A template instance's fields come first
// (template.setField: a typing or a drag is one undo step), then the basics of the selected layer - which text or shape
// (the Program Monitor's handles select through Graphics.selectedLayer too), its text, font, size and colour. Behind
// "Advanced" (closed until opened): the layer tree (top first, groups indented) with visibility, order and delete, the
// buttons that add a layer (New Layer: Text is Ctrl+T), every property of the selected layer (GraphicLayerProperties:
// motion, presets, pins; made when opened) and the graphic's Responsive Design - Time. Without a graphic selected,
// adding a layer makes one at the playhead.
Item {
    id: root
    objectName: "graphicsEditPage"

    readonly property bool hasGraphic: Graphics.clip !== ""
    /// The deep layer editing is shown (a gamer's callout needs the fields and the basics).
    property bool advanced: false
    readonly property var graphic: Graphics.graphic
    readonly property var responsive: graphic.responsive ?? null
    readonly property var fontFamilies: [
        { id: "Segoe UI", name: "Segoe UI" }, { id: "Arial", name: "Arial" }, { id: "Inter", name: "Inter" },
        { id: "Georgia", name: "Georgia" }, { id: "Noto Sans Thai", name: "Noto Sans Thai" }, { id: "Cascadia Mono", name: "Cascadia Mono" },
        { id: "Bai Jamjuree", name: "Bai Jamjuree" }, { id: "IBM Plex Sans", name: "IBM Plex Sans" }
    ].concat((Graphics.brand.fonts ?? []).map(f => ({ id: f.family, name: qsTr("Brand %1 (%2)").arg(f.role).arg(f.family) })))
    readonly property var kindIcons: ({ text: "type", shape: "square", image: "image", group: "folder" })

    /// Save as template… (the panel opens its dialog).
    signal saveRequested()

    /// A colour field's value ("#RRGGBB" or "brand:Name") as a GraphicColorRow value.
    function colourValue(v) {
        const s = String(v ?? Theme.contentWhite)
        if (s.startsWith("brand:")) {
            const name = s.slice(6)
            const found = (Graphics.brand.colors ?? []).find(c => c.name === name)
            return { color: found ? found.color : Theme.contentWhite, alpha: 1, brand: name }
        }
        return { color: s.slice(0, 7), alpha: 1, brand: "" }
    }

    // The buttons that add a layer: here, and on the empty page (where they make a graphic first).
    component AddLayerRow: RowLayout {
        id: addRow
        /// Keeps the two rows' object names apart (tests).
        property string prefix: ""
        spacing: Theme.space(1)
        IconButton {
            objectName: addRow.prefix + "graphicAddText"
            compact: true
            iconName: "type"
            command: "graphics.newTextLayer"
            tooltip: qsTr("New text layer")
            onClicked: Graphics.addLayer("text")
        }
        IconButton {
            objectName: addRow.prefix + "graphicAddRectangle"
            compact: true
            iconName: "square"
            command: "graphics.newRectangleLayer"
            tooltip: qsTr("New rectangle layer")
            onClicked: Graphics.addLayer("rectangle")
        }
        IconButton {
            objectName: addRow.prefix + "graphicAddEllipse"
            compact: true
            iconName: "circle"
            command: "graphics.newEllipseLayer"
            tooltip: qsTr("New ellipse layer")
            onClicked: Graphics.addLayer("ellipse")
        }
        IconButton {
            objectName: addRow.prefix + "graphicAddImage"
            compact: true
            iconName: "image"
            tooltip: qsTr("New picture layer")
            onClicked: Graphics.addLayer("image")
        }
        IconButton {
            objectName: addRow.prefix + "graphicAddGroup"
            compact: true
            iconName: "folder"
            tooltip: qsTr("New group")
            onClicked: Graphics.addLayer("group")
        }
    }

    // ---- No graphic selected ----
    ColumnLayout {
        objectName: "graphicsEditEmpty"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: Theme.space(4)
        visible: !root.hasGraphic
        spacing: Theme.space(2)

        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("Select a graphic on the timeline to edit it, or add a layer to start a new graphic at the playhead.")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        AddLayerRow {
            prefix: "empty_"
        }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        visible: root.hasGraphic
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        ColumnLayout {
            id: column
            width: flick.width - Theme.space(3)
            spacing: Theme.space(2)

            // ---- The graphic ----
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                Text {
                    objectName: "graphicName"
                    Layout.fillWidth: true
                    text: (root.graphic.name ?? "") !== "" ? root.graphic.name : qsTr("Graphic")
                    elide: Text.ElideRight
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Font.DemiBold
                }
                Text {
                    text: root.graphic.template
                          ? qsTr("%1 × %2 · from %3").arg(root.graphic.width ?? 0).arg(root.graphic.height ?? 0).arg(root.graphic.template.id)
                          : qsTr("%1 × %2").arg(root.graphic.width ?? 0).arg(root.graphic.height ?? 0)
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                }
                IconButton {
                    objectName: "graphicEditSaveTemplate"
                    compact: true
                    iconName: "save"
                    tooltip: qsTr("Save as template…")
                    onClicked: root.saveRequested()
                }
            }

            // ---- A template's fields ----
            GraphicGroupLabel {
                visible: Graphics.fields.length > 0
                text: qsTr("Fields")
            }
            // A count, not the fields: every edit gives a new list, and the control being typed in or dragged must stay.
            Repeater {
                model: Graphics.fields.length
                delegate: ColumnLayout {
                    id: fieldRow
                    required property int index
                    readonly property var field: Graphics.fields[index] ?? ({ id: "", label: "", kind: "", value: "" })
                    readonly property string kind: field.kind ?? ""
                    objectName: "graphicField_" + (field.id ?? "")
                    Layout.fillWidth: true
                    spacing: Theme.space(0.5)

                    Text {
                        Layout.fillWidth: true
                        visible: fieldRow.kind !== "color" && fieldRow.kind !== "checkbox"
                        text: fieldRow.field.label ?? ""
                        elide: Text.ElideRight
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    FieldInput {
                        objectName: "graphicFieldText_" + (fieldRow.field.id ?? "")
                        Layout.fillWidth: true
                        visible: fieldRow.kind === "text"
                        text: fieldRow.kind === "text" ? String(fieldRow.field.value ?? "") : ""
                        Accessible.name: fieldRow.field.label ?? ""
                        onTextEdited: Graphics.setField(fieldRow.field.id, text)
                        onEditingFinished: Graphics.endEdit()
                    }
                    GraphicColorRow {
                        visible: fieldRow.kind === "color"
                        label: fieldRow.field.label ?? ""
                        key: "field_" + (fieldRow.field.id ?? "")
                        value: root.colourValue(fieldRow.kind === "color" ? fieldRow.field.value : Theme.contentWhite)
                        onPicked: color => {
                            Graphics.setField(fieldRow.field.id, color)
                            Graphics.endEdit()
                        }
                    }
                    GraphicChoicePill {
                        objectName: "graphicFieldFont_" + (fieldRow.field.id ?? "")
                        visible: fieldRow.kind === "font"
                        options: root.fontFamilies
                        current: String(fieldRow.field.value ?? "")
                        placeholder: String(fieldRow.field.value ?? "")
                        Accessible.name: fieldRow.field.label ?? ""
                        onChosen: id => {
                            Graphics.setField(fieldRow.field.id, id)
                            Graphics.endEdit()
                        }
                    }
                    GraphicChoicePill {
                        objectName: "graphicFieldImage_" + (fieldRow.field.id ?? "")
                        visible: fieldRow.kind === "image"
                        options: Graphics.media
                        current: String(fieldRow.field.value ?? "")
                        placeholder: qsTr("Choose a picture")
                        Accessible.name: fieldRow.field.label ?? ""
                        onChosen: id => {
                            Graphics.setField(fieldRow.field.id, id)
                            Graphics.endEdit()
                        }
                    }
                    NumberField {
                        objectName: "graphicFieldSeconds_" + (fieldRow.field.id ?? "")
                        Layout.preferredWidth: Theme.numberFieldWidth
                        visible: fieldRow.kind === "duration"
                        from: 0.04
                        to: 3600
                        stepSize: 0.1
                        decimals: 2
                        suffix: qsTr("s")
                        value: fieldRow.kind === "duration" ? Number(fieldRow.field.value ?? 0) : 0
                        Accessible.name: fieldRow.field.label ?? ""
                        onValueModified: v => Graphics.setField(fieldRow.field.id, v)
                        onScrubFinished: Graphics.endEdit()
                    }
                    Toggle {
                        objectName: "graphicFieldCheck_" + (fieldRow.field.id ?? "")
                        visible: fieldRow.kind === "checkbox"
                        text: fieldRow.field.label ?? ""
                        checked: fieldRow.kind === "checkbox" && fieldRow.field.value === true
                        onToggled: {
                            Graphics.setField(fieldRow.field.id, checked)
                            Graphics.endEdit()
                        }
                    }
                }
            }

            // ---- The selected layer's basics ----
            ColumnLayout {
                id: basics
                objectName: "calloutBasics"
                /// The selected layer (Graphics.layer); not `layer`, which is Item's.
                readonly property var sel: Graphics.layer
                readonly property int layerId: sel.id ?? 0
                readonly property string kind: sel.kind ?? ""
                readonly property var run: ((sel.text ?? {}).runs ?? [])[0] ?? ({})
                readonly property var shape: sel.shape ?? ({})
                /// A shape drawn by its outline only (the circle callout's ring): its colour is the stroke's.
                readonly property bool outlineOnly: kind === "shape" && ((shape.fill ?? {}).type ?? "solid") === "none"
                /// The text and shape layers to choose from, top first.
                readonly property var choices: Graphics.layers.filter(l => l.kind === "text" || l.kind === "shape").map(l => ({
                    id: String(l.id),
                    name: l.kind === "text" && (l.text ?? "") !== "" ? qsTr("Text: %1").arg(l.text) : qsTr("Shape: %1").arg(l.name ?? "")
                }))
                Layout.fillWidth: true
                visible: choices.length > 0
                spacing: Theme.space(1)

                GraphicGroupLabel {
                    text: qsTr("Selected layer")
                }
                GraphicChoicePill {
                    objectName: "calloutLayer"
                    Layout.fillWidth: true
                    options: basics.choices
                    current: String(Graphics.selectedLayer)
                    placeholder: qsTr("Choose a text or shape")
                    Accessible.name: qsTr("Selected layer")
                    onChosen: id => Graphics.selectedLayer = Number(id)
                }
                FieldInput {
                    objectName: "calloutText"
                    Layout.fillWidth: true
                    visible: basics.kind === "text"
                    text: basics.kind === "text" ? String((basics.sel.text ?? {}).text ?? "") : ""
                    placeholderText: qsTr("Type the text")
                    Accessible.name: qsTr("Text")
                    onTextEdited: Graphics.setText(basics.layerId, text)
                    onEditingFinished: Graphics.endEdit()
                }
                GraphicChoicePill {
                    objectName: "calloutFont"
                    Layout.fillWidth: true
                    visible: basics.kind === "text"
                    options: root.fontFamilies
                    current: basics.run.fontFamily ?? ""
                    placeholder: (basics.run.fontFamily ?? "") !== "" ? basics.run.fontFamily : qsTr("Default font")
                    Accessible.name: qsTr("Font")
                    onChosen: id => {
                        if ((basics.run.brandFont ?? "") !== "") // a brand font role wins over the family: drop it
                            Graphics.setRun(basics.layerId, 0, "brandFont", "")
                        Graphics.setRun(basics.layerId, 0, "fontFamily", id)
                        Graphics.endEdit()
                    }
                }
                PropSlider {
                    objectName: "calloutSize"
                    Layout.fillWidth: true
                    visible: basics.kind === "text"
                    label: qsTr("Size")
                    unit: qsTr("px")
                    from: 8
                    to: 400
                    defaultValue: 72
                    value: Number(basics.run.fontSize ?? 72)
                    Accessible.name: qsTr("Font size")
                    onValueModified: v => Graphics.setRun(basics.layerId, 0, "fontSize", v)
                    onGestureEnded: Graphics.endEdit()
                }
                GraphicColorRow {
                    objectName: "calloutColor"
                    visible: basics.kind === "text" || basics.kind === "shape"
                    label: basics.kind === "text" ? qsTr("Text colour") : basics.outlineOnly ? qsTr("Outline colour") : qsTr("Fill colour")
                    key: "callout"
                    value: (basics.kind === "text" ? basics.run.fill
                            : basics.outlineOnly ? (basics.shape.stroke ?? {}).color
                            : (basics.shape.fill ?? {}).color) ?? ({ color: Theme.contentWhite, alpha: 1, brand: "" })
                    onPicked: color => {
                        Graphics.setColor(basics.layerId, basics.outlineOnly ? "stroke" : "fill", color)
                        Graphics.endEdit()
                    }
                }
            }

            // ---- Advanced: layers, every property, Responsive Design ----
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                IconButton {
                    objectName: "graphicsAdvanced"
                    compact: true
                    iconName: root.advanced ? "chevron-down" : "chevron-right"
                    tooltip: root.advanced ? qsTr("Hide the advanced editing")
                                           : qsTr("Show the advanced editing: layers, motion, presets, pins and responsive time")
                    onClicked: root.advanced = !root.advanced
                }
                GraphicGroupLabel {
                    Layout.fillWidth: true
                    Layout.topMargin: 0
                    text: qsTr("Advanced")
                }
            }
            ColumnLayout {
                objectName: "graphicsAdvancedSection"
                Layout.fillWidth: true
                visible: root.advanced
                spacing: Theme.space(2)

                // ---- Layers ----
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(1)
                    GraphicGroupLabel {
                        Layout.fillWidth: true
                        text: qsTr("Layers")
                    }
                    AddLayerRow {}
                }
                ColumnLayout {
                    objectName: "graphicLayerList"
                    Layout.fillWidth: true
                    spacing: 0
                    Accessible.role: Accessible.List
                    Accessible.name: qsTr("Layers: %n", "", Graphics.layers.length)

                    Repeater {
                        model: Graphics.layers.length
                        delegate: Rectangle {
                            id: layerRow
                            required property int index
                            readonly property var entry: Graphics.layers[index] ?? ({ id: 0, depth: 0, kind: "", name: "", visible: true })
                            readonly property int layerId: entry.id ?? 0
                            readonly property bool selected: Graphics.selectedLayer === layerId
                            /// 0.2: Ctrl-clicked into the selection (Align and Distribute use every selected layer).
                            readonly property bool alsoSelected: !selected && Graphics.selectedLayers.includes(layerId)
                            objectName: "graphicLayerRow_" + layerId
                            Layout.fillWidth: true
                            implicitHeight: Theme.rowHeight
                            radius: Theme.radiusField
                            color: selected || alsoSelected ? Theme.accentSoft : rowHover.hovered ? Theme.hover : "transparent"
                            border.width: alsoSelected ? Theme.hairline : 0
                            border.color: Theme.accentStroke
                            activeFocusOnTab: true
                            Accessible.role: Accessible.ListItem
                            Accessible.name: qsTr("%1 layer %2").arg(entry.kind ?? "").arg(entry.name ?? "")
                            Accessible.selected: selected || alsoSelected
                            Accessible.onPressAction: Graphics.selectedLayer = layerRow.layerId
                            // Space or Enter selects the layer (its buttons are the next tab stops).
                            Keys.onShortcutOverride: event => event.accepted = (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier | Qt.AltModifier)) === 0
                                                                               && [Qt.Key_Space, Qt.Key_Return, Qt.Key_Enter].includes(event.key)
                            Keys.onSpacePressed: Graphics.selectedLayer = layerRow.layerId
                            Keys.onReturnPressed: Graphics.selectedLayer = layerRow.layerId
                            Keys.onEnterPressed: Graphics.selectedLayer = layerRow.layerId

                            HoverHandler {
                                id: rowHover
                                cursorShape: Qt.PointingHandCursor
                            }
                            TapHandler {
                                acceptedModifiers: Qt.NoModifier
                                onTapped: Graphics.selectedLayer = layerRow.layerId
                            }
                            TapHandler {
                                objectName: "graphicLayerRowToggle"
                                acceptedModifiers: Qt.ControlModifier
                                onTapped: Graphics.toggleLayerSelected(layerRow.layerId)
                            }
                            FocusRing {
                                anchors.fill: parent
                                targetRadius: Theme.radiusField
                                visible: layerRow.activeFocus
                            }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.space(1) + (layerRow.entry.depth ?? 0) * Theme.space(4)
                                anchors.rightMargin: Theme.space(0.5)
                                spacing: Theme.space(1)
                                IconButton {
                                    objectName: "graphicLayerVisible_" + layerRow.layerId
                                    compact: true
                                    iconName: layerRow.entry.visible === false ? "eye-off" : "eye"
                                    tooltip: layerRow.entry.visible === false ? qsTr("Show %1").arg(layerRow.entry.name ?? "")
                                                                              : qsTr("Hide %1").arg(layerRow.entry.name ?? "")
                                    onClicked: Graphics.setLayerVisible(layerRow.layerId, layerRow.entry.visible === false)
                                }
                                Icon {
                                    name: root.kindIcons[layerRow.entry.kind] ?? "layers"
                                    size: Theme.iconSm
                                    color: Theme.textTertiary
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: layerRow.entry.kind === "text" && (layerRow.entry.text ?? "") !== "" && (layerRow.entry.name ?? "") === qsTr("Text")
                                          ? layerRow.entry.text : (layerRow.entry.name ?? "")
                                    elide: Text.ElideRight
                                    color: layerRow.entry.visible === false ? Theme.textTertiary : Theme.textPrimary
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textSm
                                    font.weight: layerRow.selected ? Font.DemiBold : Font.Normal
                                }
                                Text {
                                    visible: layerRow.entry.pinned === true
                                    text: qsTr("pinned")
                                    color: Theme.textTertiary
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textXs
                                }
                                IconButton {
                                    objectName: "graphicLayerUp_" + layerRow.layerId
                                    compact: true
                                    iconName: "chevron-up"
                                    enabled: (layerRow.entry.index ?? 0) < (layerRow.entry.siblings ?? 1) - 1
                                    command: "graphics.bringForward"
                                    tooltip: qsTr("Bring %1 forward").arg(layerRow.entry.name ?? "")
                                    onClicked: Graphics.moveLayer(layerRow.layerId, "forward")
                                }
                                IconButton {
                                    objectName: "graphicLayerDown_" + layerRow.layerId
                                    compact: true
                                    iconName: "chevron-down"
                                    enabled: (layerRow.entry.index ?? 0) > 0
                                    command: "graphics.sendBackward"
                                    tooltip: qsTr("Send %1 backward").arg(layerRow.entry.name ?? "")
                                    onClicked: Graphics.moveLayer(layerRow.layerId, "backward")
                                }
                                IconButton {
                                    objectName: "graphicLayerDelete_" + layerRow.layerId
                                    compact: true
                                    iconName: "trash-2"
                                    tooltip: qsTr("Delete %1").arg(layerRow.entry.name ?? "")
                                    onClicked: Graphics.removeLayer(layerRow.layerId)
                                }
                            }
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: Graphics.layers.length > 0 && Graphics.selectedLayer === 0
                    wrapMode: Text.Wrap
                    text: qsTr("Select a layer to edit its look, motion and pins.")
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }

                // ---- The selected layer, every property (made when Advanced opens) ----
                Loader {
                    Layout.fillWidth: true
                    active: root.advanced
                    visible: active
                    sourceComponent: GraphicLayerProperties {}
                }

                // ---- Responsive Design - Time ----
                GraphicGroupLabel {
                    text: qsTr("Responsive time")
                }
                Toggle {
                    objectName: "graphicResponsive"
                    text: qsTr("Keep the intro and outro when the clip is stretched")
                    checked: root.responsive !== null
                    onToggled: Graphics.setResponsive(checked, Number(root.graphic.seconds ?? 5), Math.min(1, Number(root.graphic.seconds ?? 5) / 4),
                                                      Math.min(1, Number(root.graphic.seconds ?? 5) / 4))
                }
                GridLayout {
                    Layout.fillWidth: true
                    visible: root.responsive !== null
                    columns: 2
                    columnSpacing: Theme.space(2)
                    rowSpacing: Theme.space(1)
                    Repeater {
                        model: [{ key: "duration", name: qsTr("Designed length") }, { key: "intro", name: qsTr("Intro") },
                            { key: "outro", name: qsTr("Outro") }]
                        delegate: RowLayout {
                            id: timeRow
                            required property var modelData
                            Layout.columnSpan: 2
                            Layout.fillWidth: true
                            spacing: Theme.space(1)
                            Text {
                                Layout.fillWidth: true
                                text: timeRow.modelData.name
                                color: Theme.textSecondary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textSm
                            }
                            NumberField {
                                objectName: "graphicResponsive_" + timeRow.modelData.key
                                Layout.preferredWidth: Theme.numberFieldWidth
                                from: 0
                                to: 3600
                                stepSize: 0.1
                                decimals: 2
                                suffix: qsTr("s")
                                value: Number((root.responsive ?? {})[timeRow.modelData.key] ?? 0)
                                Accessible.name: timeRow.modelData.name
                                onValueModified: v => {
                                    const r = root.responsive ?? { duration: 5, intro: 0, outro: 0 }
                                    Graphics.setResponsive(true, timeRow.modelData.key === "duration" ? v : r.duration,
                                                           timeRow.modelData.key === "intro" ? v : r.intro,
                                                           timeRow.modelData.key === "outro" ? v : r.outro)
                                }
                            }
                        }
                    }
                }
                // ---- 0.2: Align and Distribute, Text Styles, Roll / Crawl ----
                GraphicToolsSection {
                    Layout.fillWidth: true
                }
            }

            Text {
                Layout.fillWidth: true
                visible: Graphics.message !== ""
                wrapMode: Text.Wrap
                text: Graphics.message
                color: Theme.warning
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
        }
    }
}
