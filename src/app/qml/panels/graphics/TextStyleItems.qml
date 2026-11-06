pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// The project's Text Styles as cards (0.2: Premiere lists Text Styles in the Project panel; 0.7: they are Essential
// Graphics' Styles tab): a card per style previewing its font, size and colour and how many text layers follow it. Click
// applies it to every text layer of the graphic clips selected on the timeline (one undo step), and so does dragging it
// onto a graphic clip on the timeline; its menu renames, duplicates, reorders or deletes it (the order is the order of the
// cards and of the editor's Ctrl+Alt+1..9 keys). Styles are made from a text layer in Essential Graphics (Create Text
// Style).
ColumnLayout {
    id: root
    objectName: "textStyleItems"

    readonly property var items: GraphicTools.textStyles.length >= 0 ? GraphicTools.textStyleItems() : []
    property string renaming: ""

    spacing: Theme.space(1.5)

    SectionHeader {
        text: qsTr("Text styles")
    }
    Text {
        Layout.fillWidth: true
        visible: root.items.length === 0
        wrapMode: Text.Wrap
        text: qsTr("No text styles yet: select a text layer in Essential Graphics and choose Create Text Style.")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    Flow {
        Layout.fillWidth: true
        spacing: Theme.space(1.5)
        Repeater {
            model: root.items
            delegate: Rectangle {
                id: card
                required property var modelData
                required property int index
                objectName: "textStyleItem_" + index
                width: Theme.space(34)
                height: Theme.space(20)
                radius: Theme.radiusField
                color: cardMouse.containsMouse ? Theme.raised : Theme.sunken
                border.width: Theme.hairline
                border.color: Theme.border
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Text style %1, used by %n layer(s)", "", card.modelData.uses).arg(card.modelData.name)
                Accessible.onPressAction: GraphicTools.applyTextStyleToSelection(card.modelData.name)

                // The style's own colour on a stage that contrasts with it (QA: a dark style such as "Counter" vanished on a dark
                // card): light text on a dark chip, dark text on a light one, in the dark and the light theme alike.
                Rectangle {
                    id: sample
                    objectName: "textStyleSample_" + card.index
                    readonly property color fill: Qt.color(card.modelData.color)
                    readonly property real luma: fill.r * 0.2126 + fill.g * 0.7152 + fill.b * 0.0722
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.space(1)
                    height: sampleText.implicitHeight + Theme.space(1)
                    radius: Theme.radiusField
                    color: sample.luma >= 0.5 ? Qt.rgba(0.11, 0.11, 0.13, 1) : Qt.rgba(0.94, 0.94, 0.96, 1)
                    border.width: Theme.hairline
                    border.color: Theme.border
                    Text {
                        id: sampleText
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: Theme.space(1)
                        anchors.rightMargin: Theme.space(1)
                        text: qsTr("Aa")
                        elide: Text.ElideRight
                        color: card.modelData.color
                        font.family: card.modelData.fontFamily !== "" ? card.modelData.fontFamily : Theme.fontSans
                        font.weight: card.modelData.fontWeight
                        font.italic: card.modelData.italic
                        font.pixelSize: Theme.textXl
                    }
                }
                Text {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: Theme.space(1.5)
                    text: card.modelData.uses > 0 ? qsTr("%1 · %n layer(s)", "", card.modelData.uses).arg(card.modelData.name)
                                                  : card.modelData.name
                    elide: Text.ElideRight
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                MouseArea {
                    id: cardMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    cursorShape: Qt.PointingHandCursor
                    onClicked: mouse => {
                        if (mouse.button === Qt.RightButton)
                            styleMenu.popup(card, mouse.x, mouse.y)
                        else
                            GraphicTools.applyTextStyleToSelection(card.modelData.name)
                    }
                }
                // Drag onto a graphic clip on the timeline (TimelinePanel's effect drop area): its text layers take the style.
                Item {
                    id: dragSource
                    objectName: "textStyleDrag_" + card.index
                    anchors.fill: parent
                    Drag.active: dragHandler.active
                    Drag.dragType: Drag.Automatic
                    Drag.supportedActions: Qt.CopyAction
                    Drag.mimeData: { "application/x-wabcut-text-style": card.modelData.name }

                    DragHandler {
                        id: dragHandler
                        target: null
                    }
                }
                Tooltip {
                    visible: cardMouse.containsMouse && !dragHandler.active
                    text: qsTr("Apply %1 to the selected graphics, or drag it onto a graphic clip").arg(card.modelData.name)
                }
                ContextMenu {
                    id: styleMenu
                    ContextMenuItem {
                        objectName: "textStyleApply"
                        text: qsTr("Apply to Selected Graphics")
                        iconName: "type"
                        onTriggered: GraphicTools.applyTextStyleToSelection(card.modelData.name)
                    }
                    ContextMenuItem {
                        objectName: "textStyleRename"
                        text: qsTr("Rename…")
                        iconName: "pencil"
                        onTriggered: names.ask(qsTr("Rename Text Style"), card.modelData.name, qsTr("Rename"),
                                               text => GraphicTools.renameTextStyle(card.modelData.name, text))
                    }
                    ContextMenuItem {
                        objectName: "textStyleDuplicate"
                        text: qsTr("Duplicate")
                        iconName: "copy"
                        onTriggered: GraphicTools.duplicateTextStyle(card.modelData.name)
                    }
                    // 0.7 (card #81): the order of the list is the order here (and the Ctrl+Alt+1..9 order of the editor).
                    ContextMenuItem {
                        objectName: "textStyleMoveEarlier"
                        text: qsTr("Move Earlier")
                        iconName: "chevron-up"
                        enabled: card.index > 0
                        onTriggered: GraphicTools.moveTextStyle(card.modelData.name, card.index - 1)
                    }
                    ContextMenuItem {
                        objectName: "textStyleMoveLater"
                        text: qsTr("Move Later")
                        iconName: "chevron-down"
                        enabled: card.index < root.items.length - 1
                        onTriggered: GraphicTools.moveTextStyle(card.modelData.name, card.index + 1)
                    }
                    ContextMenuSeparator {}
                    ContextMenuItem {
                        objectName: "textStyleDelete"
                        text: qsTr("Delete")
                        iconName: "trash-2"
                        onTriggered: GraphicTools.removeTextStyle(card.modelData.name)
                    }
                }
            }
        }
    }
    Text {
        Layout.fillWidth: true
        visible: GraphicTools.message !== ""
        wrapMode: Text.Wrap
        text: GraphicTools.message
        color: Theme.warning
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    EffectNameDialog {
        id: names
    }
}
