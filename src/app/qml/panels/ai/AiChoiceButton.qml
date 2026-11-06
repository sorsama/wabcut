pragma ComponentBehavior: Bound
import QtQuick

// A dropdown pill of Settings > AI (r07): the chosen value's name (elided, never clipped by its row) and a chevron.
// With `options` ([{id, name}]) a click opens a menu of them with a check on `current` and emits chosen(id); without
// options it emits clicked() (the caller opens its own picker, e.g. AiModelPicker). `secondaryText` adds a dimmer part
// after the name ("Qwen3 8B · This computer"). In its own file importing only QtQuick, so Wabcut's ContextMenu is used.
Rectangle {
    id: control

    property var options: []
    property string current: ""
    /// Shown instead of the current option's name ("" = the option's name, else `placeholder`).
    property string text: ""
    property string secondaryText: ""
    property string placeholder: qsTr("Choose")
    property string iconName: ""
    property string tooltip: ""
    property bool compact: true
    property real maximumWidth: Theme.space(80)
    readonly property bool hovered: mouse.containsMouse
    readonly property string shownText: control.text !== "" ? control.text
                                         : (control.options.find(o => o.id === control.current)?.name ?? control.placeholder)

    signal chosen(string id)
    signal clicked()

    function open() {
        if (control.options.length > 0)
            menu.popup(control, 0, control.height + Theme.space(1))
        else
            control.clicked()
    }

    implicitHeight: compact ? Theme.controlHeightSmall : Theme.controlHeight
    implicitWidth: Math.min(control.maximumWidth,
                            label.implicitWidth + chevron.width + content.spacing
                            + (leading.visible ? leading.width + content.spacing : 0)
                            + content.anchors.leftMargin + content.anchors.rightMargin)
    radius: height / 2
    color: !control.enabled ? Theme.raised : mouse.pressed ? Theme.well : control.hovered ? Theme.hover : Theme.raised
    border.width: Theme.hairline
    border.color: Theme.borderStrong
    opacity: control.enabled ? 1 : Theme.disabledOpacity
    activeFocusOnTab: true

    Accessible.role: Accessible.ComboBox
    Accessible.name: control.tooltip !== "" ? control.tooltip : control.shownText
    Accessible.description: control.shownText
    Accessible.onPressAction: control.open()
    Keys.onSpacePressed: control.open()
    Keys.onReturnPressed: control.open()
    Keys.onEnterPressed: control.open()

    Behavior on color { ColorAnimation { duration: Theme.durFast } }

    FocusRing {
        targetRadius: control.radius
        visible: control.activeFocus
    }

    Row {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.space(control.compact ? 3 : 4)
        anchors.rightMargin: Theme.space(control.compact ? 2.5 : 3)
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space(1.5)

        Icon {
            id: leading
            anchors.verticalCenter: parent.verticalCenter
            visible: control.iconName !== ""
            name: control.iconName
            size: Theme.iconSm
            color: Theme.textSecondary
        }
        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(0, Math.min(implicitWidth, content.width - chevron.width - content.spacing
                                        - (leading.visible ? leading.width + content.spacing : 0)))
            text: control.secondaryText !== ""
                  ? control.shownText + "<font color=\"" + Theme.textTertiary + "\">  " + control.secondaryText + "</font>"
                  : control.shownText
            textFormat: control.secondaryText !== "" ? Text.StyledText : Text.PlainText
            elide: Text.ElideRight
            color: Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: control.compact ? Theme.textSm : Theme.textMd
            font.weight: Theme.weightLabel
        }
        Icon {
            id: chevron
            anchors.verticalCenter: parent.verticalCenter
            name: "chevron-down"
            size: Theme.iconSm
            color: Theme.textSecondary
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        enabled: control.enabled
        hoverEnabled: true
        cursorShape: control.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: {
            control.forceActiveFocus(Qt.MouseFocusReason)
            control.open()
        }
    }

    Loader {
        active: mouse.containsMouse && control.tooltip !== ""
        sourceComponent: Tooltip {
            parent: control
            visible: true
            text: control.tooltip
        }
    }

    OptionsMenu {
        id: menu
        options: control.options
        current: control.current
        onChosen: id => control.chosen(id)
    }
}
