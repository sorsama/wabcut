pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The gaming layout picker (M5o, bridge `Layouts`, the core's C.15 layout.apply): a facecam in a corner over the
// gameplay, a vertical split (gameplay and facecam stacked) or a vertical blurred fill, one click. Three cards with live
// schematics of the options, the facecam (a clip above the gameplay, or a region drawn over the webcam inside the
// capture), the options of the chosen layout, and Apply (one undo step). While it is open the Program Monitor shows the
// schematic (LayoutMonitorOverlay) and every change is checked by a dry run. It stays open while you draw on the
// monitor: Esc or Cancel closes it.
Popover {
    id: picker
    objectName: "layoutPicker"

    /// layout.apply's options (Layouts.params maps them), plus "preset" and "facecamSource" ("auto" | "clip" | "region").
    property var options: ({})
    readonly property string preset: options.preset ?? "pip"
    readonly property var current: Layouts.layouts.find(l => l.preset === picker.preset) ?? ({})
    readonly property bool vertical: current.vertical === true
    // The corner look applies when layout.apply gets it (picture-in-picture; the blur fill with a facecam). Reading
    // frameAspect re-evaluates this when the targets change.
    readonly property bool showsCornerOptions: Layouts.frameAspect > 0 && Layouts.params(picker.options).corner !== undefined
    readonly property var result: Layouts.previewResult
    property string error: ""

    function reset() {
        options = Object.assign({}, Layouts.defaults, { preset: "pip", facecamSource: "auto" })
        error = ""
    }
    /// Sets one option and checks the layout again.
    function set(key, value) {
        const next = Object.assign({}, options)
        next[key] = value
        options = next
        Layouts.preview(options)
    }
    /// Opens the picker over `host` (the window's content), beside `monitor` (the Program Monitor) when given, so the
    /// schematic stays in view and a region can be drawn on it: right of it when there is room, else left of it.
    function openFor(host, monitor) {
        Layouts.refreshTargets()
        reset()
        if (host) {
            parent = host
            const gap = Theme.space(2)
            let px = Math.round((host.width - width) / 2)
            if (monitor) {
                const m = monitor.mapToItem(host, 0, 0, monitor.width, monitor.height)
                if (host.width - (m.x + m.width) >= width + gap)
                    px = m.x + m.width + gap
                else if (m.x >= width + gap)
                    px = m.x - width - gap
                else
                    px = host.width - width - gap
            }
            x = Math.max(0, Math.min(host.width - width, px))
            y = Theme.space(16)
        }
        open()
        Layouts.preview(options)
    }
    function applyLayout() {
        error = Layouts.apply(options)
        if (error === "")
            close()
    }

    title: qsTr("Layout")
    closePolicy: Popup.CloseOnEscape
    onClosed: Layouts.previewShown = false

    component FieldLabel: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    component FieldValue: Text {
        color: Theme.textTertiary
        font.family: Theme.fontMono
        font.pixelSize: Theme.textXs
    }
    /// One layout: its schematic with the picker's options, its name and what it does.
    component LayoutCard: AbstractButton {
        id: card
        required property var modelData
        readonly property bool chosen: picker.preset === modelData.preset
        readonly property bool cardVertical: modelData.vertical === true
        objectName: "layoutCard_" + modelData.preset
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        implicitHeight: cardColumn.implicitHeight + Theme.space(3)
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        onClicked: picker.set("preset", modelData.preset)
        Accessible.role: Accessible.RadioButton
        Accessible.checkable: true
        Accessible.checked: card.chosen
        Accessible.name: modelData.name
        Accessible.description: modelData.description

        background: Rectangle {
            radius: Theme.radiusField
            color: card.chosen ? Theme.accentWash : card.hovered ? Theme.hover : Theme.sunken
            border.width: card.chosen ? Theme.selectionWidth : Theme.hairline
            border.color: card.chosen ? Theme.accentStroke : Theme.border
            FocusRing {
                targetRadius: Theme.radiusField
                visible: card.visualFocus
            }
        }
        HoverHandler {
            enabled: card.enabled
            cursorShape: Qt.PointingHandCursor
        }
        contentItem: ColumnLayout {
            id: cardColumn
            spacing: Theme.space(1)
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.space(26)
                LayoutSchematic {
                    readonly property real frameAspect: card.cardVertical ? 9 / 16 : Math.max(1, Layouts.frameAspect)
                    anchors.centerIn: parent
                    height: parent.height
                    width: Math.min(parent.width, height * frameAspect)
                    labels: false
                    boxes: {
                        const aspect = frameAspect
                        return Layouts.schematic(Object.assign({}, picker.options, { preset: card.modelData.preset }), aspect)
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                text: card.modelData.name
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                font.weight: Font.DemiBold
            }
        }
    }

    ColumnLayout {
        width: Theme.space(100)
        spacing: Theme.space(2)

        Text {
            objectName: "layoutGameplay"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: Layouts.gameplay !== "" ? qsTr("Gameplay: %1").arg(Layouts.gameplayName)
                                          : qsTr("Select the gameplay clip on the timeline (or put the playhead over it).")
            color: Layouts.gameplay !== "" ? Theme.textSecondary : Theme.warning
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        RowLayout {
            objectName: "layoutCards"
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Repeater {
                model: Layouts.layouts
                delegate: LayoutCard {}
            }
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: picker.current.description ?? ""
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        // The facecam: a clip above the gameplay, or a region of the capture.
        ColumnLayout {
            objectName: "layoutFacecam"
            Layout.fillWidth: true
            spacing: Theme.space(1)
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(2)
                FieldLabel {
                    text: qsTr("Facecam")
                }
                PillTabs {
                    id: sourceTabs
                    objectName: "layoutFacecamSource"
                    Layout.fillWidth: true
                    compact: true
                    Accessible.name: qsTr("Facecam source")
                    readonly property var sources: picker.preset === "verticalBlurFill" ? ["none", "clip", "region"] : ["clip", "region"]
                    /// The tab of the options' source ("auto": the clip when there is one, else the region).
                    readonly property int wanted: {
                        const s = picker.options.facecamSource ?? "auto"
                        if (s === "auto")
                            return sources.indexOf(Layouts.facecam !== "" || Layouts.facecamRegion.width === undefined ? "clip" : "region")
                        return Math.max(0, sources.indexOf(s))
                    }
                    model: picker.preset === "verticalBlurFill" ? [qsTr("None"), qsTr("Clip"), qsTr("Region")]
                                                                : [qsTr("Clip"), qsTr("Region")]
                    onActivated: index => picker.set("facecamSource", sources[index])
                    // A Binding, not a plain one: a click assigns currentIndex (PillTabs.select), which would end it.
                    Binding {
                        target: sourceTabs
                        property: "currentIndex"
                        value: sourceTabs.wanted
                    }
                }
            }
            // A clip above the gameplay.
            RowLayout {
                Layout.fillWidth: true
                visible: sourceTabs.sources[sourceTabs.currentIndex] === "clip"
                spacing: Theme.space(2)
                PillButton {
                    id: facecamClip
                    objectName: "layoutFacecamClip"
                    Layout.fillWidth: true
                    compact: true
                    iconName: "chevron-down"
                    enabled: Layouts.facecamCandidates.length > 0
                    text: {
                        const c = Layouts.facecamCandidates.find(f => f.key === Layouts.facecam)
                        return c ? qsTr("%1 · %2").arg(c.name).arg(c.track) : qsTr("No video clip above the gameplay")
                    }
                    tooltip: qsTr("The webcam clip on a video track above the gameplay")
                    onClicked: facecamMenu.popup(facecamClip, 0, facecamClip.height + Theme.space(1))
                    OptionsMenu {
                        id: facecamMenu
                        options: Layouts.facecamCandidates.map(c => ({ id: c.key, name: qsTr("%1 · %2").arg(c.name).arg(c.track) }))
                        current: Layouts.facecam
                        onChosen: id => {
                            Layouts.facecam = id
                            picker.set("facecamSource", "clip")
                        }
                    }
                }
            }
            // A region of the capture (the webcam overlay inside the gameplay).
            RowLayout {
                Layout.fillWidth: true
                visible: sourceTabs.sources[sourceTabs.currentIndex] === "region"
                spacing: Theme.space(2)
                PillButton {
                    objectName: "layoutDrawRegion"
                    compact: true
                    variant: Layouts.drawingRegion ? "primary" : "secondary"
                    iconName: "square"
                    text: Layouts.drawingRegion ? qsTr("Drawing on the monitor…") : qsTr("Draw on the monitor")
                    tooltip: qsTr("Drag a box around the webcam in the gameplay on the Program Monitor")
                    onClicked: {
                        Layouts.drawingRegion = !Layouts.drawingRegion
                        if (!Layouts.drawingRegion)
                            return
                        picker.set("facecamSource", "region")
                    }
                }
                FieldValue {
                    objectName: "layoutRegionValue"
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    text: Layouts.facecamRegion.width !== undefined
                          ? qsTr("%1 × %2 % of the frame").arg(Math.round(Layouts.facecamRegion.width * 100))
                                                         .arg(Math.round(Layouts.facecamRegion.height * 100))
                          : qsTr("No region yet")
                }
                IconButton {
                    objectName: "layoutClearRegion"
                    compact: true
                    visible: Layouts.facecamRegion.width !== undefined
                    iconName: "x"
                    tooltip: qsTr("Clear the region")
                    onClicked: {
                        Layouts.clearRegion()
                        Layouts.preview(picker.options)
                    }
                }
            }
        }

        // The facecam in a corner (picture-in-picture; the blur fill's facecam).
        GridLayout {
            objectName: "layoutCornerOptions"
            Layout.fillWidth: true
            visible: picker.showsCornerOptions
            columns: 2
            columnSpacing: Theme.space(2)
            rowSpacing: Theme.space(1)

            FieldLabel { text: qsTr("Corner") }
            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Theme.space(1)
                rowSpacing: Theme.space(1)
                Repeater {
                    model: [
                        { id: "topLeft", label: qsTr("Top left") },
                        { id: "topRight", label: qsTr("Top right") },
                        { id: "bottomLeft", label: qsTr("Bottom left") },
                        { id: "bottomRight", label: qsTr("Bottom right") }
                    ]
                    delegate: PillButton {
                        required property var modelData
                        objectName: "layoutCorner_" + modelData.id
                        Layout.fillWidth: true
                        compact: true
                        variant: (picker.options.corner ?? "bottomRight") === modelData.id ? "primary" : "secondary"
                        text: modelData.label
                        Accessible.role: Accessible.RadioButton
                        Accessible.checkable: true
                        Accessible.checked: (picker.options.corner ?? "bottomRight") === modelData.id
                        onClicked: picker.set("corner", modelData.id)
                    }
                }
            }

            FieldLabel { text: qsTr("Size") }
            RowLayout {
                Layout.fillWidth: true
                ValueSlider {
                    objectName: "layoutSize"
                    Layout.fillWidth: true
                    from: 0.05
                    to: 1
                    stepSize: 0.01
                    value: picker.options.size ?? 0.28
                    Accessible.name: qsTr("Facecam size")
                    onMoved: picker.set("size", value)
                }
                FieldValue { text: qsTr("%1 %").arg(Math.round((picker.options.size ?? 0.28) * 100)) }
            }
            FieldLabel { text: qsTr("Margin") }
            RowLayout {
                Layout.fillWidth: true
                ValueSlider {
                    objectName: "layoutMargin"
                    Layout.fillWidth: true
                    from: 0
                    to: 0.4
                    stepSize: 0.005
                    value: picker.options.margin ?? 0.03
                    Accessible.name: qsTr("Margin")
                    onMoved: picker.set("margin", value)
                }
                FieldValue { text: qsTr("%1 %").arg(((picker.options.margin ?? 0.03) * 100).toFixed(1)) }
            }
            FieldLabel { text: qsTr("Shape") }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(2)
                Toggle {
                    objectName: "layoutCircle"
                    text: qsTr("Circle")
                    checked: picker.options.circle === true
                    onToggled: picker.set("circle", checked)
                }
                ValueSlider {
                    objectName: "layoutCornerRadius"
                    Layout.fillWidth: true
                    enabled: picker.options.circle !== true
                    from: 0
                    to: 0.5
                    stepSize: 0.01
                    value: picker.options.cornerRadius ?? 0.12
                    Accessible.name: qsTr("Corner radius")
                    onMoved: picker.set("cornerRadius", value)
                }
            }
            FieldLabel { text: qsTr("Border") }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                ValueSlider {
                    objectName: "layoutBorderWidth"
                    Layout.fillWidth: true
                    from: 0
                    to: 24
                    stepSize: 1
                    value: picker.options.borderWidth ?? 4
                    Accessible.name: qsTr("Border width")
                    onMoved: picker.set("borderWidth", Math.round(value))
                }
                FieldValue { text: qsTr("%1 px").arg(Math.round(picker.options.borderWidth ?? 4)) }
                Repeater {
                    model: [Theme.contentWhite, Theme.contentInk].concat(Theme.clipColors.slice(0, 4))
                    delegate: Swatch {
                        required property var modelData
                        readonly property string hex: String(modelData).toUpperCase()
                        swatchColor: modelData
                        checkable: false
                        checked: hex === String(picker.options.borderColor ?? Theme.contentWhite).toUpperCase()
                        tooltip: qsTr("Border colour %1").arg(hex)
                        onClicked: picker.set("borderColor", hex)
                    }
                }
            }
            FieldLabel { text: qsTr("Shadow") }
            Toggle {
                objectName: "layoutShadow"
                text: qsTr("Drop shadow")
                checked: picker.options.shadow !== false
                onToggled: picker.set("shadow", checked)
            }
        }

        // Vertical split.
        GridLayout {
            objectName: "layoutSplitOptions"
            Layout.fillWidth: true
            visible: picker.preset === "verticalSplit"
            columns: 2
            columnSpacing: Theme.space(2)
            rowSpacing: Theme.space(1)
            FieldLabel { text: qsTr("Gameplay share") }
            RowLayout {
                Layout.fillWidth: true
                ValueSlider {
                    objectName: "layoutSplit"
                    Layout.fillWidth: true
                    from: 0.2
                    to: 0.8
                    stepSize: 0.01
                    value: picker.options.split ?? 0.6
                    Accessible.name: qsTr("Gameplay share of the height")
                    onMoved: picker.set("split", value)
                }
                FieldValue { text: qsTr("%1 %").arg(Math.round((picker.options.split ?? 0.6) * 100)) }
            }
            FieldLabel { text: qsTr("Facecam") }
            Toggle {
                objectName: "layoutFacecamOnTop"
                text: qsTr("Facecam on top")
                checked: picker.options.facecamOnTop === true
                onToggled: picker.set("facecamOnTop", checked)
            }
        }

        // Vertical blur fill.
        GridLayout {
            objectName: "layoutBlurOptions"
            Layout.fillWidth: true
            visible: picker.preset === "verticalBlurFill"
            columns: 2
            columnSpacing: Theme.space(2)
            rowSpacing: Theme.space(1)
            FieldLabel { text: qsTr("Background blur") }
            RowLayout {
                Layout.fillWidth: true
                ValueSlider {
                    objectName: "layoutBlur"
                    Layout.fillWidth: true
                    from: 0
                    to: 200
                    stepSize: 1
                    value: picker.options.blur ?? 40
                    Accessible.name: qsTr("Background blur")
                    onMoved: picker.set("blur", Math.round(value))
                }
                FieldValue { text: qsTr("%1 px").arg(Math.round(picker.options.blur ?? 40)) }
            }
            FieldLabel { text: qsTr("Zoom") }
            RowLayout {
                Layout.fillWidth: true
                ValueSlider {
                    objectName: "layoutZoom"
                    Layout.fillWidth: true
                    from: 1
                    to: 4
                    stepSize: 0.05
                    value: picker.options.zoom ?? 1
                    Accessible.name: qsTr("Gameplay zoom")
                    onMoved: picker.set("zoom", value)
                }
                FieldValue { text: qsTr("×%1").arg((picker.options.zoom ?? 1).toFixed(2)) }
            }
        }

        // What the dry run says, or why there is nothing to apply yet.
        Text {
            objectName: "layoutStatus"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: text !== ""
            text: picker.error !== "" ? picker.error
                  : !Layouts.available ? qsTr("Layouts arrive with the next engine update; the schematic shows what they will do.")
                  : picker.result.busy === true ? qsTr("Checking…")
                  : (picker.result.error ?? "")
            color: picker.error !== "" || (Layouts.available && picker.result.ok === false && (picker.result.error ?? "") !== "")
                   ? Theme.danger : Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: picker.vertical && !Layouts.sequenceIsVertical
            text: qsTr("This sequence is wide: a vertical layout belongs on a 9:16 sequence.")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            PillButton {
                objectName: "layoutCreateShorts"
                visible: picker.vertical && !Layouts.sequenceIsVertical && Layouts.sequencePresetsAvailable
                compact: true
                variant: "ghost"
                iconName: "plus"
                text: qsTr("Create a Shorts sequence")
                tooltip: qsTr("A new 1080 × 1920 sequence at 60 fps, shown next")
                onClicked: {
                    picker.error = Layouts.createShortsSequence()
                    if (picker.error === "")
                        picker.close()
                }
            }
            Item {
                Layout.fillWidth: true
            }
            PillButton {
                objectName: "layoutCancel"
                variant: "ghost"
                text: qsTr("Cancel")
                onClicked: picker.close()
            }
            PillButton {
                objectName: "layoutApply"
                variant: "primary"
                text: qsTr("Apply")
                enabled: Layouts.available && Layouts.gameplay !== "" && picker.result.busy !== true
                         && picker.result.ok !== false
                tooltip: qsTr("Apply the layout to the gameplay (one undo step)")
                onClicked: picker.applyLayout()
            }
        }
    }
}
