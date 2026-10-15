pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// ● EFFECTS: the effect library (EffectRegistry descriptors and transitions), searchable, grouped by kind with a
// category icon per row. Drag an effect onto a clip in the timeline or onto Effect Controls to add it, drag a
// transition onto an edit point (mime application/x-wabcut-effect = the effect id). Enter or a double-click adds an
// effect to the selected clip. Under them, the audio plugins this computer has (M5m, A.6: PluginBrowser, grouped by
// vendor or category, the same search, the same drag).
Panel {
    id: root
    objectName: "effectsPanel"

    KeymapScope.scope: "effects"

    title: qsTr("Effects")

    property string query: ""
    readonly property var groups: ProjectController.effectControls.library(query)
    property string status: ""

    // Focus target for the rail's "Effects" shortcut.
    function focusContent() {
        search.forceActiveFocus(Qt.TabFocusReason)
    }

    function applyToSelection(item) {
        if (item.kind === "videoTransition" || item.kind === "audioTransition") {
            status = qsTr("Drag %1 onto an edit point in the timeline").arg(item.name)
            return
        }
        const error = ProjectController.effectControls.addEffect(item.id)
        status = error === "" ? qsTr("Added %1").arg(item.name) : error
    }

    TextField {
        id: search
        objectName: "effectsSearch"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        implicitHeight: Theme.controlHeightSmall + Theme.space(1)
        leftPadding: Theme.space(8)
        rightPadding: Theme.space(3)
        placeholderText: qsTr("Search effects and transitions")
        placeholderTextColor: Theme.textTertiary
        color: Theme.textPrimary
        selectionColor: Theme.accentFill
        selectedTextColor: Theme.accentOnFill
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        selectByMouse: true
        Accessible.name: qsTr("Search effects and transitions")
        onTextChanged: root.query = text
        Keys.onEscapePressed: text = ""

        background: Rectangle {
            radius: height / 2
            color: Theme.raised
            border.width: search.activeFocus ? Theme.focusRingWidth : 0
            border.color: Theme.focus
        }

        Icon {
            anchors.left: parent.left
            anchors.leftMargin: Theme.space(3)
            anchors.verticalCenter: parent.verticalCenter
            name: "search"
            size: Theme.iconSm
            color: Theme.textTertiary
        }
    }

    Flickable {
        id: list
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: search.bottom
        anchors.bottom: statusText.visible ? statusText.top : parent.bottom
        anchors.topMargin: Theme.space(3)
        clip: true
        contentHeight: column.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        activeFocusOnTab: false
        ScrollBar.vertical: ThinScrollBar {}

        Column {
            id: column
            width: list.width
            spacing: Theme.space(3)

            Repeater {
                model: root.groups

                delegate: Column {
                    id: group
                    required property var modelData
                    width: column.width
                    spacing: Theme.space(1)

                    Row {
                        spacing: Theme.space(1.5)
                        Icon {
                            anchors.verticalCenter: parent.verticalCenter
                            name: group.modelData.icon
                            size: Theme.iconXs
                            color: Theme.textTertiary
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: group.modelData.title
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                            font.weight: Font.DemiBold
                            font.letterSpacing: Theme.labelTracking
                        }
                    }

                    Repeater {
                        model: group.modelData.items

                        delegate: AbstractButton {
                            id: item
                            required property var modelData

                            objectName: "effectItem_" + modelData.id
                            width: group.width
                            height: Theme.controlHeight
                            focusPolicy: Qt.StrongFocus
                            hoverEnabled: true
                            Accessible.role: Accessible.ListItem
                            Accessible.name: modelData.name
                            Accessible.description: modelData.description
                            onDoubleClicked: root.applyToSelection(modelData)
                            Keys.onReturnPressed: root.applyToSelection(modelData)
                            Keys.onEnterPressed: root.applyToSelection(modelData)

                            contentItem: Item {
                                Icon {
                                    id: itemIcon
                                    anchors.left: parent.left
                                    anchors.leftMargin: Theme.space(3)
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: item.modelData.icon
                                    size: Theme.iconSm
                                    color: item.hovered || dragSource.Drag.active ? Theme.accentStroke : Theme.textTertiary
                                }
                                Text {
                                    anchors.left: itemIcon.right
                                    anchors.leftMargin: Theme.space(2)
                                    anchors.right: category.left
                                    anchors.rightMargin: Theme.space(2)
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: item.modelData.name
                                    elide: Text.ElideRight
                                    color: Theme.textPrimary
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textMd
                                }
                                Text {
                                    id: category
                                    anchors.right: parent.right
                                    anchors.rightMargin: Theme.space(3)
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: item.modelData.category
                                    color: Theme.textTertiary
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textSm
                                }
                            }

                            background: Rectangle {
                                radius: Theme.radiusField
                                color: item.down ? Theme.raised : item.hovered ? Theme.hover : "transparent"
                                Behavior on color { ColorAnimation { duration: Theme.durFast } }

                                FocusRing {
                                    anchors.margins: 0
                                    targetRadius: Theme.radiusField
                                    visible: item.visualFocus
                                }
                            }

                            // Drag source: onto a clip or an edit point in the timeline, or onto Effect Controls.
                            Item {
                                id: dragSource
                                anchors.fill: parent
                                Drag.active: dragHandler.active
                                Drag.dragType: Drag.Automatic
                                Drag.supportedActions: Qt.CopyAction
                                Drag.mimeData: { "application/x-wabcut-effect": item.modelData.id }

                                DragHandler {
                                    id: dragHandler
                                    target: null
                                }
                            }

                            HoverHandler {
                                enabled: item.enabled
                                cursorShape: Qt.PointingHandCursor
                            }
                        }
                    }
                }
            }

            // Audio plugins (A.6): CLAP and VST3 effects, scanned by the helper.
            PluginBrowser {
                width: column.width
                query: root.query
                onStatus: message => root.status = message
            }

            EmptyState {
                objectName: "effectsNoMatch"
                width: column.width
                visible: root.groups.length === 0 && Plugins.plugins.length === 0
                illustration: "search"
                text: qsTr("No effect matches “%1”").arg(root.query)
            }
        }
    }

    Text {
        id: statusText
        objectName: "effectsStatus"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: root.status !== ""
        text: root.status
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
}
