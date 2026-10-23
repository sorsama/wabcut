pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// A format tile on the New project page (design language v3): the frame's shape (landscape, portrait, square; a faint
// frame for Custom), the name and one mono line (the rate). The chosen tile is filled
// with the accent (black text). The tiles are one radio group: only the chosen one is a Tab stop and the arrows choose
// the previous or next tile (`stepRequested`).
AbstractButton {
    id: tile

    property string name: ""
    property string detail: ""
    /// "landscape", "portrait", "square" or "custom".
    property string shape: "landscape"
    property bool selected: false
    readonly property color ink: tile.selected ? Theme.accentOnFill : Theme.textPrimary

    /// -1 (Left, Up) or +1 (Right, Down).
    signal stepRequested(int direction)

    implicitHeight: Theme.space(13)
    leftPadding: Theme.space(3)
    rightPadding: Theme.space(2)
    // (Kept while it has the focus: the policy may not drop Tab from the focused item.)
    focusPolicy: tile.selected || tile.activeFocus ? Qt.StrongFocus : Qt.ClickFocus
    hoverEnabled: true
    Accessible.role: Accessible.RadioButton
    Accessible.name: tile.detail !== "" ? qsTr("%1, %2").arg(tile.name).arg(tile.detail) : tile.name
    Accessible.checkable: true
    Accessible.checked: tile.selected
    Keys.onLeftPressed: tile.stepRequested(-1)
    Keys.onUpPressed: tile.stepRequested(-1)
    Keys.onRightPressed: tile.stepRequested(1)
    Keys.onDownPressed: tile.stepRequested(1)

    background: Rectangle {
        radius: Theme.radiusCard - Theme.space(0.5)
        color: tile.selected ? Theme.accentFill : tile.hovered ? Theme.hover : Theme.raised
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
                width: tile.shape === "portrait" ? unit * 9 / 16 : tile.shape === "square" ? unit * 0.75 : unit
                height: tile.shape === "portrait" ? unit : tile.shape === "square" ? unit * 0.75 : unit * 9 / 16
                radius: Theme.space(1)
                color: "transparent"
                border.width: Theme.hairline * 2
                border.color: tile.ink
                opacity: tile.shape === "custom" ? 0.55 : 1
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            Text {
                Layout.fillWidth: true
                text: tile.name
                elide: Text.ElideRight
                color: tile.ink
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                font.weight: Theme.weightTitle
            }
            Text {
                Layout.fillWidth: true
                text: tile.detail
                elide: Text.ElideRight
                color: tile.ink
                opacity: 0.75
                font.family: Theme.fontMono
                font.pixelSize: Theme.textMicro
            }
        }
    }

    FocusRing {
        targetRadius: Theme.radiusCard - Theme.space(0.5)
        visible: tile.visualFocus
    }
    HoverHandler {
        enabled: tile.enabled
        cursorShape: Qt.PointingHandCursor
    }
}
