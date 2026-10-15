pragma ComponentBehavior: Bound
import QtQuick

// One group of the Tracks panel's edit tools (0.1.3, Premiere's tool palette): the button shows the group's tool (the
// active one, else the one last used from it) and selects it on a click. A group with more tools carries a small
// corner mark; right-click, press and hold, or ↓ / the menu key on the focused button opens its flyout (ToolFlyout),
// where picking a tool makes it the panel's tool and the group's face. The palette in compact form is one such button
// whose group is every tool.
IconButton {
    id: control

    /// { key, tools: [tool keys] }.
    required property var group
    /// Tool key -> { key, icon, name, label, command } (TimelinePanel.toolCatalog).
    required property var catalog
    /// The panel's tool.
    property string current: ""
    /// The tool the button shows (TimelinePanel remembers each group's last one).
    property string shown: group.tools[0]
    readonly property var shownTool: catalog[shown] ?? ({ key: "", icon: "", name: "", label: "", command: "" })
    readonly property bool hasMore: group.tools.length > 1
    // A press and hold opened the flyout: the release that follows selects nothing.
    property bool heldOpen: false

    signal toolChosen(string key)

    function openFlyout() {
        flyout.popup(control, 0, control.height + Theme.space(1))
    }

    objectName: "toolGroup_" + group.key
    compact: true
    checkable: true
    checked: group.tools.includes(current)
    iconName: shownTool.icon
    tooltip: hasMore ? qsTr("%1 (right-click or hold for the other tools)").arg(shownTool.label) : shownTool.label
    command: shownTool.command
    onClicked: {
        checked = Qt.binding(() => control.group.tools.includes(control.current))
        if (control.heldOpen)
            control.heldOpen = false
        else
            control.toolChosen(control.shown)
    }
    onPressed: {
        control.heldOpen = false
        if (control.hasMore)
            hold.restart()
    }
    onReleased: hold.stop()
    onCanceled: hold.stop()
    Keys.onPressed: event => {
        if (control.hasMore && (event.key === Qt.Key_Down || event.key === Qt.Key_Menu)) {
            control.openFlyout()
            event.accepted = true
        }
    }

    // Qt.styleHints, untyped: qmllint knows it only as a QObject.
    readonly property var styleHints: Qt.styleHints

    Timer {
        id: hold
        interval: control.styleHints.mousePressAndHoldInterval // the platform's press and hold
        onTriggered: {
            control.heldOpen = true
            control.openFlyout()
        }
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        enabled: control.hasMore
        onTapped: control.openFlyout()
    }

    // The corner mark of a group with more tools: a small right triangle in the bottom-right corner.
    Item {
        visible: control.hasMore
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.space(1.25) // inside a round button's curve
        width: Theme.space(1.25)
        height: width
        clip: true

        Rectangle {
            width: parent.width * Math.SQRT2
            height: width
            x: parent.width - width / 2
            y: parent.height - height / 2
            rotation: 45
            antialiasing: true
            color: control.checked ? Theme.accentOnFill : control.hovered ? Theme.textPrimary : Theme.textTertiary
        }
    }

    ToolFlyout {
        id: flyout
        objectName: "toolFlyout_" + control.group.key
        tools: control.group.tools.map(key => control.catalog[key]).filter(t => t !== undefined)
        current: control.current
        onChosen: key => control.toolChosen(key)
    }
}
