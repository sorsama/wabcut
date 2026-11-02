pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Color Management (0.2, I.9; Premiere Pro's Sequence Settings > Color and the Lumetri Settings tab): the project's
// Color Setup - Direct Rec.709 (SDR, the default: HDR and log media tone-mapped into SDR on input), Direct HLG / PQ
// (HDR), Wide Gamut (Tone Mapped: ACES 1.3), Wide Gamut (Minimal Tone Mapping, 0.6) and Wide Gamut (No Tone Mapping), or
// Disable Color Management - with the HDR peak and graphics white, and whether log media is read as the camera log its
// metadata names. 0.6 (#72, HDR finishing): gamut compression of camera log and HDR media (ACES 1.3), the output colour
// space exports write, and SDR Conform (how an HDR or Wide Gamut sequence becomes an SDR export). OK applies all of it as
// one undo step (color.management.set). The settings scroll when the window is short.
WabcutDialog {
    id: dialog
    objectName: "colorManagementDialog"

    property string mode: "rec709"
    property real peakNits: 1000
    property real graphicsWhite: 203
    property bool autoDetectLog: true
    // 0.6 (#72)
    property bool gamutCompression: false
    property real gamutStrength: 1
    property string outputSpace: "auto"
    property bool conformEnabled: false
    property real conformPeak: 100
    property real conformPaperWhite: 100
    property string conformOperator: "bt2390"
    property string conformGamut: "aces-rgc"
    property bool conformLegal: false
    readonly property bool hdr: mode === "hdr-pq" || mode === "hdr-hlg"
    readonly property bool minimal: mode === "wide-gamut-minimal"
    readonly property bool managedMode: mode !== "rec709" && mode !== "off"
    readonly property bool conformable: hdr || mode === "wide-gamut" || minimal
    readonly property var operatorIds: ["bt2390", "aces", "reinhard"]
    readonly property var gamutIds: ["aces-rgc", "saturation", "clip"]
    readonly property string outputSpaceName: {
        const spaces = Color.outputColorSpaces
        for (let i = 0; i < spaces.length; ++i) {
            if (spaces[i].id === dialog.outputSpace)
                return spaces[i].name
        }
        return qsTr("Automatic")
    }

    function openFromProject() {
        const cm = Color.colorManagement
        mode = cm.mode ?? "rec709"
        peakNits = cm.peakNits ?? 1000
        graphicsWhite = cm.referenceWhiteNits ?? 203
        autoDetectLog = cm.autoDetectLog ?? true
        gamutCompression = cm.gamutCompression ?? false
        gamutStrength = cm.gamutCompressionStrength ?? 1
        outputSpace = cm.outputSpace ?? "auto"
        const conform = cm.sdrConform ?? {}
        conformEnabled = conform.enabled ?? false
        conformPeak = conform.targetPeakNits ?? 100
        conformPaperWhite = conform.paperWhiteNits ?? 100
        conformOperator = conform.toneMap ?? "bt2390"
        conformGamut = conform.gamut ?? "aces-rgc"
        conformLegal = conform.legalRange ?? false
        open()
    }
    function apply() {
        if (Color.setColorManagement({ mode: dialog.mode, peakNits: dialog.peakNits,
                                       referenceWhiteNits: dialog.graphicsWhite, autoDetectLog: dialog.autoDetectLog,
                                       gamutCompression: dialog.gamutCompression,
                                       gamutCompressionStrength: dialog.gamutStrength, outputSpace: dialog.outputSpace,
                                       sdrConform: { enabled: dialog.conformEnabled, targetPeakNits: dialog.conformPeak,
                                                     paperWhiteNits: dialog.conformPaperWhite,
                                                     toneMap: dialog.conformOperator, gamut: dialog.conformGamut,
                                                     legalRange: dialog.conformLegal } }))
            close()
    }

    title: qsTr("Color Management")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(150), Theme.space(150))

    ScrollView {
        id: scroller
        objectName: "colorManagementScroller"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(settings.implicitHeight,
                                         dialog.parent ? dialog.parent.height - Theme.space(60) : settings.implicitHeight)
        contentWidth: availableWidth
        clip: true
        ScrollBar.vertical: ThinScrollBar {}

        ColumnLayout {
            id: settings
            width: scroller.availableWidth
            spacing: Theme.space(3)

            Text {
                text: qsTr("Color setup")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
                font.letterSpacing: Theme.labelTracking
            }

            // One row per preset: its name and what it does; the chosen one is outlined in the accent.
            Repeater {
                model: Color.colorManagementModes
                delegate: Rectangle {
                    id: choice
                    required property var modelData
                    readonly property bool chosen: dialog.mode === modelData.id
                    objectName: "colorSetup_" + modelData.id
                    Layout.fillWidth: true
                    implicitHeight: choiceColumn.implicitHeight + Theme.space(3)
                    radius: Theme.radiusField
                    color: choiceArea.containsMouse ? Theme.hover : chosen ? Theme.sunken : "transparent"
                    border.width: chosen ? Theme.hairline * 2 : Theme.hairline
                    border.color: chosen ? Theme.accentStroke : Theme.border
                    activeFocusOnTab: true
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: modelData.name
                    Accessible.checked: chosen
                    Keys.onSpacePressed: dialog.mode = modelData.id

                    ColumnLayout {
                        id: choiceColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: Theme.space(3)
                        anchors.rightMargin: Theme.space(3)
                        spacing: Theme.space(0.5)
                        Text {
                            Layout.fillWidth: true
                            text: choice.modelData.name
                            color: choice.chosen ? Theme.textPrimary : Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: choice.chosen ? Font.DemiBold : Font.Normal
                        }
                        Text {
                            Layout.fillWidth: true
                            text: choice.modelData.description
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                            wrapMode: Text.Wrap
                        }
                    }
                    MouseArea {
                        id: choiceArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: dialog.mode = choice.modelData.id
                    }
                    FocusRing {
                        targetRadius: choice.radius
                        visible: choice.activeFocus
                    }
                }
            }

            // HDR (and 0.6 Minimal Tone Mapping, whose roll-off reaches white at the peak): the mastering peak (the HDR10
            // metadata and the preview's knee) and the graphics white (where SDR white, titles and captions land; ITU-R
            // BT.2408's 203 nits by default).
            RowLayout {
                Layout.fillWidth: true
                visible: dialog.hdr || dialog.minimal
                spacing: Theme.space(3)
                Text {
                    Layout.fillWidth: true
                    text: qsTr("HDR peak")
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }
                NumberField {
                    objectName: "colorPeakNits"
                    Layout.preferredWidth: Theme.numberFieldWidth
                    from: 100
                    to: 10000
                    stepSize: 100
                    dragStep: 10
                    decimals: 0
                    value: dialog.peakNits
                    suffix: qsTr("nits")
                    Accessible.name: qsTr("HDR peak")
                    onValueModified: value => dialog.peakNits = value
                }
            }
            RowLayout {
                Layout.fillWidth: true
                visible: dialog.hdr || dialog.minimal
                spacing: Theme.space(3)
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Graphics white")
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }
                NumberField {
                    objectName: "colorGraphicsWhite"
                    Layout.preferredWidth: Theme.numberFieldWidth
                    from: 80
                    to: 400
                    stepSize: 1
                    dragStep: 1
                    decimals: 0
                    value: dialog.graphicsWhite
                    suffix: qsTr("nits")
                    Accessible.name: qsTr("Graphics white")
                    onValueModified: value => dialog.graphicsWhite = value
                }
            }

            Toggle {
                objectName: "colorAutoDetectLog"
                Layout.fillWidth: true
                enabled: dialog.mode !== "off"
                text: qsTr("Color manage auto-detected log media")
                tooltip: qsTr("Clips whose camera metadata names a log curve (Sony S-Log3, ARRI LogC, ...) are read through it")
                checked: dialog.autoDetectLog
                onToggled: dialog.autoDetectLog = checked
            }

            // 0.6: ACES 1.3 reference gamut compression of camera log and HDR media in the working space (always on in
            // Minimal Tone Mapping); SDR media and graphics are left as they are.
            Toggle {
                objectName: "colorGamutCompression"
                Layout.fillWidth: true
                visible: dialog.managedMode
                enabled: !dialog.minimal
                text: qsTr("Gamut compression (ACES 1.3) of camera log and HDR media")
                tooltip: dialog.minimal
                         ? qsTr("Always on in Wide Gamut (Minimal Tone Mapping)")
                         : qsTr("Brings colours from outside the working gamut in smoothly instead of clipping them, and keeps values below 0 for the grade")
                checked: dialog.minimal || dialog.gamutCompression
                onToggled: dialog.gamutCompression = checked
            }
            PropSlider {
                objectName: "colorGamutStrength"
                Layout.fillWidth: true
                visible: dialog.managedMode && (dialog.minimal || dialog.gamutCompression)
                label: qsTr("Strength")
                unit: " %"
                from: 0
                to: 100
                defaultValue: 100
                value: dialog.gamutStrength * 100
                onValueModified: v => dialog.gamutStrength = v / 100
            }

            // 0.6: what exports write (the program monitor always shows the Rec.709 picture).
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(3)
                enabled: dialog.mode !== "off"
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Output color space")
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }
                PillButton {
                    id: outputSpaceButton
                    objectName: "colorOutputSpace"
                    compact: true
                    variant: "ghost"
                    iconName: "chevron-down"
                    text: dialog.outputSpaceName
                    tooltip: qsTr("What exports write. Automatic: Rec.709, or PQ / HLG for the HDR setups' HDR exports")
                    Accessible.name: qsTr("Output color space: %1").arg(dialog.outputSpaceName)
                    onClicked: outputSpaceMenu.popup(outputSpaceButton, 0, outputSpaceButton.height + Theme.space(1))
                    OptionsMenu {
                        id: outputSpaceMenu
                        objectName: "colorOutputSpaceMenu"
                        options: Color.outputColorSpaces
                        current: dialog.outputSpace
                        onChosen: id => dialog.outputSpace = id
                    }
                }
            }

            // 0.6: SDR Conform of the SDR exports of an HDR or Wide Gamut (No / Minimal) setup.
            ColumnLayout {
                objectName: "colorSdrConform"
                Layout.fillWidth: true
                visible: dialog.conformable
                spacing: Theme.space(2)

                Toggle {
                    objectName: "colorConformEnabled"
                    Layout.fillWidth: true
                    text: qsTr("SDR Conform for SDR exports")
                    tooltip: qsTr("How this HDR or Wide Gamut sequence becomes an SDR file: tone mapping, gamut mapping and legal range")
                    checked: dialog.conformEnabled
                    onToggled: dialog.conformEnabled = checked
                }
                Text {
                    text: qsTr("Tone mapping")
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                    font.letterSpacing: Theme.labelTracking
                }
                PillTabs {
                    objectName: "colorConformOperator"
                    Layout.fillWidth: true
                    compact: true
                    enabled: dialog.conformEnabled
                    model: [qsTr("BT.2390"), qsTr("ACES"), qsTr("Reinhard")]
                    currentIndex: Math.max(0, dialog.operatorIds.indexOf(dialog.conformOperator))
                    onActivated: index => dialog.conformOperator = dialog.operatorIds[index]
                    Accessible.name: qsTr("Tone mapping")
                }
                Text {
                    text: qsTr("Gamut mapping")
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                    font.letterSpacing: Theme.labelTracking
                }
                PillTabs {
                    objectName: "colorConformGamut"
                    Layout.fillWidth: true
                    compact: true
                    enabled: dialog.conformEnabled
                    model: [qsTr("ACES compression"), qsTr("Saturation"), qsTr("Clip")]
                    currentIndex: Math.max(0, dialog.gamutIds.indexOf(dialog.conformGamut))
                    onActivated: index => dialog.conformGamut = dialog.gamutIds[index]
                    Accessible.name: qsTr("Gamut mapping")
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(3)
                    enabled: dialog.conformEnabled
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("SDR peak")
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                    }
                    PillTabs {
                        objectName: "colorConformPeak"
                        compact: true
                        model: [qsTr("100 nits"), qsTr("203 nits")]
                        currentIndex: dialog.conformPeak > 150 ? 1 : 0
                        onActivated: index => dialog.conformPeak = index === 1 ? 203 : 100
                        Accessible.name: qsTr("SDR peak")
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(3)
                    enabled: dialog.conformEnabled
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Paper white")
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                    }
                    NumberField {
                        objectName: "colorConformPaperWhite"
                        Layout.preferredWidth: Theme.numberFieldWidth
                        from: 48
                        to: 400
                        stepSize: 1
                        dragStep: 1
                        decimals: 0
                        value: dialog.conformPaperWhite
                        suffix: qsTr("nits")
                        Accessible.name: qsTr("Paper white")
                        onValueModified: value => dialog.conformPaperWhite = value
                    }
                }
                Toggle {
                    objectName: "colorConformLegal"
                    Layout.fillWidth: true
                    enabled: dialog.conformEnabled
                    text: qsTr("Legal range (soft clip above 90 %)")
                    tooltip: qsTr("Rolls the brightest components off instead of clipping them at 100 %")
                    checked: dialog.conformLegal
                    onToggled: dialog.conformLegal = checked
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Preview it on the program monitor with the Color panel's Compare & Match > SDR Conform.")
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    footerData: [
        PillButton {
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "colorManagementOk"
            variant: "primary"
            text: qsTr("OK")
            onClicked: dialog.apply()
        }
    ]
}
