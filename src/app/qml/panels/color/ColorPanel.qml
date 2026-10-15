pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// ● COLOR (Color workspace side panel, I.5, docs/color.md): the grade of the targeted clip - the first selected video
// clip, else the clip under the playhead - in the grade's order: Basic, Creative, Curves, Color Wheels,
// HSL Secondary, Vignette, then Compare & Match. Every change is a typed edit (one undo step per drag) and every slider
// is a keyframable parameter (an animated one gets a key at the playhead; Effect Controls shows its keyframes).
Panel {
    id: root
    objectName: "colorPanel"

    title: qsTr("Color")

    property string curveName: "master"
    property string hueCurveName: "hueSat"
    property real matchStrength: 1.0
    property bool skinPriority: false
    property string lutSlot: "look"

    trailing: [
        Toggle {
            objectName: "gradeEnabledToggle"
            visible: Color.hasGrade
            checked: Color.gradeEnabled
            tooltip: Color.gradeEnabled ? qsTr("Turn the grade off") : qsTr("Turn the grade on")
            onToggled: Color.setGradeEnabled(checked)
        }
    ]

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(2)

        // The clip and Auto.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)

            Icon {
                name: "film"
                size: Theme.iconSm
                color: Color.clip !== "" ? Theme.accentStroke : Theme.textTertiary
            }
            Text {
                Layout.fillWidth: true
                text: Color.clip !== "" ? Color.clipName : qsTr("Select a clip or put the playhead over one")
                color: Color.clip !== "" ? Theme.textPrimary : Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                elide: Text.ElideRight
            }
            PillButton {
                objectName: "autoButton"
                text: qsTr("Auto")
                iconName: "sparkles"
                compact: true
                enabled: Color.clip !== "" && !Color.busy
                tooltip: qsTr("White balance and levels from the clip itself (editable Basic settings)")
                onClicked: Color.autoColor()
            }
        }

        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: sections.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height
            ScrollBar.vertical: ThinScrollBar {}
            enabled: Color.clip !== ""
            opacity: enabled ? 1 : Theme.disabledOpacity

            ColumnLayout {
                id: sections
                width: flick.width - Theme.space(2)
                spacing: Theme.space(1)

                GradeSection {
                    title: qsTr("Basic Correction")
                    stage: "basic"

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            Layout.preferredWidth: Theme.propLabelWidth
                            text: qsTr("Input LUT")
                            color: Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                        }
                        PillButton {
                            id: inputLutButton
                            objectName: "inputLutButton"
                            Layout.fillWidth: true
                            compact: true
                            text: Color.inputLut === "" ? qsTr("None") : root.lutName(Color.inputLut)
                            iconName: "chevron-down"
                            onClicked: {
                                lutMenu.slot = "input"
                                lutMenu.openAt(inputLutButton)
                            }
                        }
                    }
                    Text {
                        text: qsTr("White balance")
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                        font.letterSpacing: Theme.labelTracking
                        Layout.topMargin: Theme.space(1)
                    }
                    GradeSlider { Layout.fillWidth: true; key: "basic.temperature" }
                    GradeSlider { Layout.fillWidth: true; key: "basic.tint" }
                    Text {
                        text: qsTr("Tone")
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                        font.letterSpacing: Theme.labelTracking
                        Layout.topMargin: Theme.space(1)
                    }
                    GradeSlider { Layout.fillWidth: true; key: "basic.exposure"; objectName: "exposureSlider" }
                    GradeSlider { Layout.fillWidth: true; key: "basic.contrast" }
                    GradeSlider { Layout.fillWidth: true; key: "basic.highlights" }
                    GradeSlider { Layout.fillWidth: true; key: "basic.shadows" }
                    GradeSlider { Layout.fillWidth: true; key: "basic.whites" }
                    GradeSlider { Layout.fillWidth: true; key: "basic.blacks" }
                    GradeSlider { Layout.fillWidth: true; key: "basic.saturation" }
                    GradeSlider { Layout.fillWidth: true; key: "basic.vibrance" }
                }

                GradeSection {
                    title: qsTr("Creative")
                    stage: "creative"
                    expanded: false

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            Layout.preferredWidth: Theme.propLabelWidth
                            text: qsTr("Look")
                            color: Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                        }
                        PillButton {
                            id: lookButton
                            objectName: "lookButton"
                            Layout.fillWidth: true
                            compact: true
                            text: Color.look === "" ? qsTr("None") : root.lutName(Color.look)
                            iconName: "chevron-down"
                            onClicked: {
                                lutMenu.slot = "look"
                                lutMenu.openAt(lookButton)
                            }
                        }
                    }
                    GradeSlider { Layout.fillWidth: true; key: "creative.lookIntensity"; title: qsTr("Intensity") }
                    GradeSlider { Layout.fillWidth: true; key: "creative.fadedFilm" }
                    GradeSlider { Layout.fillWidth: true; key: "creative.vibrance"; title: qsTr("Vibrance") }
                    GradeSlider { Layout.fillWidth: true; key: "creative.saturation"; title: qsTr("Saturation") }
                    GradeSlider { Layout.fillWidth: true; key: "creative.shadowTint.hue" }
                    GradeSlider { Layout.fillWidth: true; key: "creative.shadowTint.amount" }
                    GradeSlider { Layout.fillWidth: true; key: "creative.highlightTint.hue" }
                    GradeSlider { Layout.fillWidth: true; key: "creative.highlightTint.amount" }
                    GradeSlider { Layout.fillWidth: true; key: "creative.tintBalance" }
                }

                GradeSection {
                    title: qsTr("Curves")
                    stage: "curves"
                    expanded: false

                    PillTabs {
                        Layout.fillWidth: true
                        compact: true
                        model: [qsTr("Master"), qsTr("Red"), qsTr("Green"), qsTr("Blue")]
                        currentIndex: ["master", "red", "green", "blue"].indexOf(root.curveName)
                        onActivated: index => root.curveName = ["master", "red", "green", "blue"][index]
                    }
                    CurveEditor {
                        objectName: "rgbCurve"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 200
                        name: root.curveName
                        kind: "rgb"
                        lineColor: root.curveName === "red" ? Theme.danger : root.curveName === "green" ? Theme.success
                                 : root.curveName === "blue" ? Theme.info : Theme.textPrimary
                    }
                    PillTabs {
                        Layout.fillWidth: true
                        Layout.topMargin: Theme.space(2)
                        compact: true
                        model: [qsTr("Hue/Sat"), qsTr("Hue/Hue"), qsTr("Hue/Luma"), qsTr("Luma/Sat"), qsTr("Sat/Sat")]
                        currentIndex: ["hueSat", "hueHue", "hueLuma", "lumaSat", "satSat"].indexOf(root.hueCurveName)
                        onActivated: index => root.hueCurveName = ["hueSat", "hueHue", "hueLuma", "lumaSat", "satSat"][index]
                    }
                    CurveEditor {
                        objectName: "hueCurve"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 150
                        name: root.hueCurveName
                        kind: root.hueCurveName.startsWith("hue") ? "hue" : "neutral"
                        lineColor: Theme.accentStroke
                    }
                }

                GradeSection {
                    title: qsTr("Color Wheels")
                    stage: "wheels"
                    expanded: false

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        rowSpacing: Theme.space(3)
                        columnSpacing: Theme.space(2)
                        ColorWheel {
                            objectName: "liftWheel"
                            Layout.alignment: Qt.AlignHCenter
                            title: qsTr("Lift")
                            prefix: "wheels.lift"
                            neutral: 0
                            range: 0.2
                            lumaFrom: -0.5
                            lumaTo: 0.5
                        }
                        ColorWheel {
                            Layout.alignment: Qt.AlignHCenter
                            title: qsTr("Gamma")
                            prefix: "wheels.gamma"
                            neutral: 1
                            range: 0.4
                            lumaFrom: 0.25
                            lumaTo: 3
                        }
                        ColorWheel {
                            Layout.alignment: Qt.AlignHCenter
                            title: qsTr("Gain")
                            prefix: "wheels.gain"
                            neutral: 1
                            range: 0.4
                            lumaFrom: 0
                            lumaTo: 3
                        }
                        ColorWheel {
                            Layout.alignment: Qt.AlignHCenter
                            title: qsTr("Offset")
                            prefix: "wheels.offset"
                            neutral: 0
                            range: 0.2
                            lumaFrom: -0.5
                            lumaTo: 0.5
                        }
                    }
                    GradeSlider { Layout.fillWidth: true; key: "wheels.saturation"; title: qsTr("Saturation") }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(2)
                        PillButton {
                            text: qsTr("Import CDL…")
                            compact: true
                            iconName: "import"
                            onClicked: cdlImportDialog.open()
                        }
                        PillButton {
                            text: qsTr("Export CDL…")
                            compact: true
                            iconName: "upload"
                            onClicked: cdlExportDialog.open()
                        }
                    }
                }

                GradeSection {
                    title: qsTr("HSL Secondary")
                    stage: "hsl"
                    expanded: false

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(2)
                        PillButton {
                            objectName: "eyedropperButton"
                            text: Color.eyedropper ? qsTr("Click the picture…") : qsTr("Pick colour")
                            iconName: "crosshair"
                            compact: true
                            variant: Color.eyedropper ? "primary" : "secondary"
                            tooltip: qsTr("Click a colour on the program monitor to qualify it")
                            onClicked: Color.eyedropper = !Color.eyedropper
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: qsTr("Invert")
                            color: Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                        }
                        Toggle {
                            checked: (Color.params["hsl.invert"] ?? 0) > 0.5
                            onToggled: Color.setParam("hsl.invert", checked ? 1 : 0)
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            Layout.preferredWidth: Theme.propLabelWidth
                            text: qsTr("Show mask")
                            color: Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                        }
                        PillTabs {
                            objectName: "maskViewTabs"
                            Layout.fillWidth: true
                            compact: true
                            model: [qsTr("Off"), qsTr("Grey"), qsTr("Black"), qsTr("White")]
                            currentIndex: Color.maskView
                            onActivated: index => Color.maskView = index
                        }
                    }
                    GradeSlider { Layout.fillWidth: true; key: "hsl.hue.center"; objectName: "hueCenterSlider" }
                    GradeSlider { Layout.fillWidth: true; key: "hsl.hue.width" }
                    GradeSlider { Layout.fillWidth: true; key: "hsl.hue.softness" }
                    GradeSlider { Layout.fillWidth: true; key: "hsl.sat.low" }
                    GradeSlider { Layout.fillWidth: true; key: "hsl.sat.high" }
                    GradeSlider { Layout.fillWidth: true; key: "hsl.sat.softness" }
                    GradeSlider { Layout.fillWidth: true; key: "hsl.luma.low" }
                    GradeSlider { Layout.fillWidth: true; key: "hsl.luma.high" }
                    GradeSlider { Layout.fillWidth: true; key: "hsl.luma.softness" }
                    GradeSlider { Layout.fillWidth: true; key: "hsl.denoise" }
                    GradeSlider { Layout.fillWidth: true; key: "hsl.blur" }
                    GradeSlider { Layout.fillWidth: true; key: "hsl.temperature"; title: qsTr("Temperature") }
                    GradeSlider { Layout.fillWidth: true; key: "hsl.tint"; title: qsTr("Tint") }
                    GradeSlider { Layout.fillWidth: true; key: "hsl.contrast"; title: qsTr("Contrast") }
                    GradeSlider { Layout.fillWidth: true; key: "hsl.sharpen"; title: qsTr("Sharpen") }
                    GradeSlider { Layout.fillWidth: true; key: "hsl.saturation"; title: qsTr("Saturation") }
                    ColorWheel {
                        Layout.alignment: Qt.AlignHCenter
                        title: qsTr("Correction")
                        prefix: "hsl.offset"
                        neutral: 0
                        range: 0.2
                        lumaFrom: -0.5
                        lumaTo: 0.5
                        size: 92
                    }
                }

                GradeSection {
                    title: qsTr("Vignette")
                    stage: "vignette"
                    expanded: false

                    GradeSlider { Layout.fillWidth: true; key: "vignette.amount"; title: qsTr("Amount") }
                    GradeSlider { Layout.fillWidth: true; key: "vignette.midpoint"; title: qsTr("Midpoint") }
                    GradeSlider { Layout.fillWidth: true; key: "vignette.roundness"; title: qsTr("Roundness") }
                    GradeSlider { Layout.fillWidth: true; key: "vignette.feather"; title: qsTr("Feather") }
                }

                GradeSection {
                    title: qsTr("Compare & Match")

                    PillTabs {
                        objectName: "compareTabs"
                        Layout.fillWidth: true
                        compact: true
                        model: [qsTr("Off"), qsTr("Before / After"), qsTr("Reference")]
                        currentIndex: Color.compareMode
                        onActivated: index => Color.compareMode = index
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(2)
                        PillButton {
                            text: Color.hasReference ? qsTr("Reference %1").arg(Color.referenceLabel) : qsTr("Set reference")
                            iconName: "bookmark"
                            compact: true
                            tooltip: qsTr("The program at the playhead becomes the reference frame")
                            onClicked: Color.setReference()
                        }
                        IconButton {
                            visible: Color.hasReference
                            compact: true
                            iconName: "x"
                            tooltip: qsTr("Clear the reference")
                            onClicked: Color.clearReference()
                        }
                    }
                    PropSlider {
                        Layout.fillWidth: true
                        label: qsTr("Strength")
                        unit: " %"
                        from: 0
                        to: 100
                        defaultValue: 100
                        value: root.matchStrength * 100
                        onValueModified: v => root.matchStrength = v / 100
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Skin tone priority")
                            color: Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                        }
                        Toggle {
                            checked: root.skinPriority
                            onToggled: root.skinPriority = checked
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(2)
                        PillButton {
                            objectName: "matchButton"
                            text: qsTr("Match")
                            iconName: "blend"
                            variant: "primary"
                            compact: true
                            enabled: Color.hasReference && !Color.busy
                            tooltip: qsTr("Make this clip look like the reference frame")
                            onClicked: Color.matchToReference(root.matchStrength, root.skinPriority)
                        }
                        PillButton {
                            text: qsTr("Match scene")
                            iconName: "film"
                            compact: true
                            enabled: !Color.busy
                            tooltip: qsTr("Match the other shots of this take (split at its shots) to this clip")
                            onClicked: Color.matchScene(root.matchStrength, root.skinPriority)
                        }
                    }
                }

                Text {
                    objectName: "colorStatus"
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.space(1)
                    visible: text !== ""
                    text: Color.missingLuts > 0 ? qsTr("%n LUT(s) of this grade are missing (relink them)", "", Color.missingLuts)
                                                : Color.status
                    color: Color.missingLuts > 0 ? Theme.warning : Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    wrapMode: Text.WordWrap
                }
            }
        }
    }

    function lutName(reference) {
        const choice = Color.lutChoices.find(c => c.reference === reference)
        return choice ? choice.name : qsTr("Missing LUT")
    }

    LutMenu {
        id: lutMenu
        onBrowse: lutDialog.open()
    }

    FileDialog {
        id: lutDialog
        title: qsTr("Import a LUT")
        nameFilters: [qsTr("Cube LUTs (*.cube)")]
        onAccepted: {
            const error = Color.importLut(selectedFile, lutMenu.slot)
            if (error !== "")
                console.warn(error)
        }
    }
    FileDialog {
        id: cdlImportDialog
        title: qsTr("Import an ASC CDL")
        nameFilters: [qsTr("ASC CDL (*.cc *.ccc *.cdl)")]
        onAccepted: Color.importCdl(selectedFile)
    }
    FileDialog {
        id: cdlExportDialog
        title: qsTr("Export the colour wheels as an ASC CDL")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "cc"
        nameFilters: [qsTr("ASC CDL (*.cc)"), qsTr("ASC CDL collection (*.ccc)"), qsTr("Color decision list (*.cdl)")]
        onAccepted: Color.exportCdl(selectedFile)
    }
}
