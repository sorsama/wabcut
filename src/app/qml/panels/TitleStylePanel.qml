pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// Essential Graphics (lite): "New Title" and the title style — text, font, size, colour, stroke, shadow, alignment —
// with brand presets built from the Theme accents. A live card previews the style. With a title clip selected every
// change edits it (text and font on the title, the rest on its keyframable "Title Style" effect at the playhead);
// otherwise the style is the draft for the next New Title, which adds a title clip at the playhead (title.add) on a
// free video track at or above the targeted one and selects it.
Flickable {
    id: root
    objectName: "essentialGraphics"

    readonly property var style: Titles.style
    property string status: ""

    contentHeight: column.implicitHeight
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    activeFocusOnTab: false
    ScrollBar.vertical: ThinScrollBar {}

    readonly property var presetNames: ({
        violet: qsTr("Violet"), lime: qsTr("Lime"), orange: qsTr("Orange"), cyan: qsTr("Cyan"), pink: qsTr("Pink")
    })
    // Brand presets: each accent's fill on the deep tone, from Theme.accents (single source of the brand colours).
    readonly property var presets: Theme.accentOrder.map(key => {
        const a = Theme.accents[key]
        return {
            preset: key,
            label: root.presetNames[key] ?? key,
            color: a.onFill === Theme.contentWhite ? a.soft : a.fill,
            strokeEnabled: true,
            strokeColor: a.deep,
            strokeWidth: 6,
            shadowEnabled: true,
            shadowColor: a.deep,
            shadowDistance: 8,
            shadowSoftness: 16,
            swatch: a.fill
        }
    })
    readonly property var colors: [Theme.contentWhite, Theme.contentInk].concat(Theme.clipColors)
    readonly property var fontFamily: {
        const fonts = Titles.fonts
        for (let i = 0; i < fonts.length; ++i) {
            if (fonts[i].key === root.style.font)
                return fonts[i].family
        }
        return Theme.fontSans
    }

    component GroupLabel: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.DemiBold
        font.letterSpacing: Theme.labelTracking
    }

    ColumnLayout {
        id: column
        width: root.width
        spacing: Theme.space(3)

        // 0.2: the project's Text Styles as Library items (apply to the selected graphics, rename, duplicate, delete).
        TextStyleItems {
            Layout.fillWidth: true
        }

        // Preview card: the title as styled, over a dark frame.
        Rectangle {
            objectName: "titlePreview"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.round(width * 9 / 16)
            radius: Theme.radiusField
            gradient: Gradient {
                GradientStop { position: 0; color: Theme.mockFrame[0] }
                GradientStop { position: 0.6; color: Theme.mockFrame[1] }
                GradientStop { position: 1; color: Theme.mockFrame[2] }
            }
            clip: true

            readonly property real scaleFactor: width / 1920

            Text {
                id: shadowText
                visible: root.style.shadowEnabled
                anchors.fill: titleText
                anchors.leftMargin: root.style.shadowDistance * parent.scaleFactor
                anchors.topMargin: root.style.shadowDistance * parent.scaleFactor
                text: titleText.text
                horizontalAlignment: titleText.horizontalAlignment
                verticalAlignment: titleText.verticalAlignment
                wrapMode: titleText.wrapMode
                font: titleText.font
                color: root.style.shadowColor
                opacity: 0.6
            }
            Text {
                id: titleText
                anchors.fill: parent
                anchors.margins: Theme.space(3)
                text: root.style.text
                horizontalAlignment: root.style.alignment === "left" ? Text.AlignLeft
                                   : root.style.alignment === "right" ? Text.AlignRight : Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.Wrap
                font.family: root.fontFamily
                font.pixelSize: Math.max(8, root.style.size * parent.scaleFactor)
                font.bold: root.style.bold
                color: root.style.color
                style: root.style.strokeEnabled ? Text.Outline : Text.Normal
                styleColor: root.style.strokeColor
            }
        }

        // What the edits go to: the selected title, or the next one.
        Text {
            objectName: "titleTarget"
            Layout.fillWidth: true
            text: Titles.editing ? qsTr("Editing “%1”").arg(Titles.targetName) : qsTr("Style for the next title")
            elide: Text.ElideRight
            color: Titles.editing ? Theme.accentStroke : Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Font.DemiBold
        }

        PillButton {
            objectName: "newTitleButton"
            Layout.fillWidth: true
            variant: "primary"
            iconName: "type"
            text: qsTr("New Title")
            enabled: Titles.available
            tooltip: Titles.available ? qsTr("A text clip at the playhead on the targeted video track") : Titles.unavailableReason
            onClicked: root.status = Titles.createTitle()
        }
        Text {
            objectName: "titleStatus"
            Layout.fillWidth: true
            visible: text !== ""
            text: root.status !== "" ? root.status : (Titles.available ? "" : Titles.unavailableReason)
            wrapMode: Text.Wrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        GroupLabel { text: qsTr("Brand styles") }
        Flow {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            Repeater {
                model: root.presets
                delegate: PillButton {
                    required property var modelData
                    objectName: "titlePreset_" + modelData.preset
                    compact: true
                    text: modelData.label
                    variant: root.style.preset === modelData.preset ? "primary" : "secondary"
                    onClicked: Titles.applyPreset(modelData)
                }
            }
        }

        GroupLabel { text: qsTr("Text") }
        TextField {
            id: textField
            objectName: "titleText"
            Layout.fillWidth: true
            implicitHeight: Theme.controlHeightSmall + Theme.space(1)
            text: root.style.text
            leftPadding: Theme.space(3)
            rightPadding: Theme.space(3)
            color: Theme.textPrimary
            selectionColor: Theme.accentFill
            selectedTextColor: Theme.accentOnFill
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            selectByMouse: true
            Accessible.name: qsTr("Title text")
            onTextEdited: Titles.setStyleValue("text", text)
            onEditingFinished: Titles.endEdit()
            background: Rectangle {
                radius: height / 2
                color: Theme.raised
                border.width: textField.activeFocus ? Theme.focusRingWidth : 0
                border.color: Theme.focus
            }
        }

        GroupLabel { text: qsTr("Font") }
        Flow {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            Repeater {
                model: Titles.fonts
                delegate: PillButton {
                    required property var modelData
                    compact: true
                    text: modelData.label
                    variant: root.style.font === modelData.key ? "primary" : "secondary"
                    onClicked: Titles.setStyleValue("font", modelData.key)
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            PropSlider {
                Layout.fillWidth: true
                label: qsTr("Size")
                iconName: "type"
                unit: qsTr("px")
                from: 12
                to: 300
                value: root.style.size
                defaultValue: 96
                onValueModified: v => Titles.setStyleValue("size", v)
                onGestureEnded: Titles.endEdit()
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            IconButton {
                objectName: "titleBold"
                compact: true
                checkable: true
                checked: root.style.bold
                iconName: "type"
                tooltip: qsTr("Bold")
                onClicked: Titles.setStyleValue("bold", !root.style.bold)
            }
            Item { Layout.fillWidth: true; Layout.preferredHeight: 1 }
            Repeater {
                model: [
                    { value: "left", icon: "align-left", label: qsTr("Align left") },
                    { value: "center", icon: "align-center", label: qsTr("Center") },
                    { value: "right", icon: "align-right", label: qsTr("Align right") }
                ]
                delegate: IconButton {
                    required property var modelData
                    compact: true
                    checkable: true
                    checked: root.style.alignment === modelData.value
                    iconName: modelData.icon
                    tooltip: modelData.label
                    onClicked: Titles.setStyleValue("alignment", modelData.value)
                }
            }
        }

        GroupLabel { text: qsTr("Fill") }
        Flow {
            Layout.fillWidth: true
            spacing: Theme.space(1.5)
            Repeater {
                model: root.colors
                delegate: Swatch {
                    required property string modelData
                    swatchColor: modelData
                    checkable: false
                    checked: String(root.style.color).toUpperCase() === modelData.toUpperCase()
                    tooltip: qsTr("Fill %1").arg(modelData)
                    onClicked: Titles.setStyleValue("color", modelData)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            GroupLabel { Layout.fillWidth: true; text: qsTr("Stroke") }
            Toggle {
                objectName: "titleStroke"
                tooltip: qsTr("Stroke")
                checked: root.style.strokeEnabled
                onToggled: Titles.setStyleValue("strokeEnabled", checked)
            }
        }
        PropSlider {
            Layout.fillWidth: true
            visible: root.style.strokeEnabled
            label: qsTr("Width")
            unit: qsTr("px")
            from: 0
            to: 40
            value: root.style.strokeWidth
            defaultValue: 4
            onValueModified: v => Titles.setStyleValue("strokeWidth", v)
            onGestureEnded: Titles.endEdit()
        }

        RowLayout {
            Layout.fillWidth: true
            GroupLabel { Layout.fillWidth: true; text: qsTr("Shadow") }
            Toggle {
                objectName: "titleShadow"
                tooltip: qsTr("Shadow")
                checked: root.style.shadowEnabled
                onToggled: Titles.setStyleValue("shadowEnabled", checked)
            }
        }
        PropSlider {
            Layout.fillWidth: true
            visible: root.style.shadowEnabled
            label: qsTr("Distance")
            unit: qsTr("px")
            from: 0
            to: 100
            value: root.style.shadowDistance
            defaultValue: 6
            onValueModified: v => Titles.setStyleValue("shadowDistance", v)
            onGestureEnded: Titles.endEdit()
        }
        PropSlider {
            Layout.fillWidth: true
            visible: root.style.shadowEnabled
            label: qsTr("Softness")
            unit: qsTr("px")
            from: 0
            to: 100
            value: root.style.shadowSoftness
            defaultValue: 12
            onValueModified: v => Titles.setStyleValue("shadowSoftness", v)
            onGestureEnded: Titles.endEdit()
        }

        PillButton {
            Layout.alignment: Qt.AlignRight
            compact: true
            iconName: "rotate-ccw"
            text: qsTr("Reset style")
            onClicked: Titles.resetStyle()
        }
    }
}
