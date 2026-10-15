pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// New project: the sequence preset (Home.presets: 1080p at 23.976 / 25 / 29.97 / 30 / 60, 4K UHD, vertical 9:16,
// square 1:1, custom) as tiles with the frame's shape, then a name and a folder. 4K, vertical, square and custom take
// another frame rate (Home.rates); custom takes any size. The lime Create saves <folder>/<name>.wabcut and opens it
// (Home.createProject, after the unsaved-changes prompt in Main).
HomeCard {
    id: card
    objectName: "homeNewProjectCard"

    property url folder: Home.defaultFolder
    property int presetIndex: 0
    readonly property var preset: Home.presets[presetIndex]
    property int rateIndex: -1        // -1: the preset's own rate
    property int customWidth: 1920
    property int customHeight: 1080
    property string error: ""

    signal createRequested(string name, url folder, var sequence)

    /// The sequence to create: { width, height, rateNum, rateDen }.
    function sequence() {
        const rate = rateIndex >= 0 && preset.rateChoice ? Home.rates[rateIndex] : preset
        return {
            width: preset.custom ? customWidth : preset.width,
            height: preset.custom ? customHeight : preset.height,
            rateNum: rate.rateNum,
            rateDen: rate.rateDen
        }
    }
    function rateLabel() {
        return rateIndex >= 0 && preset.rateChoice ? Home.rates[rateIndex].label : preset.rateLabel
    }
    function selectPreset(id) {
        for (let i = 0; i < Home.presets.length; ++i) {
            if (Home.presets[i].id === id) {
                presetIndex = i
                rateIndex = -1
                return true
            }
        }
        return false
    }
    function create() {
        error = Home.validateProject(nameField.text, folder)
        if (error === "")
            createRequested(nameField.text.trim(), folder, sequence())
    }
    /// The name field, selected (Home's "New project" button).
    function focusName() {
        nameField.forceActiveFocus()
        nameField.selectAll()
    }
    /// "Vertical 9:16" reads "vertical 9:16" inside a sentence; names that start with a number or an acronym stay.
    function inSentence(name: string): string {
        return /^[A-Z][a-z]/.test(name) ? name.charAt(0).toLowerCase() + name.slice(1) : name
    }

    title: qsTr("New project")

    // The brand's lime pill: the one Create on Home.
    component LimeButton: AbstractButton {
        id: lime
        property string tooltip: ""
        implicitHeight: Theme.controlHeightLarge
        implicitWidth: limeText.implicitWidth + Theme.space(10)
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        scale: down ? Theme.pressScale : 1
        Behavior on scale { NumberAnimation { duration: Theme.durPress; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeSnapOut } }
        Accessible.role: Accessible.Button
        Accessible.name: text
        Accessible.description: tooltip

        background: Rectangle {
            radius: height / 2
            color: lime.down ? Qt.darker(Theme.brandLime, 1.08) : lime.hovered ? Qt.lighter(Theme.brandLime, 1.06) : Theme.brandLime
            Behavior on color { ColorAnimation { duration: Theme.durFast } }
        }
        contentItem: Text {
            id: limeText
            text: lime.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            color: Theme.clipInk
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            font.weight: Theme.weightTitle
        }
        Loader {
            active: lime.hovered && lime.tooltip !== ""
            sourceComponent: Tooltip {
                parent: lime
                visible: true
                text: lime.tooltip
            }
        }
        FocusRing {
            targetRadius: lime.height / 2
            visible: lime.visualFocus
        }
        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }
    }

    // Sequence presets
    GridLayout {
        Layout.fillWidth: true
        columns: width >= Theme.space(96) ? 3 : 2
        columnSpacing: Theme.space(2)
        rowSpacing: Theme.space(2)

        Repeater {
            model: Home.presets
            delegate: AbstractButton {
                id: presetCard
                required property var modelData
                required property int index
                readonly property bool selected: card.presetIndex === index
                readonly property color ink: selected ? Theme.accentOnFill : Theme.textPrimary
                objectName: "homePreset_" + modelData.id
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                implicitHeight: Theme.space(13)
                leftPadding: Theme.space(3)
                rightPadding: Theme.space(2)
                focusPolicy: Qt.StrongFocus
                hoverEnabled: true
                Accessible.name: modelData.custom ? modelData.name
                                                  : qsTr("%1, %2 fps, %3").arg(modelData.name).arg(modelData.rateLabel).arg(modelData.sizeText)
                Accessible.role: Accessible.RadioButton
                Accessible.checked: selected
                onClicked: {
                    card.presetIndex = index
                    card.rateIndex = -1
                }

                background: Rectangle {
                    radius: Theme.radiusCard - Theme.space(0.5)
                    color: presetCard.selected ? Theme.accentFill : presetCard.hovered ? Theme.hover : Theme.raised
                    Behavior on color { ColorAnimation { duration: Theme.durFast } }
                }

                contentItem: RowLayout {
                    spacing: Theme.space(2.5)

                    // The frame's shape
                    Item {
                        implicitWidth: Theme.space(7)
                        implicitHeight: Theme.space(7)
                        Rectangle {
                            anchors.centerIn: parent
                            readonly property real unit: Theme.space(6)
                            width: presetCard.modelData.shape === "portrait" ? unit * 9 / 16
                                 : presetCard.modelData.shape === "square" ? unit * 0.75 : unit
                            height: presetCard.modelData.shape === "portrait" ? unit
                                  : presetCard.modelData.shape === "square" ? unit * 0.75 : unit * 9 / 16
                            radius: Theme.space(1)
                            color: "transparent"
                            border.width: Theme.hairline * 2
                            border.color: presetCard.ink
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Text {
                            Layout.fillWidth: true
                            text: presetCard.modelData.name
                            elide: Text.ElideRight
                            color: presetCard.ink
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                            font.weight: Theme.weightTitle
                        }
                        Text {
                            Layout.fillWidth: true
                            text: presetCard.modelData.custom ? qsTr("Any size") : qsTr("%1 fps").arg(presetCard.modelData.rateLabel)
                            elide: Text.ElideRight
                            color: presetCard.ink
                            opacity: 0.75
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textMicro
                        }
                    }
                }

                FocusRing {
                    targetRadius: Theme.radiusCard - Theme.space(0.5)
                    visible: presetCard.visualFocus
                }

                HoverHandler {
                    enabled: presetCard.enabled
                    cursorShape: Qt.PointingHandCursor
                }
            }
        }
    }

    // Custom size
    RowLayout {
        Layout.fillWidth: true
        visible: card.preset.custom
        spacing: Theme.space(2)
        Text {
            text: qsTr("Frame size")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        Item { Layout.fillWidth: true }
        NumberField {
            objectName: "homeCustomWidth"
            Layout.preferredWidth: Theme.numberFieldWidth
            from: 16
            to: 8192
            value: card.customWidth
            suffix: qsTr("px")
            onValueModified: v => card.customWidth = Math.round(v / 2) * 2
            Accessible.name: qsTr("Frame width in pixels")
        }
        Icon {
            name: "x"
            size: Theme.iconXs
            color: Theme.textTertiary
        }
        NumberField {
            objectName: "homeCustomHeight"
            Layout.preferredWidth: Theme.numberFieldWidth
            from: 16
            to: 8192
            value: card.customHeight
            suffix: qsTr("px")
            onValueModified: v => card.customHeight = Math.round(v / 2) * 2
            Accessible.name: qsTr("Frame height in pixels")
        }
    }

    // Frame rate (presets that can change it)
    Flow {
        Layout.fillWidth: true
        visible: card.preset.rateChoice
        spacing: Theme.space(1)
        Repeater {
            model: Home.rates
            delegate: PillButton {
                required property var modelData
                required property int index
                objectName: "homeRate_" + modelData.label
                compact: true
                variant: card.rateLabel() === modelData.label ? "primary" : "secondary"
                text: modelData.label
                tooltip: qsTr("%1 frames per second").arg(modelData.label)
                onClicked: card.rateIndex = index
            }
        }
    }

    // Name and folder
    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)

        TextField {
            id: nameField
            objectName: "homeProjectName"
            Layout.fillWidth: true
            implicitHeight: Theme.controlHeight
            leftPadding: Theme.space(4)
            rightPadding: Theme.space(4)
            text: qsTr("Untitled Project")
            placeholderText: qsTr("Project name")
            placeholderTextColor: Theme.textTertiary
            color: Theme.textPrimary
            selectionColor: Theme.accentFill
            selectedTextColor: Theme.accentOnFill
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            font.weight: Font.DemiBold
            selectByMouse: true
            Accessible.name: qsTr("Project name")
            onTextEdited: card.error = ""
            onAccepted: card.create()
            background: Rectangle {
                radius: height / 2
                color: Theme.raised
                border.width: nameField.activeFocus ? Theme.focusRingWidth : 0
                border.color: Theme.accentStroke
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: Theme.controlHeight
                radius: height / 2
                color: Theme.sunken
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space(3.5)
                    anchors.rightMargin: Theme.space(4)
                    spacing: Theme.space(2)
                    Icon {
                        name: "folder"
                        size: Theme.iconXs
                        color: Theme.textTertiary
                    }
                    Text {
                        objectName: "homeProjectFolder"
                        Layout.fillWidth: true
                        text: Home.projectPath(nameField.text, card.folder)
                        elide: Text.ElideMiddle
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                }
            }
            PillButton {
                iconName: "folder-open"
                text: qsTr("Browse…")
                tooltip: qsTr("Choose where to save the project")
                onClicked: folderDialog.open()
            }
        }
    }

    // Summary and error
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 0
        Text {
            objectName: "homeSequenceSummary"
            Layout.fillWidth: true
            text: qsTr("%1 × %2 · %3 fps").arg(card.sequence().width).arg(card.sequence().height).arg(card.rateLabel())
            elide: Text.ElideRight
            color: Theme.textSecondary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textSm
        }
        Text {
            objectName: "homeNewProjectError"
            Layout.fillWidth: true
            visible: card.error !== ""
            text: card.error
            wrapMode: Text.Wrap
            color: Theme.danger
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    LimeButton {
        objectName: "homeCreateButton"
        Layout.fillWidth: true
        text: qsTr("Create %1 project").arg(card.inSentence(card.preset.name))
        tooltip: qsTr("Create the project and open it")
        onClicked: card.create()
    }

    FolderDialog {
        id: folderDialog
        currentFolder: card.folder
        onAccepted: card.folder = selectedFolder
    }
}
