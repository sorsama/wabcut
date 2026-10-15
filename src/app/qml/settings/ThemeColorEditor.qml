pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Settings > Appearance > Custom colours (0.1.3): every themable colour token, grouped (surfaces, borders, text,
// accent, timeline and status), and the 16 clip label colours. A swatch opens the colour picker; the theme follows
// live (Theme.customColors / customClipColors, saved by Preferences). An edited colour shows its reset button.
Column {
    id: editor
    objectName: "themeColorEditor"

    readonly property var groups: [
        { title: qsTr("Surfaces"), tokens: [
            { key: "canvas", label: qsTr("Window background") },
            { key: "sunken", label: qsTr("Tab strips and timeline lanes") },
            { key: "panel", label: qsTr("Panels") },
            { key: "section", label: qsTr("Cards in panels") },
            { key: "raised", label: qsTr("Buttons, fields and menus") },
            { key: "hover", label: qsTr("Hover") },
            { key: "well", label: qsTr("Wells and slider tracks") }] },

        { title: qsTr("Borders"), tokens: [
            { key: "border", label: qsTr("Edges") },
            { key: "borderStrong", label: qsTr("Strong edges") },
            { key: "rimLight", label: qsTr("Rim light") }] },
        { title: qsTr("Text"), tokens: [
            { key: "textPrimary", label: qsTr("Primary") },
            { key: "textSecondary", label: qsTr("Secondary") },
            { key: "textTertiary", label: qsTr("Meta") },
            { key: "textDisabled", label: qsTr("Disabled") }] },
        { title: qsTr("Accent"), tokens: [
            { key: "accentFill", label: qsTr("Fill: buttons, active tab") },
            { key: "accentOnFill", label: qsTr("Text on the fill") },
            { key: "accentStroke", label: qsTr("Lines, focus, selection") },
            { key: "accentSoft", label: qsTr("Soft highlight") },
            { key: "accentDeep", label: qsTr("Pressed and deep tints") },
            { key: "beamMid", label: qsTr("Beam middle") },
            { key: "beamEnd", label: qsTr("Beam end") }] },
        { title: qsTr("Timeline and status"), tokens: [
            { key: "playhead", label: qsTr("Playhead") },
            { key: "success", label: qsTr("Success") },
            { key: "warning", label: qsTr("Warning") },
            { key: "danger", label: qsTr("Danger") },
            { key: "info", label: qsTr("Info") }] }
    ]
    // The token (or "clip:<index>") the picker is editing.
    property string editingKey: ""

    function isEdited(key: string): bool {
        return (Theme.customColors ?? {})[key] !== undefined
    }
    function setColor(key: string, hex: string) {
        const next = Object.assign({}, Theme.customColors ?? {})
        next[key] = hex
        Theme.customColors = next
    }
    function resetColor(key: string) {
        const next = Object.assign({}, Theme.customColors ?? {})
        delete next[key]
        Theme.customColors = next
    }
    function setClipColor(index: int, hex: string) {
        const list = Theme.clipColors.slice()
        list[index] = hex.length === 9 ? "#" + hex.slice(3) : hex
        Theme.customClipColors = list
    }
    function tokenColor(key: string): color {
        return Theme[key]
    }
    function hexOf(c: color): string {
        return picker.hexOf(c)
    }
    function pick(anchor: Item, key: string, label: string, current: color) {
        editor.editingKey = key
        picker.allowAlpha = !key.startsWith("clip:")
        picker.parent = anchor
        picker.x = 0
        picker.y = anchor.height + Theme.space(1)
        picker.edit(current, label)
    }

    width: parent ? parent.width : implicitWidth
    spacing: Theme.space(4)
    enabled: !Theme.highContrast

    ColorPickerPopover {
        id: picker
        onColorPicked: hex => {
            const key = editor.editingKey
            if (key.startsWith("clip:"))
                editor.setClipColor(Number(key.slice(5)), hex)
            else if (key !== "")
                editor.setColor(key, hex)
        }
        onClosed: editor.editingKey = ""
    }

    Repeater {
        model: editor.groups

        delegate: Column {
            id: group
            required property var modelData
            width: editor.width
            spacing: Theme.space(1)

            Text {
                text: group.modelData.title
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                font.weight: Font.Medium
            }

            Grid {
                width: parent.width
                columns: 2
                columnSpacing: Theme.space(6)

                Repeater {
                    model: group.modelData.tokens

                    delegate: Item {
                        id: tokenRow
                        required property var modelData
                        readonly property color value: editor.tokenColor(modelData.key)
                        readonly property bool edited: editor.isEdited(modelData.key)

                        objectName: "themeColor_" + modelData.key
                        width: (group.width - Theme.space(6)) / 2
                        height: Theme.rowHeight + Theme.space(1)

                        AbstractButton {
                            id: swatchButton
                            objectName: "themeSwatch_" + tokenRow.modelData.key
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            width: Theme.space(10)
                            height: Theme.controlHeightSmall - Theme.space(2)
                            focusPolicy: Qt.StrongFocus
                            hoverEnabled: true
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("%1: %2, change").arg(tokenRow.modelData.label).arg(editor.hexOf(tokenRow.value))
                            onClicked: editor.pick(swatchButton, tokenRow.modelData.key, tokenRow.modelData.label, tokenRow.value)

                            background: Rectangle {
                                radius: Theme.radiusBadge
                                color: tokenRow.value
                                border.width: swatchButton.hovered || editor.editingKey === tokenRow.modelData.key
                                              ? Theme.focusRingWidth : Theme.hairline
                                border.color: editor.editingKey === tokenRow.modelData.key ? Theme.accentStroke : Theme.borderStrong
                                FocusRing {
                                    targetRadius: Theme.radiusBadge
                                    visible: swatchButton.visualFocus
                                }
                            }
                            HoverHandler {
                                cursorShape: Qt.PointingHandCursor
                            }
                        }
                        Text {
                            anchors.left: swatchButton.right
                            anchors.leftMargin: Theme.space(2)
                            anchors.right: hexText.left
                            anchors.rightMargin: Theme.space(2)
                            anchors.verticalCenter: parent.verticalCenter
                            text: tokenRow.modelData.label
                            elide: Text.ElideRight
                            color: tokenRow.edited ? Theme.textPrimary : Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                            font.weight: tokenRow.edited ? Font.Medium : Font.Normal
                        }
                        Text {
                            id: hexText
                            anchors.right: resetButton.left
                            anchors.rightMargin: Theme.space(1)
                            anchors.verticalCenter: parent.verticalCenter
                            text: editor.hexOf(tokenRow.value)
                            color: Theme.textTertiary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                        IconButton {
                            id: resetButton
                            objectName: "themeReset_" + tokenRow.modelData.key
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            compact: true
                            iconName: "rotate-ccw"
                            iconSize: Theme.iconXs
                            opacity: tokenRow.edited ? 1 : 0
                            enabled: tokenRow.edited
                            tooltip: qsTr("Back to the theme's %1").arg(tokenRow.modelData.label.toLowerCase())
                            onClicked: editor.resetColor(tokenRow.modelData.key)
                        }
                    }
                }
            }
        }
    }

    // ---- The 16 clip label colours ----
    Column {
        width: editor.width
        spacing: Theme.space(1)

        Row {
            spacing: Theme.space(2)
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Clip labels")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                font.weight: Font.Medium
            }
            PillButton {
                objectName: "themeResetClipColors"
                anchors.verticalCenter: parent.verticalCenter
                visible: Theme.customClipColors.length > 0
                compact: true
                variant: "ghost"
                iconName: "rotate-ccw"
                text: qsTr("Reset labels")
                onClicked: Theme.customClipColors = []
            }
        }
        Flow {
            width: parent.width
            spacing: Theme.space(1)

            Repeater {
                model: Theme.clipColors.length

                delegate: AbstractButton {
                    id: labelButton
                    required property int index
                    readonly property color value: Theme.clipColors[index]
                    objectName: "themeClipColor_" + index
                    width: Theme.swatchSize + Theme.space(3)
                    height: width
                    focusPolicy: Qt.StrongFocus
                    hoverEnabled: true
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Clip label %1: %2, change").arg(index + 1).arg(editor.hexOf(value))
                    onClicked: editor.pick(labelButton, "clip:" + index, qsTr("Clip label %1").arg(index + 1), value)

                    background: Rectangle {
                        radius: Theme.radiusBadge
                        color: labelButton.value
                        border.width: labelButton.hovered || editor.editingKey === "clip:" + labelButton.index
                                      ? Theme.focusRingWidth : Theme.hairline
                        border.color: editor.editingKey === "clip:" + labelButton.index ? Theme.textPrimary : Theme.borderStrong
                        FocusRing {
                            targetRadius: Theme.radiusBadge
                            visible: labelButton.visualFocus
                        }
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                }
            }
        }
    }
}
