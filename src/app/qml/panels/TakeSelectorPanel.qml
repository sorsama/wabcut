pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// ● TAKE SELECTOR (0.6, card #74; Final Cut's Audition window, Resolve's Take Selector): the takes of an audition - the
// selected clip when it is one, else the audition under the playhead - in take order, each with its first frame, name,
// source, length and how far it is short of the audition's range; the active one is marked. Play auditions a take in
// place (the audition's range plays once and the take that was active comes back when it stops, unless Use This Take is
// chosen meanwhile); Use This Take (the check button, Enter, a double-click) makes it the active take without moving the
// playhead. F2 or the row menu renames a take, Alt+Up / Alt+Down reorder, Delete removes. The header makes a new
// audition of the selected clips, adds the other selected clips or the Source Monitor's clip as takes, duplicates the
// active take and flattens the audition. Every change is one undo step (Auditions).
Panel {
    id: root

    objectName: "takeSelectorPanel"
    title: qsTr("Take Selector")

    readonly property bool hasAudition: Auditions.clip !== "" && Auditions.count > 1

    function focusContent() {
        if (list.count > 0)
            list.forceActiveFocus(Qt.TabFocusReason)
    }
    /// The take menu for row `row` (a right click at `point`, or the Menu key).
    function openTakeMenu(row: Item, point: point, index: int) {
        takeMenu.openAt(row, point, index)
    }
    /// Opens row `index`'s name field.
    function rename(index: int) {
        list.currentIndex = index
        list.renaming = index
    }

    leading: [
        IconButton {
            objectName: "takeSelectorNew"
            compact: true
            iconName: "plus"
            tooltip: qsTr("New Audition from the selected clips")
            command: "audition.create"
            enabled: Auditions.canCreate
            onClicked: Auditions.createFromSelection()
        },
        IconButton {
            objectName: "takeSelectorAddSelected"
            compact: true
            iconName: "folder-plus"
            tooltip: qsTr("Add the selected clips as takes")
            command: "audition.addSelected"
            enabled: Auditions.canAddSelected
            onClicked: Auditions.addSelected()
        },
        IconButton {
            objectName: "takeSelectorAddSource"
            compact: true
            iconName: "film"
            tooltip: qsTr("Add the Source Monitor's clip as a take")
            command: "audition.addSource"
            enabled: Auditions.canAddSource
            onClicked: Auditions.addSource()
        }
    ]
    trailing: [
        IconButton {
            objectName: "takeSelectorDuplicate"
            compact: true
            iconName: "copy"
            tooltip: qsTr("Duplicate the active take (to try other effects on it)")
            command: "audition.duplicateTake"
            enabled: root.hasAudition
            onClicked: Auditions.duplicateTake(-1)
        },
        IconButton {
            objectName: "takeSelectorFlatten"
            compact: true
            iconName: "minimize-2"
            tooltip: qsTr("Flatten: keep the active take only")
            command: "audition.flatten"
            enabled: root.hasAudition
            onClicked: Auditions.flatten()
        },
        IconButton {
            objectName: "takeSelectorOverlay"
            compact: true
            iconName: "layers"
            checkable: true
            checked: Auditions.overlayShown
            tooltip: qsTr("Show the takes in the Program Monitor")
            command: "audition.toggleOverlay"
            onClicked: {
                checked = Qt.binding(() => Auditions.overlayShown)
                Auditions.overlayShown = !Auditions.overlayShown
            }
        }
    ]

    // The audition: its active take's name, "Take 2 of 4", what it holds and where it is.
    Column {
        id: summary
        objectName: "takeSelectorSummary"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        visible: root.hasAudition
        height: visible ? implicitHeight : 0
        spacing: Theme.space(0.5)

        Row {
            width: parent.width
            spacing: Theme.space(2)
            Text {
                width: Math.min(implicitWidth, parent.width - badge.width - Theme.space(2))
                anchors.verticalCenter: parent.verticalCenter
                text: Auditions.name
                elide: Text.ElideRight
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Font.DemiBold
            }
            Rectangle {
                id: badge
                objectName: "takeSelectorBadge"
                anchors.verticalCenter: parent.verticalCenter
                width: badgeText.implicitWidth + Theme.space(3)
                height: badgeText.implicitHeight + Theme.space(1)
                radius: height / 2
                color: Theme.accentFill
                Text {
                    id: badgeText
                    anchors.centerIn: parent
                    text: Auditions.badge
                    color: Theme.accentOnFill
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                    font.weight: Font.DemiBold
                }
            }
        }
        Text {
            width: parent.width
            text: qsTr("%1 · %2").arg(Auditions.kindText).arg(Auditions.rangeText)
            elide: Text.ElideRight
            color: Theme.textTertiary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
        }
    }

    ListView {
        id: list
        objectName: "takeSelectorList"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: summary.bottom
        anchors.bottom: parent.bottom
        anchors.topMargin: root.hasAudition ? Theme.space(2) : 0
        clip: true
        spacing: Theme.space(1)
        model: root.hasAudition ? Auditions.takes : []
        currentIndex: Auditions.activeIndex
        boundsBehavior: Flickable.StopAtBounds
        keyNavigationEnabled: true
        ScrollBar.vertical: ThinScrollBar {}
        Accessible.role: Accessible.List
        Accessible.name: qsTr("Takes")

        /// The row whose name field is open, or -1.
        property int renaming: -1

        delegate: AbstractButton {
            id: row

            required property var modelData
            required property int index

            readonly property bool active: modelData.active === true
            readonly property bool previewing: Auditions.previewing === modelData.index

            objectName: "takeRow_" + modelData.index
            width: list.width
            height: Theme.space(14)
            focusPolicy: Qt.StrongFocus
            hoverEnabled: true
            onClicked: {
                list.currentIndex = row.index
                row.forceActiveFocus(Qt.MouseFocusReason)
            }
            onDoubleClicked: Auditions.useTake(row.modelData.index)
            Keys.onReturnPressed: Auditions.useTake(row.modelData.index)
            Keys.onEnterPressed: Auditions.useTake(row.modelData.index)
            Keys.onDeletePressed: Auditions.removeTake(row.modelData.index)
            Keys.onPressed: event => {
                if (event.key === Qt.Key_F2) {
                    root.rename(row.index)
                    event.accepted = true
                } else if ((event.modifiers & Qt.AltModifier) && (event.key === Qt.Key_Up || event.key === Qt.Key_Down)) {
                    const to = row.index + (event.key === Qt.Key_Up ? -1 : 1)
                    if (to >= 0 && to < list.count)
                        Auditions.moveTake(row.index, to)
                    event.accepted = true
                } else if (event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
                    root.openTakeMenu(row, Qt.point(Theme.space(4), row.height), row.index)
                    event.accepted = true
                }
            }
            Accessible.role: Accessible.ListItem
            Accessible.name: row.active ? qsTr("Take %1, %2, active").arg(row.index + 1).arg(row.modelData.name)
                                        : qsTr("Take %1, %2").arg(row.index + 1).arg(row.modelData.name)
            Accessible.description: row.modelData.isShort ? qsTr("%1, %2").arg(row.modelData.durationText).arg(row.modelData.shortText)
                                                           : row.modelData.durationText

            contentItem: Item {
                // The take's first frame (a glyph for sound, a sequence or a take without a picture).
                Rectangle {
                    id: thumb
                    objectName: "takeThumbnail"
                    x: Theme.space(2)
                    anchors.verticalCenter: parent.verticalCenter
                    height: parent.height - Theme.space(3)
                    width: Math.round(height * 16 / 9)
                    radius: Theme.radiusField
                    color: Theme.sunken
                    border.width: row.active ? 2 : Theme.hairline
                    border.color: row.active ? Theme.accentStroke : Theme.border
                    clip: true
                    Image {
                        id: picture
                        anchors.fill: parent
                        anchors.margins: row.active ? 2 : 1
                        source: row.modelData.thumbnail
                        asynchronous: true
                        fillMode: Image.PreserveAspectCrop
                        visible: status === Image.Ready
                    }
                    Icon {
                        anchors.centerIn: parent
                        visible: !picture.visible
                        name: row.modelData.kind === "audio" ? "audio-lines"
                            : row.modelData.kind === "nested" || row.modelData.kind === "multicam" ? "layers"
                            : row.modelData.kind === "graphic" ? "type" : "film"
                        size: Theme.iconMd
                        color: Theme.textTertiary
                    }
                    // The take's number, as on the Program Monitor's cards.
                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.margins: Theme.space(1.5)
                        width: number.implicitWidth + Theme.space(2)
                        height: number.implicitHeight + Theme.space(0.5)
                        radius: height / 2
                        color: row.active ? Theme.accentFill : Theme.raised
                        Text {
                            id: number
                            anchors.centerIn: parent
                            text: row.index + 1
                            color: row.active ? Theme.accentOnFill : Theme.textPrimary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                            font.weight: Font.DemiBold
                        }
                    }
                }
                Column {
                    anchors.left: thumb.right
                    anchors.leftMargin: Theme.space(3)
                    anchors.right: buttons.left
                    anchors.rightMargin: Theme.space(2)
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.space(0.5)
                    Item {
                        width: parent.width
                        height: Math.max(nameText.implicitHeight, nameField.visible ? nameField.height : 0)
                        Text {
                            id: nameText
                            width: parent.width
                            visible: list.renaming !== row.index
                            text: row.modelData.name !== "" ? row.modelData.name : qsTr("Take %1").arg(row.index + 1)
                            elide: Text.ElideRight
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: row.active ? Font.DemiBold : Font.Normal
                        }
                        FieldInput {
                            id: nameField
                            objectName: "takeNameField"
                            width: parent.width
                            visible: list.renaming === row.index
                            text: row.modelData.name
                            Accessible.name: qsTr("Take name")
                            onVisibleChanged: {
                                if (visible) {
                                    nameField.text = row.modelData.name
                                    nameField.forceActiveFocus(Qt.OtherFocusReason)
                                    nameField.selectAll()
                                }
                            }
                            onAccepted: {
                                const name = nameField.text
                                list.renaming = -1
                                Auditions.renameTake(row.modelData.index, name)
                                row.forceActiveFocus(Qt.OtherFocusReason)
                            }
                            Keys.onEscapePressed: {
                                list.renaming = -1
                                row.forceActiveFocus(Qt.OtherFocusReason)
                            }
                        }
                    }
                    Text {
                        width: parent.width
                        text: row.modelData.effects <= 0 ? row.modelData.source
                              : row.modelData.effects === 1
                                ? qsTr("%1 · 1 effect").arg(row.modelData.source)
                                : qsTr("%1 · %2 effects").arg(row.modelData.source).arg(row.modelData.effects)
                        elide: Text.ElideRight
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    Row {
                        spacing: Theme.space(2)
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: row.modelData.durationText
                            color: row.active ? Theme.accentStroke : Theme.textTertiary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                        Text {
                            objectName: "takeShortText"
                            anchors.verticalCenter: parent.verticalCenter
                            visible: row.modelData.isShort === true
                            text: row.modelData.shortText
                            color: Theme.warningText
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: row.active
                            text: qsTr("Active")
                            color: Theme.accentStroke
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                            font.weight: Font.DemiBold
                        }
                    }
                }
                Row {
                    id: buttons
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.space(2)
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.space(1)
                    IconButton {
                        objectName: "takePlay_" + row.index
                        compact: true
                        iconName: row.previewing ? "pause" : "play"
                        tooltip: row.previewing ? qsTr("Stop auditioning this take")
                                                : qsTr("Play: audition this take in place")
                        Accessible.name: row.previewing ? qsTr("Stop take %1: %2").arg(row.index + 1).arg(row.modelData.name)
                                                        : qsTr("Play take %1: %2").arg(row.index + 1).arg(row.modelData.name)
                        onClicked: row.previewing ? Auditions.stopPreview() : Auditions.previewTake(row.modelData.index)
                    }
                    IconButton {
                        objectName: "takeUse_" + row.index
                        compact: true
                        iconName: "check"
                        checkable: true
                        checked: row.active
                        enabled: !row.active || row.previewing
                        tooltip: qsTr("Use this take")
                        Accessible.name: qsTr("Use take %1").arg(row.index + 1)
                        onClicked: {
                            checked = Qt.binding(() => row.active)
                            Auditions.useTake(row.modelData.index)
                        }
                    }
                }
            }

            background: Rectangle {
                radius: Theme.radiusField
                color: row.down ? Theme.raised
                     : row.previewing ? Theme.accentWash
                     : row.hovered ? Theme.hover
                     : list.currentIndex === row.index ? Theme.sunken : "transparent"
                Behavior on color {
                    enabled: !Theme.reduceMotion
                    ColorAnimation { duration: Theme.durFast }
                }

                FocusRing {
                    anchors.margins: 0
                    targetRadius: Theme.radiusField
                    visible: row.visualFocus
                }
            }

            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: eventPoint => root.openTakeMenu(row, eventPoint.position, row.index)
            }
        }
    }

    EmptyState {
        objectName: "takeSelectorEmptyState"
        anchors.centerIn: parent
        width: parent.width - Theme.space(8)
        visible: !root.hasAudition
        illustration: "timeline"
        text: qsTr("No audition here. Select clips (other takes of a shot, line readings or versions of a graphic) and make them an audition: they share one place in the edit and you pick the take that plays.")
        actionText: qsTr("New Audition")
        actionIcon: "plus"
        onActionTriggered: Auditions.createFromSelection()
    }

    TakeContextMenu {
        id: takeMenu
        onRenameRequested: index => root.rename(index)
    }
}
