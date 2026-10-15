pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// The selected layer of a graphic in Essential Graphics > Edit (Graphics.layer): its text (font, size, weight, colour,
// tracking, leading, alignment, paragraph box, background), shape (kind, size, corners, fill - solid or a linear / radial
// gradient with its stops -, stroke with dashes, trim), image, transform, blend, drop shadow and masks; then the
// animation presets (In / Out / Loop) and Responsive Design - Position (pin to a sibling's edges). Animatable numbers are
// GraphicParamRows (stopwatch, value, key at the playhead); a drag is one undo step.
ColumnLayout {
    id: root
    objectName: "graphicLayerProperties"

    /// The selected layer (Graphics.layer); not `layer`, which is Item's.
    readonly property var sel: Graphics.layer
    readonly property int layerId: sel.id ?? 0
    readonly property string kind: sel.kind ?? ""
    readonly property var text: sel.text ?? ({})
    readonly property var shape: sel.shape ?? ({})
    readonly property var image: sel.image ?? ({})
    readonly property var xform: sel.transform ?? ({})
    readonly property var shadow: sel.shadow ?? ({})
    readonly property var masks: sel.masks ?? []
    readonly property var runs: text.runs ?? []
    /// The text run the run controls edit (most graphics have one).
    property int runIndex: 0
    readonly property var run: runs[Math.min(runIndex, Math.max(0, runs.length - 1))] ?? ({})
    readonly property var canvas: Graphics.graphic

    readonly property var fonts: [
        { id: "", name: qsTr("Default (sans-serif)") }, { id: "Segoe UI", name: "Segoe UI" }, { id: "Arial", name: "Arial" },
        { id: "Inter", name: "Inter" }, { id: "Georgia", name: "Georgia" }, { id: "Noto Sans Thai", name: "Noto Sans Thai" },
        { id: "Cascadia Mono", name: "Cascadia Mono" }
    ].concat((Graphics.brand.fonts ?? []).map(f => ({ id: "brand:" + f.role, name: qsTr("Brand %1 (%2)").arg(f.role).arg(f.family) })))
    readonly property var weights: [
        { id: "300", name: qsTr("Light") }, { id: "400", name: qsTr("Regular") }, { id: "500", name: qsTr("Medium") },
        { id: "600", name: qsTr("Semibold") }, { id: "700", name: qsTr("Bold") }, { id: "900", name: qsTr("Black") }
    ]
    readonly property var blends: [
        { id: "normal", name: qsTr("Normal") }, { id: "multiply", name: qsTr("Multiply") }, { id: "screen", name: qsTr("Screen") },
        { id: "overlay", name: qsTr("Overlay") }, { id: "add", name: qsTr("Add") }, { id: "darken", name: qsTr("Darken") },
        { id: "lighten", name: qsTr("Lighten") }, { id: "difference", name: qsTr("Difference") }
    ]
    readonly property var shapeKinds: [
        { id: "rectangle", name: qsTr("Rectangle") }, { id: "ellipse", name: qsTr("Ellipse") }, { id: "polygon", name: qsTr("Polygon") },
        { id: "star", name: qsTr("Star") }, { id: "path", name: qsTr("Path") }
    ]
    readonly property var fills: ["none", "solid", "linear", "radial"]
    readonly property var joins: [{ id: "miter", name: qsTr("Miter") }, { id: "round", name: qsTr("Round") }, { id: "bevel", name: qsTr("Bevel") }]
    readonly property var caps: [{ id: "butt", name: qsTr("Butt") }, { id: "round", name: qsTr("Round") }, { id: "square", name: qsTr("Square") }]
    readonly property var fits: [{ id: "contain", name: qsTr("Contain") }, { id: "cover", name: qsTr("Cover") }, { id: "fill", name: qsTr("Fill") },
        { id: "none", name: qsTr("Actual size") }]
    readonly property var maskModes: [{ id: "add", name: qsTr("Add") }, { id: "subtract", name: qsTr("Subtract") },
        { id: "intersect", name: qsTr("Intersect") }]
    readonly property var presets: [
        { id: "", name: qsTr("None") }, { id: "fade", name: qsTr("Fade") }, { id: "slide", name: qsTr("Slide") },
        { id: "scalePop", name: qsTr("Scale pop") }, { id: "bounce", name: qsTr("Bounce") }, { id: "wipe", name: qsTr("Wipe") }
    ].concat(kind === "text" ? [{ id: "typewriter", name: qsTr("Typewriter") }, { id: "wordByWord", name: qsTr("Word by word") }] : [])
    readonly property var easings: [
        { id: "linear", name: qsTr("Linear") }, { id: "easeIn", name: qsTr("Ease in") }, { id: "easeOut", name: qsTr("Ease out") },
        { id: "easeInOut", name: qsTr("Ease in and out") }, { id: "overshoot", name: qsTr("Overshoot") }
    ]
    readonly property var directions: [{ id: "left", name: qsTr("Left") }, { id: "right", name: qsTr("Right") }, { id: "up", name: qsTr("Up") },
        { id: "down", name: qsTr("Down") }]

    spacing: Theme.space(1.5)
    visible: layerId !== 0

    // ---- The layer ----
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(1)
        FieldInput {
            objectName: "graphicLayerName"
            Layout.fillWidth: true
            text: root.sel.name ?? ""
            placeholderText: qsTr("Layer name")
            Accessible.name: qsTr("Layer name")
            onEditingFinished: {
                if (text !== (root.sel.name ?? ""))
                    Graphics.renameLayer(root.layerId, text)
            }
        }
    }

    // ---- Text ----
    GraphicGroupLabel {
        visible: root.kind === "text"
        text: qsTr("Text")
    }
    ColumnLayout {
        objectName: "graphicTextSection"
        Layout.fillWidth: true
        visible: root.kind === "text"
        spacing: Theme.space(1)

        FieldInput {
            objectName: "graphicText"
            Layout.fillWidth: true
            text: root.text.text ?? ""
            placeholderText: qsTr("Type the text")
            Accessible.name: qsTr("Text")
            onTextEdited: Graphics.setText(root.layerId, text)
            onEditingFinished: Graphics.endEdit()
        }
        RowLayout {
            Layout.fillWidth: true
            visible: root.runs.length > 1
            spacing: Theme.space(1)
            Text {
                text: qsTr("Style of")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            GraphicChoicePill {
                objectName: "graphicRunPicker"
                options: root.runs.map((r, i) => ({ id: String(i), name: qsTr("Part %1: %2").arg(i + 1).arg(r.text) }))
                current: String(root.runIndex)
                Accessible.name: qsTr("Text part")
                onChosen: id => root.runIndex = Number(id)
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            GraphicChoicePill {
                objectName: "graphicFont"
                Layout.fillWidth: true
                options: root.fonts
                current: (root.run.brandFont ?? "") !== "" ? "brand:" + root.run.brandFont : (root.run.fontFamily ?? "")
                placeholder: root.run.fontFamily ?? ""
                Accessible.name: qsTr("Font")
                onChosen: id => {
                    if (id.startsWith("brand:")) {
                        Graphics.setRun(root.layerId, root.runIndex, "brandFont", id.slice(6))
                    } else {
                        Graphics.setRun(root.layerId, root.runIndex, "brandFont", "")
                        Graphics.setRun(root.layerId, root.runIndex, "fontFamily", id)
                    }
                    Graphics.endEdit()
                }
            }
            GraphicChoicePill {
                objectName: "graphicWeight"
                options: root.weights
                current: String(root.run.fontWeight ?? 400)
                Accessible.name: qsTr("Font weight")
                onChosen: id => {
                    Graphics.setRun(root.layerId, root.runIndex, "fontWeight", Number(id))
                    Graphics.endEdit()
                }
            }
        }
        PropSlider {
            objectName: "graphicFontSize"
            Layout.fillWidth: true
            label: qsTr("Size")
            unit: qsTr("px")
            from: 8
            to: 400
            defaultValue: 72
            value: Number(root.run.fontSize ?? 72)
            Accessible.name: qsTr("Font size")
            onValueModified: v => Graphics.setRun(root.layerId, root.runIndex, "fontSize", v)
            onGestureEnded: Graphics.endEdit()
        }
        PropSlider {
            objectName: "graphicTracking"
            Layout.fillWidth: true
            label: qsTr("Tracking")
            from: -200
            to: 1000
            origin: 0
            defaultValue: 0
            value: Number(root.run.tracking ?? 0)
            Accessible.name: qsTr("Tracking")
            onValueModified: v => Graphics.setRun(root.layerId, root.runIndex, "tracking", v)
            onGestureEnded: Graphics.endEdit()
        }
        PropSlider {
            objectName: "graphicLeading"
            Layout.fillWidth: true
            label: qsTr("Leading (0 = automatic)")
            unit: qsTr("px")
            from: 0
            to: 400
            defaultValue: 0
            value: Number(root.run.leading ?? 0)
            Accessible.name: qsTr("Leading")
            onValueModified: v => Graphics.setRun(root.layerId, root.runIndex, "leading", v)
            onGestureEnded: Graphics.endEdit()
        }
        GraphicColorRow {
            label: qsTr("Colour")
            key: "textFill"
            value: root.run.fill ?? ({ color: Theme.contentWhite, alpha: 1, brand: "" })
            onPicked: color => {
                Graphics.setRun(root.layerId, root.runIndex, "fill", color)
                Graphics.endEdit()
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            Repeater {
                model: [{ id: "left", icon: "align-left", name: qsTr("Align left") }, { id: "center", icon: "align-center", name: qsTr("Centre") },
                    { id: "right", icon: "align-right", name: qsTr("Align right") }]
                delegate: IconButton {
                    required property var modelData
                    objectName: "graphicAlign_" + modelData.id
                    compact: true
                    checkable: true
                    checked: (root.text.align ?? "left") === modelData.id
                    iconName: modelData.icon
                    tooltip: modelData.name
                    onClicked: {
                        checked = Qt.binding(() => (root.text.align ?? "left") === modelData.id)
                        Graphics.updateLayer(root.layerId, { text: { align: modelData.id } })
                    }
                }
            }
            PillButton {
                objectName: "graphicAlign_justify"
                compact: true
                variant: (root.text.align ?? "") === "justify" ? "primary" : "ghost"
                text: qsTr("Justify")
                tooltip: qsTr("Justify paragraph text")
                onClicked: Graphics.updateLayer(root.layerId, { text: { align: "justify" } })
            }
            Item { Layout.fillWidth: true }
        }
        Toggle {
            objectName: "graphicParagraph"
            text: qsTr("Paragraph box")
            tooltip: qsTr("Wrap the text at a width (between words; Thai too)")
            checked: Number(root.text.boxWidth ?? 0) > 0
            onToggled: Graphics.updateLayer(root.layerId, { text: { boxWidth: checked ? Math.round((root.canvas.width ?? 1920) * 0.4) : 0, boxHeight: 0 } })
        }
        RowLayout {
            Layout.fillWidth: true
            visible: Number(root.text.boxWidth ?? 0) > 0
            spacing: Theme.space(1)
            Text {
                Layout.fillWidth: true
                text: qsTr("Box width and height (0 = fits)")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            NumberField {
                objectName: "graphicBoxWidth"
                Layout.preferredWidth: Theme.numberFieldWidth
                from: 1
                to: 16384
                value: Number(root.text.boxWidth ?? 0)
                suffix: qsTr("px")
                Accessible.name: qsTr("Box width")
                onValueModified: v => Graphics.updateLayer(root.layerId, { text: { boxWidth: Math.round(v) } }, "boxWidth")
                onScrubFinished: Graphics.endEdit()
            }
            NumberField {
                objectName: "graphicBoxHeight"
                Layout.preferredWidth: Theme.numberFieldWidth
                from: 0
                to: 16384
                value: Number(root.text.boxHeight ?? 0)
                suffix: qsTr("px")
                Accessible.name: qsTr("Box height")
                onValueModified: v => Graphics.updateLayer(root.layerId, { text: { boxHeight: Math.round(v) } }, "boxHeight")
                onScrubFinished: Graphics.endEdit()
            }
        }
        Toggle {
            objectName: "graphicBackground"
            text: qsTr("Background box")
            checked: (root.text.background ?? {}).enabled === true
            onToggled: Graphics.updateLayer(root.layerId, { text: { background: { enabled: checked } } })
        }
        ColumnLayout {
            Layout.fillWidth: true
            visible: (root.text.background ?? {}).enabled === true
            spacing: Theme.space(1)
            GraphicColorRow {
                label: qsTr("Background")
                key: "textBackground"
                value: (root.text.background ?? {}).color ?? ({ color: String(Theme.videoBlack).toUpperCase(), alpha: 0.6, brand: "" })
                onPicked: color => {
                    Graphics.setColor(root.layerId, "background", color)
                    Graphics.endEdit()
                }
            }
            PropSlider {
                objectName: "graphicBackgroundPadding"
                Layout.fillWidth: true
                label: qsTr("Padding")
                unit: qsTr("px")
                from: 0
                to: 200
                defaultValue: 12
                value: Number((root.text.background ?? {}).padding ?? 12)
                onValueModified: v => Graphics.updateLayer(root.layerId, { text: { background: { padding: v } } }, "padding")
                onGestureEnded: Graphics.endEdit()
            }
            PropSlider {
                objectName: "graphicBackgroundRadius"
                Layout.fillWidth: true
                label: qsTr("Corner radius")
                unit: qsTr("px")
                from: 0
                to: 200
                defaultValue: 0
                value: Number((root.text.background ?? {}).radius ?? 0)
                onValueModified: v => Graphics.updateLayer(root.layerId, { text: { background: { radius: v } } }, "radius")
                onGestureEnded: Graphics.endEdit()
            }
        }
    }

    // ---- Shape ----
    GraphicGroupLabel {
        visible: root.kind === "shape"
        text: qsTr("Shape")
    }
    ColumnLayout {
        objectName: "graphicShapeSection"
        Layout.fillWidth: true
        visible: root.kind === "shape"
        spacing: Theme.space(1)

        GraphicChoicePill {
            objectName: "graphicShapeKind"
            options: root.shapeKinds
            current: root.shape.type ?? "rectangle"
            Accessible.name: qsTr("Shape kind")
            onChosen: id => Graphics.updateLayer(root.layerId, { shape: { type: id } })
        }
        GraphicParamRow {
            visible: ["rectangle", "ellipse"].includes(root.shape.type ?? "rectangle")
            layerId: root.layerId
            path: "shape.width"
            param: root.shape.width ?? ({})
            label: qsTr("Width")
            unit: qsTr("px")
            from: 0
            to: 4000
            defaultValue: 200
        }
        GraphicParamRow {
            visible: ["rectangle", "ellipse"].includes(root.shape.type ?? "rectangle")
            layerId: root.layerId
            path: "shape.height"
            param: root.shape.height ?? ({})
            label: qsTr("Height")
            unit: qsTr("px")
            from: 0
            to: 4000
            defaultValue: 100
        }
        GraphicParamRow {
            visible: (root.shape.type ?? "rectangle") === "rectangle"
            layerId: root.layerId
            path: "shape.cornerRadius"
            param: root.shape.cornerRadius ?? ({})
            label: qsTr("Corner radius")
            unit: qsTr("px")
            from: 0
            to: 1000
            defaultValue: 0
        }
        RowLayout {
            Layout.fillWidth: true
            visible: ["polygon", "star"].includes(root.shape.type ?? "")
            spacing: Theme.space(1)
            Text {
                Layout.fillWidth: true
                text: qsTr("Points")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            NumberField {
                objectName: "graphicShapePoints"
                Layout.preferredWidth: Theme.numberFieldWidth
                from: 3
                to: 64
                value: Number(root.shape.points ?? 5)
                Accessible.name: qsTr("Points")
                onValueModified: v => Graphics.updateLayer(root.layerId, { shape: { points: Math.round(v) } }, "points")
                onScrubFinished: Graphics.endEdit()
            }
        }
        GraphicParamRow {
            visible: ["polygon", "star"].includes(root.shape.type ?? "")
            layerId: root.layerId
            path: "shape.outerRadius"
            param: root.shape.outerRadius ?? ({})
            label: qsTr("Outer radius")
            unit: qsTr("px")
            from: 0
            to: 2000
            defaultValue: 100
        }
        GraphicParamRow {
            visible: (root.shape.type ?? "") === "star"
            layerId: root.layerId
            path: "shape.innerRadius"
            param: root.shape.innerRadius ?? ({})
            label: qsTr("Inner radius")
            unit: qsTr("px")
            from: 0
            to: 2000
            defaultValue: 50
        }

        // Fill: none, a colour or a gradient with its stops.
        PillTabs {
            id: fillTabs
            objectName: "graphicFillKind"
            Layout.fillWidth: true
            compact: true
            model: [qsTr("None"), qsTr("Solid"), qsTr("Linear"), qsTr("Radial")]
            currentIndex: Math.max(0, root.fills.indexOf((root.shape.fill ?? {}).type ?? "solid"))
            Accessible.name: qsTr("Fill")
            onActivated: index => {
                // The tabs set their index; the layer's fill is the source of truth again after the edit.
                fillTabs.currentIndex = Qt.binding(() => Math.max(0, root.fills.indexOf((root.shape.fill ?? {}).type ?? "solid")))
                Graphics.setFillKind(root.layerId, root.fills[index])
            }
        }
        GraphicColorRow {
            visible: ((root.shape.fill ?? {}).type ?? "solid") === "solid"
            label: qsTr("Fill")
            key: "shapeFill"
            value: (root.shape.fill ?? {}).color ?? ({ color: Theme.contentWhite, alpha: 1, brand: "" })
            onPicked: color => {
                Graphics.setColor(root.layerId, "fill", color)
                Graphics.endEdit()
            }
        }
        ColumnLayout {
            objectName: "graphicGradientStops"
            Layout.fillWidth: true
            visible: ["linear", "radial"].includes((root.shape.fill ?? {}).type ?? "")
            spacing: Theme.space(1)
            // A count, not the stops: every edit gives a new list, and the rows being dragged must stay.
            Repeater {
                model: ((root.shape.fill ?? {}).stops ?? []).length
                delegate: ColumnLayout {
                    id: stopRow
                    required property int index
                    readonly property var stop: ((root.shape.fill ?? {}).stops ?? [])[index] ?? ({ offset: 0, color: {} })
                    Layout.fillWidth: true
                    spacing: Theme.space(0.5)
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(1)
                        PropSlider {
                            objectName: "graphicStopOffset" + stopRow.index
                            Layout.fillWidth: true
                            label: qsTr("Stop %1").arg(stopRow.index + 1)
                            unit: "%"
                            from: 0
                            to: 100
                            value: Number(stopRow.stop.offset ?? 0) * 100
                            Accessible.name: qsTr("Gradient stop %1 position").arg(stopRow.index + 1)
                            onValueModified: v => Graphics.setStopOffset(root.layerId, stopRow.index, v / 100)
                            onGestureEnded: Graphics.endEdit()
                        }
                        IconButton {
                            objectName: "graphicStopRemove" + stopRow.index
                            compact: true
                            iconName: "x"
                            enabled: ((root.shape.fill ?? {}).stops ?? []).length > 2
                            tooltip: qsTr("Remove gradient stop %1").arg(stopRow.index + 1)
                            onClicked: Graphics.removeStop(root.layerId, stopRow.index)
                        }
                    }
                    GraphicColorRow {
                        label: qsTr("Stop %1 colour").arg(stopRow.index + 1)
                        key: "stop" + stopRow.index
                        value: stopRow.stop.color ?? ({ color: Theme.contentWhite, alpha: 1, brand: "" })
                        onPicked: color => {
                            Graphics.setColor(root.layerId, "stop:" + stopRow.index, color)
                            Graphics.endEdit()
                        }
                    }
                }
            }
            PillButton {
                objectName: "graphicStopAdd"
                compact: true
                variant: "ghost"
                iconName: "plus"
                text: qsTr("Add stop")
                onClicked: Graphics.addStop(root.layerId)
            }
        }

        // Stroke.
        Toggle {
            objectName: "graphicStroke"
            text: qsTr("Stroke")
            checked: (root.shape.stroke ?? {}).enabled === true
            onToggled: Graphics.setStroke(root.layerId, "enabled", checked)
        }
        ColumnLayout {
            Layout.fillWidth: true
            visible: (root.shape.stroke ?? {}).enabled === true
            spacing: Theme.space(1)
            GraphicColorRow {
                label: qsTr("Stroke")
                key: "shapeStroke"
                value: (root.shape.stroke ?? {}).color ?? ({ color: String(Theme.videoBlack).toUpperCase(), alpha: 1, brand: "" })
                onPicked: color => {
                    Graphics.setColor(root.layerId, "stroke", color)
                    Graphics.endEdit()
                }
            }
            GraphicParamRow {
                layerId: root.layerId
                path: "stroke.width"
                param: (root.shape.stroke ?? {}).width ?? ({})
                label: qsTr("Stroke width")
                unit: qsTr("px")
                from: 0
                to: 200
                defaultValue: 2
                decimals: 1
                stepSize: 0.5
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                FieldInput {
                    objectName: "graphicDashes"
                    Layout.fillWidth: true
                    text: (root.shape.stroke ?? {}).dashes ?? ""
                    placeholderText: qsTr("Dashes, e.g. 12 6 (solid when empty)")
                    Accessible.name: qsTr("Dash pattern")
                    onEditingFinished: {
                        if (text !== ((root.shape.stroke ?? {}).dashes ?? ""))
                            Graphics.setStroke(root.layerId, "dashes", text)
                    }
                }
                GraphicChoicePill {
                    objectName: "graphicJoin"
                    options: root.joins
                    current: (root.shape.stroke ?? {}).join ?? "miter"
                    Accessible.name: qsTr("Line join")
                    onChosen: id => Graphics.setStroke(root.layerId, "join", id)
                }
                GraphicChoicePill {
                    objectName: "graphicCap"
                    options: root.caps
                    current: (root.shape.stroke ?? {}).cap ?? "butt"
                    Accessible.name: qsTr("Line cap")
                    onChosen: id => Graphics.setStroke(root.layerId, "cap", id)
                }
            }
        }
        GraphicParamRow {
            layerId: root.layerId
            path: "shape.trimStart"
            param: root.shape.trimStart ?? ({})
            label: qsTr("Trim start")
            unit: "%"
            from: 0
            to: 100
            defaultValue: 0
        }
        GraphicParamRow {
            layerId: root.layerId
            path: "shape.trimEnd"
            param: root.shape.trimEnd ?? ({})
            label: qsTr("Trim end")
            unit: "%"
            from: 0
            to: 100
            defaultValue: 100
        }
    }

    // ---- Image ----
    GraphicGroupLabel {
        visible: root.kind === "image"
        text: qsTr("Image")
    }
    ColumnLayout {
        objectName: "graphicImageSection"
        Layout.fillWidth: true
        visible: root.kind === "image"
        spacing: Theme.space(1)
        GraphicChoicePill {
            objectName: "graphicImageMedia"
            Layout.fillWidth: true
            options: [{ id: "", name: qsTr("No picture") }].concat(Graphics.media)
            current: root.image.media ?? ""
            Accessible.name: qsTr("Picture")
            onChosen: id => Graphics.updateLayer(root.layerId, { image: { media: id === "" ? 0 : Number(id.slice(1)) } })
        }
        GraphicChoicePill {
            objectName: "graphicImageLogo"
            Layout.fillWidth: true
            visible: (Graphics.brand.logos ?? []).length > 0
            options: [{ id: "", name: qsTr("No brand logo") }].concat((Graphics.brand.logos ?? []).map(l => ({ id: l.name, name: qsTr("Brand logo %1").arg(l.name) })))
            current: root.image.brandLogo ?? ""
            Accessible.name: qsTr("Brand logo")
            onChosen: id => Graphics.updateLayer(root.layerId, { image: { brandLogo: id } })
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            NumberField {
                objectName: "graphicImageWidth"
                Layout.fillWidth: true
                from: 1
                to: 16384
                value: Number(root.image.width ?? 400)
                suffix: qsTr("px")
                Accessible.name: qsTr("Image width")
                onValueModified: v => Graphics.updateLayer(root.layerId, { image: { width: v } }, "imageWidth")
                onScrubFinished: Graphics.endEdit()
            }
            NumberField {
                objectName: "graphicImageHeight"
                Layout.fillWidth: true
                from: 1
                to: 16384
                value: Number(root.image.height ?? 300)
                suffix: qsTr("px")
                Accessible.name: qsTr("Image height")
                onValueModified: v => Graphics.updateLayer(root.layerId, { image: { height: v } }, "imageHeight")
                onScrubFinished: Graphics.endEdit()
            }
            GraphicChoicePill {
                objectName: "graphicImageFit"
                options: root.fits
                current: root.image.fit ?? "contain"
                Accessible.name: qsTr("Fit")
                onChosen: id => Graphics.updateLayer(root.layerId, { image: { fit: id } })
            }
        }
    }

    // ---- Transform ----
    GraphicGroupLabel {
        text: qsTr("Transform")
    }
    GraphicParamRow {
        layerId: root.layerId
        path: "transform.x"
        param: root.xform.x ?? ({})
        label: qsTr("Position X")
        unit: qsTr("px")
        from: -Number(root.canvas.width ?? 1920)
        to: 2 * Number(root.canvas.width ?? 1920)
        origin: 0
    }
    GraphicParamRow {
        layerId: root.layerId
        path: "transform.y"
        param: root.xform.y ?? ({})
        label: qsTr("Position Y")
        unit: qsTr("px")
        from: -Number(root.canvas.height ?? 1080)
        to: 2 * Number(root.canvas.height ?? 1080)
        origin: 0
    }
    GraphicParamRow {
        layerId: root.layerId
        path: "transform.scaleX"
        param: root.xform.scaleX ?? ({})
        label: qsTr("Scale X")
        unit: "%"
        from: 0
        to: 400
        defaultValue: 100
    }
    GraphicParamRow {
        layerId: root.layerId
        path: "transform.scaleY"
        param: root.xform.scaleY ?? ({})
        label: qsTr("Scale Y")
        unit: "%"
        from: 0
        to: 400
        defaultValue: 100
    }
    GraphicParamRow {
        layerId: root.layerId
        path: "transform.rotation"
        param: root.xform.rotation ?? ({})
        label: qsTr("Rotation")
        unit: qsTr("°")
        from: -360
        to: 360
        origin: 0
        decimals: 1
        stepSize: 0.5
    }
    GraphicParamRow {
        layerId: root.layerId
        path: "transform.anchorX"
        param: root.xform.anchorX ?? ({})
        label: qsTr("Anchor X")
        unit: qsTr("px")
        from: -2000
        to: 2000
        origin: 0
    }
    GraphicParamRow {
        layerId: root.layerId
        path: "transform.anchorY"
        param: root.xform.anchorY ?? ({})
        label: qsTr("Anchor Y")
        unit: qsTr("px")
        from: -2000
        to: 2000
        origin: 0
    }
    GraphicParamRow {
        layerId: root.layerId
        path: "transform.opacity"
        param: root.xform.opacity ?? ({})
        label: qsTr("Opacity")
        unit: "%"
        from: 0
        to: 100
        defaultValue: 100
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(1)
        Text {
            Layout.fillWidth: true
            text: qsTr("Blend")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        GraphicChoicePill {
            objectName: "graphicBlend"
            options: root.blends
            current: root.sel.blend ?? "normal"
            Accessible.name: qsTr("Blend mode")
            onChosen: id => Graphics.updateLayer(root.layerId, { blend: id })
        }
    }

    // ---- Drop shadow ----
    Toggle {
        objectName: "graphicShadow"
        text: qsTr("Drop shadow")
        checked: root.shadow.enabled === true
        onToggled: Graphics.updateLayer(root.layerId, { shadow: { enabled: checked } })
    }
    ColumnLayout {
        Layout.fillWidth: true
        visible: root.shadow.enabled === true
        spacing: Theme.space(1)
        GraphicColorRow {
            label: qsTr("Shadow")
            key: "shadow"
            value: root.shadow.color ?? ({ color: String(Theme.videoBlack).toUpperCase(), alpha: 1, brand: "" })
            onPicked: color => {
                Graphics.setColor(root.layerId, "shadow", color)
                Graphics.endEdit()
            }
        }
        PropSlider {
            objectName: "graphicShadowOpacity"
            Layout.fillWidth: true
            label: qsTr("Shadow opacity")
            unit: "%"
            from: 0
            to: 100
            defaultValue: 75
            value: Number(root.shadow.opacity ?? 0.75) * 100
            onValueModified: v => Graphics.updateLayer(root.layerId, { shadow: { opacity: v / 100 } }, "shadowOpacity")
            onGestureEnded: Graphics.endEdit()
        }
        PropSlider {
            objectName: "graphicShadowAngle"
            Layout.fillWidth: true
            label: qsTr("Angle")
            unit: qsTr("°")
            from: 0
            to: 360
            defaultValue: 135
            value: Number(root.shadow.angle ?? 135)
            onValueModified: v => Graphics.updateLayer(root.layerId, { shadow: { angle: v } }, "shadowAngle")
            onGestureEnded: Graphics.endEdit()
        }
        PropSlider {
            objectName: "graphicShadowDistance"
            Layout.fillWidth: true
            label: qsTr("Distance")
            unit: qsTr("px")
            from: 0
            to: 200
            defaultValue: 6
            value: Number(root.shadow.distance ?? 6)
            onValueModified: v => Graphics.updateLayer(root.layerId, { shadow: { distance: v } }, "shadowDistance")
            onGestureEnded: Graphics.endEdit()
        }
        PropSlider {
            objectName: "graphicShadowSoftness"
            Layout.fillWidth: true
            label: qsTr("Softness")
            unit: qsTr("px")
            from: 0
            to: 200
            defaultValue: 8
            value: Number(root.shadow.softness ?? 8)
            onValueModified: v => Graphics.updateLayer(root.layerId, { shadow: { softness: v } }, "shadowSoftness")
            onGestureEnded: Graphics.endEdit()
        }
    }

    // ---- Masks ----
    GraphicGroupLabel {
        visible: root.masks.length > 0
        text: qsTr("Masks")
    }
    Repeater {
        model: root.masks.length
        delegate: RowLayout {
            id: maskRow
            required property int index
            readonly property var mask: root.masks[index] ?? ({})
            Layout.fillWidth: true
            spacing: Theme.space(1)
            Text {
                Layout.fillWidth: true
                text: (maskRow.mask.name ?? "") !== "" ? maskRow.mask.name : qsTr("Mask %1").arg(maskRow.index + 1)
                elide: Text.ElideRight
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            GraphicChoicePill {
                objectName: "graphicMaskMode" + maskRow.index
                options: root.maskModes
                current: maskRow.mask.mode ?? "add"
                Accessible.name: qsTr("Mask %1 mode").arg(maskRow.index + 1)
                onChosen: id => Graphics.setMask(root.layerId, maskRow.index, "mode", id)
            }
            Toggle {
                objectName: "graphicMaskInverted" + maskRow.index
                text: qsTr("Inverted")
                checked: maskRow.mask.inverted === true
                onToggled: Graphics.setMask(root.layerId, maskRow.index, "inverted", checked)
            }
            IconButton {
                objectName: "graphicMaskRemove" + maskRow.index
                compact: true
                iconName: "trash-2"
                tooltip: qsTr("Delete mask %1").arg(maskRow.index + 1)
                onClicked: Graphics.removeMask(root.layerId, maskRow.index)
            }
        }
    }

    // ---- Animation presets ----
    GraphicGroupLabel {
        text: qsTr("Animation")
    }
    Repeater {
        model: [{ id: "in", name: qsTr("In") }, { id: "out", name: qsTr("Out") }, { id: "loop", name: qsTr("Loop") }]
        delegate: RowLayout {
            id: presetRow
            required property var modelData
            property string preset: ""
            property real seconds: 0.5
            property string easing: "easeOut"
            property string direction: "left"
            Layout.fillWidth: true
            spacing: Theme.space(1)
            Text {
                Layout.preferredWidth: Theme.space(10)
                text: presetRow.modelData.name
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            GraphicChoicePill {
                objectName: "graphicPreset_" + presetRow.modelData.id
                options: root.presets
                current: presetRow.preset
                Accessible.name: qsTr("%1 animation").arg(presetRow.modelData.name)
                onChosen: id => presetRow.preset = id
            }
            NumberField {
                objectName: "graphicPresetSeconds_" + presetRow.modelData.id
                Layout.preferredWidth: Theme.numberFieldWidth
                from: 0.04
                to: 60
                stepSize: 0.1
                decimals: 2
                value: presetRow.seconds
                suffix: qsTr("s")
                Accessible.name: qsTr("%1 animation length").arg(presetRow.modelData.name)
                onValueModified: v => presetRow.seconds = v
            }
            GraphicChoicePill {
                objectName: "graphicPresetEasing_" + presetRow.modelData.id
                options: root.easings
                current: presetRow.easing
                Accessible.name: qsTr("%1 animation easing").arg(presetRow.modelData.name)
                onChosen: id => presetRow.easing = id
            }
            GraphicChoicePill {
                objectName: "graphicPresetDirection_" + presetRow.modelData.id
                visible: ["slide", "wipe", "bounce"].includes(presetRow.preset)
                options: root.directions
                current: presetRow.direction
                Accessible.name: qsTr("%1 animation direction").arg(presetRow.modelData.name)
                onChosen: id => presetRow.direction = id
            }
            Item { Layout.fillWidth: true }
            PillButton {
                objectName: "graphicPresetApply_" + presetRow.modelData.id
                compact: true
                variant: "secondary"
                text: qsTr("Apply")
                enabled: presetRow.preset !== ""
                tooltip: qsTr("Write the %1 animation as keyframes you can edit").arg(presetRow.modelData.name.toLowerCase())
                onClicked: Graphics.applyPreset(root.layerId, presetRow.preset, presetRow.modelData.id, presetRow.seconds,
                                                presetRow.easing, presetRow.direction)
            }
        }
    }

    // ---- Responsive Design - Position: pins ----
    GraphicGroupLabel {
        text: qsTr("Pin to")
    }
    RowLayout {
        id: pinRow
        Layout.fillWidth: true
        spacing: Theme.space(1)
        readonly property var pin: root.sel.pin ?? null
        readonly property var targets: [{ id: "0", name: qsTr("Nothing") }].concat(
            Graphics.pinTargets(root.layerId).map(t => ({ id: String(t.id), name: t.name })))
        property var edges: ["left", "top", "right", "bottom"]
        GraphicChoicePill {
            objectName: "graphicPinTarget"
            options: pinRow.targets
            current: String(pinRow.pin ? pinRow.pin.target : 0)
            Accessible.name: qsTr("Pin to layer")
            onChosen: id => Graphics.setPin(root.layerId, Number(id), pinRow.pin ? pinRow.pin.edges : ["left", "top", "right", "bottom"])
        }
        Repeater {
            model: [{ id: "left", name: qsTr("L"), full: qsTr("Left edge") }, { id: "top", name: qsTr("T"), full: qsTr("Top edge") },
                { id: "right", name: qsTr("R"), full: qsTr("Right edge") }, { id: "bottom", name: qsTr("B"), full: qsTr("Bottom edge") }]
            delegate: PillButton {
                required property var modelData
                objectName: "graphicPinEdge_" + modelData.id
                compact: true
                enabled: pinRow.pin !== null
                variant: pinRow.pin && pinRow.pin.edges.includes(modelData.id) ? "primary" : "ghost"
                text: modelData.name
                tooltip: modelData.full
                Accessible.name: qsTr("Pin %1").arg(modelData.full)
                onClicked: {
                    const current = pinRow.pin ? pinRow.pin.edges.slice() : []
                    const at = current.indexOf(modelData.id)
                    if (at >= 0)
                        current.splice(at, 1)
                    else
                        current.push(modelData.id)
                    Graphics.setPin(root.layerId, current.length > 0 ? pinRow.pin.target : 0, current)
                }
            }
        }
        Item { Layout.fillWidth: true }
    }
}
