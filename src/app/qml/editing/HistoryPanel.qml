pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// History (P.2, E5): Premiere's Window > History - every undo step by name, the project as it was opened first; the
// current state is marked and the steps after it (undone, still redoable) are dimmed. A click goes back (or forward)
// to that state, as many undos or redos at once. A floating panel that stays open while you edit (not modal).
Popup {
    id: panel
    objectName: "historyPanel"

    parent: Overlay.overlay
    x: parent ? parent.width - width - Theme.space(6) : 0
    y: Theme.space(18)
    width: Theme.space(80)
    height: Math.min(parent ? parent.height - Theme.space(30) : Theme.space(150), Theme.space(150))
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape
    padding: Theme.pad

    background: Rectangle {
        radius: Theme.radiusPanel
        color: Theme.raised
        border.width: Theme.hairline
        border.color: Theme.borderStrong
    }

    contentItem: ColumnLayout {
        spacing: Theme.space(2)
        Accessible.role: Accessible.Pane
        Accessible.name: qsTr("History")

        RowLayout {
            Layout.fillWidth: true
            SectionHeader {
                Layout.fillWidth: true
                text: qsTr("History")
            }
            IconButton {
                objectName: "historyClose"
                iconName: "x"
                tooltip: qsTr("Close")
                onClicked: panel.close()
            }
        }

        ListView {
            id: steps
            objectName: "historySteps"
            Layout.fillWidth: true
            Layout.fillHeight: true
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
                hoverEnabled: true
                Accessible.name: row.modelData.label
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
