pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// History (P.2, E5): Premiere's Window > History - every undo step by name, the project as it was opened first; the
// current state is marked and the steps after it (undone, still redoable) are dimmed. A click goes back (or forward)
// to that state, as many undos or redos at once.
// UX rework: Premiere's History panel keys - with the panel focused, Left / Right step one state back / forward
// (history.stepBackward / stepForward in the keymap's "history" scope).
// P1 round (#4): a dockable panel of the catalog ("history", Main.qml's LazyPanel; Window > History opens it as a tab of
// the active group and closes it again), with Premiere's Delete (Backspace: the current step and every step after it)
// and the panel menu's Clear History; both ask first (EditingHost), as they cannot be undone.
Panel {
    id: panel
    objectName: "historyPanel"

    /// Shown in the workspace now (Main.qml binds it).
    property bool opened: false

    title: qsTr("History")

    // Focus target for Window > History: the steps (the "history" key scope).
    function focusContent() {
        steps.forceActiveFocus(Qt.OtherFocusReason)
    }

    onOpenedChanged: {
        if (panel.opened)
            Qt.callLater(() => steps.positionViewAtEnd())
    }

    trailing: [
        IconButton {
            id: menuButton
            objectName: "historyMenuButton"
            compact: true
            iconName: "ellipsis"
            tooltip: qsTr("History menu")
            onClicked: menu.popup(menuButton, 0, menuButton.height + Theme.space(1))
        }
    ]

    HistoryPanelMenu {
        id: menu
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(2)
        Accessible.role: Accessible.Pane
        Accessible.name: qsTr("History")
        // The keymap's History panel keys (Left / Right step through the states, Backspace deletes) while focus is in
        // here.
        KeymapScope.scope: "history"

        ListView {
            id: steps
            objectName: "historySteps"
            Layout.fillWidth: true
            Layout.fillHeight: true
            focus: true
            activeFocusOnTab: true
            Accessible.role: Accessible.List
            Accessible.name: qsTr("History steps")
            clip: true
            model: Editing.historySteps
            spacing: 1
            currentIndex: -1
            ScrollBar.vertical: ThinScrollBar {}
            onCountChanged: Qt.callLater(() => steps.positionViewAtEnd())

            delegate: ItemDelegate {
                id: row
                required property var modelData
                required property int index
                width: steps.width
                height: Theme.controlHeightSmall
                topPadding: 0
                bottomPadding: 0
                leftPadding: Theme.space(2)
                rightPadding: Theme.space(2)
                hoverEnabled: true
                Accessible.name: row.modelData.label
                Accessible.selected: row.modelData.state === "current"
                onClicked: Editing.goToHistoryStep(row.index)

                background: Rectangle {
                    radius: Theme.radiusControl
                    color: row.modelData.state === "current" ? Theme.accentFill
                                                            : row.hovered ? Theme.hover : "transparent"
                }
                contentItem: RowLayout {
                    spacing: Theme.space(2)
                    Icon {
                        name: row.index === 0 ? "folder-open" : row.modelData.state === "undone" ? "redo-2" : "undo-2"
                        size: Theme.iconSm
                        color: row.modelData.state === "current" ? Theme.accentOnFill : Theme.textTertiary
                    }
                    Text {
                        Layout.fillWidth: true
                        text: row.modelData.label
                        elide: Text.ElideRight
                        color: row.modelData.state === "current" ? Theme.accentOnFill
                                                                : row.modelData.state === "undone" ? Theme.textDisabled
                                                                                                    : Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                    }
                }
            }
        }
    }
}
