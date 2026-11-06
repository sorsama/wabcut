pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// A choice of the Capture panel (design language v3): a pill that shows the choice in use - elided, with a chevron - and
// opens a menu of options [{ id, name }] under itself. `label` is its accessible name's first part.
Button {
    id: root

    property var options: []
    property string current: ""
    /// Shown when `current` is none of the options.
    property string placeholder: ""
    property string label: ""
    property string tooltip: ""

    /// Emitted before the menu opens, so the owner can refresh the options.
    signal aboutToOpen
    signal chosen(string id)

    /// The option `current` names, if there is one.
    readonly property int currentIndex: {
        for (let i = 0; i < root.options.length; ++i) {
            if (String(root.options[i].id) === root.current)
                return i
        }
        return -1
    }
    readonly property string shown: root.currentIndex >= 0 ? String(root.options[root.currentIndex].name) : root.placeholder

    implicitHeight: Theme.controlHeightSmall + Theme.space(1)
    implicitWidth: Theme.space(40)
    padding: 0
    leftPadding: Theme.space(3.5)
    rightPadding: Theme.space(3)
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    font.family: Theme.fontSans
    font.pixelSize: Theme.textMd
    font.weight: Theme.weightBody
    scale: root.down ? Theme.pressScale : 1.0
    Behavior on scale {
        NumberAnimation { duration: Theme.durPress; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.easeOutSoft }
    }

    Accessible.name: root.label !== "" ? qsTr("%1: %2").arg(root.label).arg(root.shown) : root.shown
    Accessible.description: root.tooltip

    contentItem: RowLayout {
        spacing: Theme.space(2)
        Text {
            Layout.fillWidth: true
            text: root.shown
            elide: Text.ElideRight
            color: root.currentIndex >= 0 ? Theme.textPrimary : Theme.textTertiary
            opacity: root.enabled ? 1 : Theme.disabledOpacity
            font: root.font
        }
        Icon {
            name: "chevron-down"
            size: Theme.iconSm
            color: root.enabled ? Theme.textSecondary : Theme.textDisabled
        }
    }

    background: Rectangle {
        radius: height / 2
        opacity: root.enabled ? 1 : Theme.disabledOpacity
        color: root.down ? Theme.well : root.hovered ? Theme.hover : Theme.raised
        Behavior on color { ColorAnimation { duration: Theme.durFast } }

        FocusRing {
            targetRadius: height / 2
            visible: root.visualFocus
        }
    }

    HoverHandler {
        enabled: root.enabled
        cursorShape: Qt.PointingHandCursor
    }

    onClicked: {
        root.aboutToOpen()
        menu.popup(root, 0, root.height + Theme.space(1))
    }

    OptionsMenu {
        id: menu
        options: root.options
        current: root.current
        onChosen: id => root.chosen(id)
    }
}
