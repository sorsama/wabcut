pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// A theme in Settings > Appearance (0.1.3): a small preview of its window (canvas, a panel with a title, a raised card,
// text and its accent) and its name. Click applies it; a saved theme also has a delete button. The previews show a
// theme's own colours over its mode's built-in surfaces (the same values as Theme's dark and light palettes).
AbstractButton {
    id: card

    /// {name, dark, accent, colors, clipColors}.
    property var theme: ({})
    property string label: ""
    property bool builtIn: true
    property bool current: false
    signal chosen()
    signal deleteRequested()

    // The mode's built-in surfaces, for the preview (a function, not a property: the preview's bindings may run before
    // a property's own binding would have).
    function baseColor(key: string): string {
        const dark = card.theme?.dark !== false
        const base = Theme.modeSwatches[dark ? "dark" : "light"]
        return base[key] ?? base.textTertiary
    }
    function colorOf(key: string): color {
        const custom = (card.theme?.colors ?? {})[key]
        if (typeof custom === "string" && custom !== "")
            return custom
        if (key === "accentFill")
            return Theme.accents[Theme.canonicalAccent(card.theme?.accent ?? Theme.accentName)].fill
        return card.baseColor(key)
    }

    implicitWidth: Theme.space(36)
    implicitHeight: Theme.space(26)
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.role: Accessible.RadioButton
    Accessible.name: qsTr("Theme %1").arg(card.label)
    Accessible.checked: card.current
    onClicked: card.chosen()

    background: Rectangle {
        radius: Theme.radiusCard
        color: Theme.raised
        border.width: card.current ? Theme.focusRingWidth : Theme.hairline
        border.color: card.current ? Theme.accentStroke : card.hovered ? Theme.borderStrong : Theme.border
        Behavior on border.color { ColorAnimation { duration: Theme.durFast } }

        FocusRing {
            targetRadius: Theme.radiusCard
            visible: card.visualFocus
        }
    }

    contentItem: Item {
        // The preview: the window's canvas, a panel with its dot-marked title, a raised card and the accent pill.
        Rectangle {
            id: preview
            x: Theme.space(1.5)
            y: Theme.space(1.5)
            width: parent.width - Theme.space(3)
            height: parent.height - Theme.space(9)
            radius: Theme.radiusField
            color: card.colorOf("canvas")
            clip: true

            Rectangle {
                x: Theme.space(1.5)
                y: Theme.space(1.5)
                width: parent.width * 0.62
                height: parent.height - Theme.space(3)
                radius: Theme.radiusBadge
                color: card.colorOf("panel")
                border.width: Theme.hairline
                border.color: card.colorOf("border")

                Rectangle {
                    x: Theme.space(1.5)
                    y: Theme.space(2.25)
                    width: Theme.space(1.5)
                    height: width
                    radius: width / 2
                    color: card.colorOf("accentFill")
                }
                Rectangle {
                    x: Theme.space(3.5)
                    y: Theme.space(2.25)
                    width: parent.width * 0.5
                    height: Theme.space(1.5)
                    radius: height / 2
                    color: card.colorOf("textPrimary")
                }
                Rectangle {
                    x: Theme.space(1.5)
                    y: Theme.space(5.5)
                    width: parent.width * 0.7
                    height: Theme.space(1)
                    radius: height / 2
                    color: card.colorOf("textTertiary")
                }
                Rectangle {
                    x: Theme.space(1.5)
                    y: Theme.space(8)
                    width: parent.width - Theme.space(3)
                    height: Theme.space(3.5)
                    radius: Theme.radiusBadge
                    color: card.colorOf("raised")
                }
            }
            Rectangle {
                anchors.right: parent.right
                anchors.rightMargin: Theme.space(1.5)
                y: Theme.space(1.5)
                width: parent.width * 0.3 - Theme.space(3)
                height: Theme.space(4)
                radius: height / 2
                color: card.colorOf("accentFill")

            }
            Rectangle {
                anchors.right: parent.right
                anchors.rightMargin: Theme.space(1.5)
                y: Theme.space(7)
                width: parent.width * 0.3 - Theme.space(3)
                height: parent.height - Theme.space(8.5)
                radius: Theme.radiusBadge
                color: card.colorOf("panel")
            }
        }

        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.space(2.5)
            anchors.right: deleteButton.visible ? deleteButton.left : parent.right
            anchors.rightMargin: Theme.space(1)
            anchors.bottom: parent.bottom
            anchors.bottomMargin: Theme.space(1.5)
            text: card.label
            elide: Text.ElideRight
            color: card.current ? Theme.textPrimary : Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: card.current ? Font.Medium : Font.Normal
        }
        IconButton {
            id: deleteButton
            objectName: "deleteTheme_" + (card.theme.name ?? "")
            visible: !card.builtIn
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: Theme.space(0.5)
            compact: true
            iconName: "trash-2"
            iconSize: Theme.iconXs
            tooltip: qsTr("Delete %1").arg(card.label)
            onClicked: card.deleteRequested()
        }
    }

    HoverHandler {
        cursorShape: Qt.PointingHandCursor
    }
}
