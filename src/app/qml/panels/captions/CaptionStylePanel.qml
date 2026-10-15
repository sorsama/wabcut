pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// ● CAPTION STYLE (M5m, the Captions workspace's side panel; C.7): the shown caption track's CaptionStyle, which the
// model stores and the program monitor draws live. Every control is one Captions.setStyle(key, value); a slider drag is
// one undo step. Presets: the five built in and the project's own (save the track's look as one, delete one).
Panel {
    id: root
    objectName: "captionStylePanel"

    readonly property var style: Captions.style
    readonly property bool hasTrack: Captions.track !== ""
    readonly property var fonts: [
        { id: "", name: qsTr("Default (sans-serif)") }, { id: "Segoe UI", name: "Segoe UI" }, { id: "Arial", name: "Arial" },
        { id: "Inter", name: "Inter" }, { id: "Georgia", name: "Georgia" }, { id: "Noto Sans Thai", name: "Noto Sans Thai" },
        { id: "Cascadia Mono", name: "Cascadia Mono" }
    ]
    readonly property var weights: [
        { id: "400", name: qsTr("Regular") }, { id: "500", name: qsTr("Medium") }, { id: "600", name: qsTr("Semibold") },
        { id: "700", name: qsTr("Bold") }, { id: "900", name: qsTr("Black") }
    ]
    readonly property var animations: [
        { id: "none", name: qsTr("None") }, { id: "pop", name: qsTr("Pop") }, { id: "fade", name: qsTr("Fade") },
        { id: "wordByWord", name: qsTr("Word by word") }
    ]

    function weightName(w) {
        for (let i = 0; i < weights.length; ++i) {
            if (Number(weights[i].id) === w)
                return weights[i].name
        }
        return String(w)
    }
    function fontName(f) {
        return f === "" || f === undefined ? qsTr("Default (sans-serif)") : f
    }
    // C.13: the brand kit's font roles, ahead of the families ("brand:<role>": the style follows the kit's font).
    readonly property var fontOptions: Brand.fontRoles.map(r => ({
        id: "brand:" + r.role,
        //: A brand kit font role in the caption font menu, e.g. "Brand: heading · Kanit"
        name: qsTr("Brand: %1").arg(r.label)
    })).concat(fonts)
    readonly property string brandFont: style.brandFont ?? ""
    function fontText() {
        if (brandFont !== "") {
            const role = Brand.fontRoles.find(r => r.role === brandFont)
            return qsTr("Brand: %1").arg(role ? role.label : brandFont)
        }
        return fontName(style.fontFamily)
    }

    title: qsTr("Caption Style")
    trailing: Toggle {
        objectName: "captionsOverlay"
        text: qsTr("On monitor")
        tooltip: qsTr("Draw the captions over the program monitor")
        checked: Captions.overlay
        onToggled: Captions.overlay = checked
    }

    component GroupLabel: Text {
        Layout.fillWidth: true
        Layout.topMargin: Theme.space(1)
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.DemiBold
        font.letterSpacing: Theme.labelTracking
    }
    component Hint: Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
    }
    // A pill that opens a menu of choices ([{ id, name }]).
    component ChoicePill: PillButton {
        id: pill
        property var options: []
        property string current: ""
        signal chosen(string id)
        compact: true
        iconName: "chevron-down"
        onClicked: menu.popup(pill, 0, pill.height + Theme.space(1))
        OptionsMenu {
            id: menu
            options: pill.options
            current: pill.current
            onChosen: id => pill.chosen(id)
        }
    }
    component Row2: RowLayout {
        id: row2
        property string label: ""
        default property alias content: slot.data
        Layout.fillWidth: true
        spacing: Theme.space(2)
        Text {
            Layout.fillWidth: true
            text: row2.label
            elide: Text.ElideRight
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        RowLayout {
            id: slot
            spacing: Theme.space(1)
        }
    }

    EmptyState {
        objectName: "captionStyleEmpty"
        anchors.centerIn: parent
        width: parent.width - Theme.space(8)
        visible: !root.hasTrack
        illustration: "transcript"
        text: qsTr("Generate captions or add a caption track in the Captions tab, then style it here.")
    }

    Flickable {
        id: flick
        anchors.fill: parent
        visible: root.hasTrack
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        ColumnLayout {
            id: column
            width: flick.width - Theme.space(3)
            spacing: Theme.space(2)

            // ---- The track ----
            GroupLabel { text: qsTr("Track") }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                FieldInput {
                    objectName: "captionTrackName"
                    Layout.fillWidth: true
                    text: Captions.trackInfo.name ?? ""
                    Accessible.name: qsTr("Caption track name")
                    onEditingFinished: {
                        if (text !== (Captions.trackInfo.name ?? ""))
                            Captions.setTrackName(text)
                    }
                }
                FieldInput {
                    objectName: "captionTrackLanguage"
                    Layout.preferredWidth: Theme.space(14)
                    text: Captions.trackInfo.language ?? ""
                    placeholderText: qsTr("en")
                    Accessible.name: qsTr("Caption language")
                    onEditingFinished: {
                        if (text !== (Captions.trackInfo.language ?? ""))
                            Captions.setTrackLanguage(text)
                    }
                }
                Toggle {
                    objectName: "captionTrackEnabled"
                    tooltip: qsTr("Burn in and export this track")
                    checked: Captions.trackInfo.enabled === true
                    onToggled: Captions.setTrackEnabled(checked)
                }
            }

            // ---- Presets ----
            GroupLabel { text: qsTr("Presets") }
            Flow {
                objectName: "captionPresets"
                Layout.fillWidth: true
                spacing: Theme.space(1.5)
                Repeater {
                    id: presetChips
                    model: Captions.presets
                    /// Applies a preset from the keyboard and gives the focus to its chip again (the chips are made
                    /// again when the current preset changes).
                    function applyFromKeys(name) {
                        Captions.applyPreset(name)
                        Qt.callLater(() => {
                            for (let i = 0; i < presetChips.count; ++i) {
                                const item = presetChips.itemAt(i)
                                if (item && item.objectName === "captionPreset_" + name)
                                    item.forceActiveFocus(Qt.TabFocusReason)
                            }
                        })
                    }
                    delegate: Item {
                        id: chip
                        required property var modelData
                        objectName: "captionPreset_" + modelData.name
                        implicitWidth: Theme.space(20)
                        implicitHeight: card.height + nameText.implicitHeight + Theme.space(1)
                        activeFocusOnTab: true
                        // The chip's keys are its own while it has focus (Space would otherwise play).
                        Keys.onShortcutOverride: event => event.accepted = (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier | Qt.AltModifier)) === 0
                                                                           && [Qt.Key_Space, Qt.Key_Return, Qt.Key_Enter, Qt.Key_Delete].includes(event.key)
                        Keys.onSpacePressed: presetChips.applyFromKeys(chip.modelData.name)
                        Keys.onReturnPressed: presetChips.applyFromKeys(chip.modelData.name)
                        Keys.onEnterPressed: presetChips.applyFromKeys(chip.modelData.name)
                        Keys.onDeletePressed: {
                            if (!chip.modelData.builtin)
                                Captions.removePreset(chip.modelData.name)
                        }
                        Rectangle {
                            id: card
                            width: parent.width
                            height: Theme.space(11)
                            radius: Theme.radiusField
                            color: Theme.videoBlack
                            border.width: chip.modelData.current ? Theme.focusRingWidth : Theme.hairline
                            border.color: chip.modelData.current ? Theme.accentStroke : chipHover.hovered ? Theme.borderStrong : Theme.border
                            Rectangle {
                                anchors.centerIn: parent
                                width: sample.implicitWidth + Theme.space(2)
                                height: sample.implicitHeight + Theme.space(1)
                                radius: Theme.radiusBadge
                                visible: chip.modelData.box
                                color: chip.modelData.boxColor
                                opacity: 0.85
                            }
                            Text {
                                id: sample
                                anchors.centerIn: parent
                                //: Sample letters showing a caption preset's look
                                text: qsTr("Aa")
                                color: chip.modelData.fill
                                style: chip.modelData.outlineWidth > 0 ? Text.Outline : Text.Normal
                                styleColor: Theme.contentInk
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                                font.weight: chip.modelData.fontWeight >= 700 ? Font.Bold : Font.DemiBold
                            }
                            IconButton {
                                anchors.top: parent.top
                                anchors.right: parent.right
                                visible: !chip.modelData.builtin && chipHover.hovered
                                compact: true
                                iconName: "x"
                                iconSize: Theme.iconXs
                                tooltip: qsTr("Delete the preset “%1”").arg(chip.modelData.name)
                                onClicked: Captions.removePreset(chip.modelData.name)
                            }
                            TapHandler {
                                onTapped: Captions.applyPreset(chip.modelData.name)
                            }
                            HoverHandler {
                                id: chipHover
                                cursorShape: Qt.PointingHandCursor
                            }
                            FocusRing {
                                targetRadius: Theme.radiusField
                                visible: chip.activeFocus
                            }
                        }
                        Text {
                            id: nameText
                            anchors.top: card.bottom
                            anchors.topMargin: Theme.space(1)
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            text: chip.modelData.name
                            elide: Text.ElideRight
                            color: chip.modelData.current ? Theme.accentStroke : Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                        }
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: qsTr("Caption preset %1").arg(modelData.name)
                        Accessible.checked: modelData.current === true
                        Accessible.description: modelData.builtin ? qsTr("Space applies it")
                                                                  : qsTr("Space applies it, Delete removes it")
                        Accessible.onPressAction: Captions.applyPreset(chip.modelData.name)
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                FieldInput {
                    id: presetName
                    objectName: "captionPresetName"
                    Layout.fillWidth: true
                    Accessible.name: qsTr("New preset name")
                    placeholderText: qsTr("Name this look…")
                    onAccepted: saveButton.clicked()
                }
                PillButton {
                    id: saveButton
                    objectName: "captionSavePreset"
                    compact: true
                    iconName: "save"
                    text: qsTr("Save preset")
                    enabled: presetName.text.trim() !== ""
                    onClicked: {
                        if (Captions.savePreset(presetName.text) === "")
                            presetName.text = ""
                    }
                }
            }

            // ---- Text ----
            GroupLabel { text: qsTr("Text") }
            Row2 {
                label: qsTr("Font")
                ChoicePill {
                    objectName: "captionFont"
                    text: root.fontText()
                    options: root.fontOptions
                    current: root.brandFont !== "" ? "brand:" + root.brandFont : (root.style.fontFamily ?? "")
                    onChosen: id => {
                        if (id.startsWith("brand:"))
                            Captions.setStyle("brandFont", id.slice(6))
                        else
                            Captions.setStyle("fontFamily", id)
                    }
                }
                ChoicePill {
                    objectName: "captionWeight"
                    text: root.weightName(root.style.fontWeight)
                    options: root.weights
                    current: String(root.style.fontWeight)
                    onChosen: id => Captions.setStyle("fontWeight", Number(id))
                }
            }
            PropSlider {
                objectName: "captionSize"
                Layout.fillWidth: true
                label: qsTr("Size")
                unit: qsTr("px")
                from: 12
                to: 160
                value: root.style.fontSizePx ?? 48
                defaultValue: 48
                onValueModified: v => Captions.setStyle("fontSizePx", v)
                onGestureEnded: Captions.endStyleEdit()
            }
            PropSlider {
                objectName: "captionLineSpacing"
                Layout.fillWidth: true
                label: qsTr("Line spacing")
                from: 0.5
                to: 3
                stepSize: 0.05
                decimals: 2
                value: root.style.lineSpacing ?? 1
                defaultValue: 1
                onValueModified: v => Captions.setStyle("lineSpacing", v)
                onGestureEnded: Captions.endStyleEdit()
            }
            PropSlider {
                objectName: "captionMaxWidth"
                Layout.fillWidth: true
                label: qsTr("Max width")
                unit: "%"
                from: 10
                to: 100
                value: (root.style.maxWidth ?? 0.8) * 100
                defaultValue: 80
                onValueModified: v => Captions.setStyle("maxWidth", v / 100)
                onGestureEnded: Captions.endStyleEdit()
            }
            Row2 {
                label: qsTr("Alignment")
                Repeater {
                    model: [{ id: "left", icon: "align-left", tip: qsTr("Align left") },
                            { id: "center", icon: "align-center", tip: qsTr("Centre") },
                            { id: "right", icon: "align-right", tip: qsTr("Align right") }]
                    delegate: IconButton {
                        required property var modelData
                        objectName: "captionAlign_" + modelData.id
                        compact: true
                        checkable: true
                        checked: root.style.alignment === modelData.id
                        iconName: modelData.icon
                        tooltip: modelData.tip
                        onClicked: Captions.setStyle("alignment", modelData.id)
                    }
                }
            }
            CaptionColorRow {
                Layout.fillWidth: true
                key: "fill"
                label: qsTr("Fill")
                value: root.style.fill ?? Theme.contentWhite
                onPicked: c => Captions.setStyle("fill", c)
                brandRef: root.style.brandFill ?? ""
                onPickedBrand: name => Captions.setStyle("brandFill", name)
            }

            // ---- Outline ----
            GroupLabel { text: qsTr("Outline") }
            PropSlider {
                objectName: "captionOutlineWidth"
                Layout.fillWidth: true
                label: qsTr("Width")
                unit: qsTr("% of size")
                from: 0
                to: 50
                value: (root.style.outlineWidth ?? 0) * 100
                defaultValue: 0
                onValueModified: v => Captions.setStyle("outlineWidth", v / 100)
                onGestureEnded: Captions.endStyleEdit()
            }
            CaptionColorRow {
                Layout.fillWidth: true
                visible: (root.style.outlineWidth ?? 0) > 0
                key: "outline"
                label: qsTr("Outline colour")
                value: root.style.outline ?? String(Theme.videoBlack)
                onPicked: c => Captions.setStyle("outline", c)
                brandRef: root.style.brandOutline ?? ""
                onPickedBrand: name => Captions.setStyle("brandOutline", name)
            }

            // ---- Shadow ----
            GroupLabel { text: qsTr("Shadow") }
            PropSlider {
                objectName: "captionShadowOpacity"
                Layout.fillWidth: true
                label: qsTr("Opacity")
                unit: "%"
                from: 0
                to: 100
                value: (root.style.shadowOpacity ?? 0) * 100
                defaultValue: 0
                onValueModified: v => Captions.setStyle("shadowOpacity", v / 100)
                onGestureEnded: Captions.endStyleEdit()
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: (root.style.shadowOpacity ?? 0) > 0
                spacing: Theme.space(2)
                PropSlider {
                    Layout.fillWidth: true
                    label: qsTr("Distance")
                    unit: qsTr("% of size")
                    from: 0
                    to: 100
                    value: (root.style.shadowDistance ?? 0.08) * 100
                    defaultValue: 8
                    onValueModified: v => Captions.setStyle("shadowDistance", v / 100)
                    onGestureEnded: Captions.endStyleEdit()
                }
                PropSlider {
                    Layout.fillWidth: true
                    label: qsTr("Angle")
                    unit: "°"
                    from: -180
                    to: 180
                    value: root.style.shadowAngle ?? 135
                    defaultValue: 135
                    onValueModified: v => Captions.setStyle("shadowAngle", v)
                    onGestureEnded: Captions.endStyleEdit()
                }
                PropSlider {
                    Layout.fillWidth: true
                    label: qsTr("Softness")
                    unit: qsTr("% of size")
                    from: 0
                    to: 100
                    value: (root.style.shadowSoftness ?? 0.1) * 100
                    defaultValue: 10
                    onValueModified: v => Captions.setStyle("shadowSoftness", v / 100)
                    onGestureEnded: Captions.endStyleEdit()
                }
                CaptionColorRow {
                    Layout.fillWidth: true
                    key: "shadowColor"
                    label: qsTr("Shadow colour")
                    value: root.style.shadowColor ?? String(Theme.videoBlack)
                    onPicked: c => Captions.setStyle("shadowColor", c)
                    brandRef: root.style.brandShadow ?? ""
                    onPickedBrand: name => Captions.setStyle("brandShadow", name)
                }
            }

            // ---- Background box ----
            RowLayout {
                Layout.fillWidth: true
                GroupLabel { text: qsTr("Background box") }
                Toggle {
                    objectName: "captionBox"
                    tooltip: qsTr("A box behind the text")
                    checked: root.style.box === true
                    onToggled: Captions.setStyle("box", checked)
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.style.box === true
                spacing: Theme.space(2)
                CaptionColorRow {
                    Layout.fillWidth: true
                    key: "boxColor"
                    label: qsTr("Box colour")
                    value: root.style.boxColor ?? Theme.contentInk
                    onPicked: c => Captions.setStyle("boxColor", c)
                    brandRef: root.style.brandBox ?? ""
                    onPickedBrand: name => Captions.setStyle("brandBox", name)
                }
                PropSlider {
                    objectName: "captionBoxOpacity"
                    Layout.fillWidth: true
                    label: qsTr("Opacity")
                    unit: "%"
                    from: 0
                    to: 100
                    value: (root.style.boxOpacity ?? 0.78) * 100
                    defaultValue: 78
                    onValueModified: v => Captions.setStyle("boxOpacity", v / 100)
                    onGestureEnded: Captions.endStyleEdit()
                }
                PropSlider {
                    Layout.fillWidth: true
                    label: qsTr("Padding")
                    unit: qsTr("% of size")
                    from: 0
                    to: 100
                    value: (root.style.boxPadding ?? 0.35) * 100
                    defaultValue: 35
                    onValueModified: v => Captions.setStyle("boxPadding", v / 100)
                    onGestureEnded: Captions.endStyleEdit()
                }
                PropSlider {
                    Layout.fillWidth: true
                    label: qsTr("Corner radius")
                    unit: qsTr("% of size")
                    from: 0
                    to: 100
                    value: (root.style.boxRadius ?? 0.18) * 100
                    defaultValue: 18
                    onValueModified: v => Captions.setStyle("boxRadius", v / 100)
                    onGestureEnded: Captions.endStyleEdit()
                }
            }

            // ---- Position ----
            GroupLabel { text: qsTr("Position") }
            PillTabs {
                id: placementTabs
                objectName: "captionPlacement"
                Layout.fillWidth: true
                compact: true
                readonly property var ids: ["bottom", "top", "custom"]
                model: [qsTr("Bottom"), qsTr("Top"), qsTr("Custom")]
                currentIndex: Math.max(0, placementTabs.ids.indexOf(root.style.placement))
                onActivated: index => Captions.setStyle("placement", placementTabs.ids[index])
                Accessible.name: qsTr("Caption position")
            }
            PropSlider {
                objectName: "captionSafeArea"
                Layout.fillWidth: true
                label: qsTr("Safe area")
                unit: "%"
                from: 0
                to: 40
                value: (root.style.safeArea ?? 0.1) * 100
                defaultValue: 10
                onValueModified: v => Captions.setStyle("safeArea", v / 100)
                onGestureEnded: Captions.endStyleEdit()
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.style.placement === "custom"
                spacing: Theme.space(2)
                PropSlider {
                    objectName: "captionPositionX"
                    Layout.fillWidth: true
                    label: qsTr("Centre X")
                    unit: "%"
                    from: 0
                    to: 100
                    value: (root.style.positionX ?? 0.5) * 100
                    defaultValue: 50
                    onValueModified: v => Captions.setStyle("positionX", v / 100)
                    onGestureEnded: Captions.endStyleEdit()
                }
                PropSlider {
                    objectName: "captionPositionY"
                    Layout.fillWidth: true
                    label: qsTr("Centre Y")
                    unit: "%"
                    from: 0
                    to: 100
                    value: (root.style.positionY ?? 0.85) * 100
                    defaultValue: 85
                    onValueModified: v => Captions.setStyle("positionY", v / 100)
                    onGestureEnded: Captions.endStyleEdit()
                }
            }

            // ---- Karaoke, speakers, animation ----
            GroupLabel { text: qsTr("Karaoke and speakers") }
            CaptionColorRow {
                Layout.fillWidth: true
                key: "highlight"
                label: qsTr("Spoken words")
                value: root.style.highlight ?? Theme.captionHighlight
                onPicked: c => Captions.setStyle("highlight", c)
                brandRef: root.style.brandHighlight ?? ""
                onPickedBrand: name => Captions.setStyle("brandHighlight", name)
            }
            Row2 {
                label: qsTr("Speaker names")
                Toggle {
                    objectName: "captionShowSpeakers"
                    tooltip: qsTr("Start each caption with its speaker's name in the speaker's colour")
                    checked: root.style.showSpeakers === true
                    onToggled: Captions.setStyle("showSpeakers", checked)
                }
            }
            Row2 {
                label: qsTr("Overlapping captions")
                Toggle {
                    objectName: "captionAllowOverlap"
                    tooltip: qsTr("Captions of this track may overlap (stacked)")
                    checked: root.style.allowOverlap === true
                    onToggled: Captions.setStyle("allowOverlap", checked)
                }
            }
            Row2 {
                label: qsTr("Animation")
                ChoicePill {
                    objectName: "captionAnimation"
                    text: {
                        for (let i = 0; i < root.animations.length; ++i) {
                            if (root.animations[i].id === root.style.animation)
                                return root.animations[i].name
                        }
                        return qsTr("None")
                    }
                    options: root.animations
                    current: root.style.animation ?? "none"
                    onChosen: id => Captions.setStyle("animation", id)
                }
            }
            Hint {
                text: qsTr("Saved with the track: the program monitor and burned-in exports draw it the same way.")
            }
            Text {
                Layout.fillWidth: true
                visible: text !== ""
                text: Captions.message
                wrapMode: Text.Wrap
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
            Item { Layout.preferredHeight: Theme.space(2) }
        }
    }
}
