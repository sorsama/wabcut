pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// The Timeline list (M5j, accessibility): the sequence as a list of tracks and their clips, for the keyboard and
// screen readers. Each track is a heading (V3 … V1, A1 … An); each clip row shows its name, start, duration, effects
// and state, and its spoken name says all of it (ProjectController.clipList). The list is one tab stop:
//   ↑ / ↓ move between clips (Home / End: first / last), Enter or Space selects the clip and moves the playhead to it,
//   Ctrl/⌘+Space adds it to the selection, Delete removes the selected clips, Esc goes back to the lanes.
FocusScope {
    id: root

    objectName: "timelineList"
    signal closeRequested()
    signal revealRequested(string clipId)

    function focusList() {
        list.forceActiveFocus(Qt.TabFocusReason)
        if (list.currentIndex < 0 && list.count > 0)
            list.currentIndex = Math.max(0, ProjectController.clipList.currentRow)
    }

    function choose(row, add) {
        const clip = ProjectController.clipList.clipIdAt(row)
        if (clip === "")
            return
        ProjectController.selectClip(clip, add ? ProjectController.Toggle : ProjectController.Replace)
        if (!add)
            root.revealRequested(clip)
    }

    // Design language v3: the list sits in a dark well like the timeline's lanes.
    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusCard
        color: Theme.sunken
    }

    // Presses anywhere on the list (its heading strip, its margins) stay here: the lanes and track headers under it
    // are hidden and must not move or select clips (review P2-42). The rows above take their own presses.
    MouseArea {
        objectName: "timelineListBlocker"
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onPressed: list.forceActiveFocus(Qt.MouseFocusReason)
        onWheel: wheel => wheel.accepted = true
    }

    // Column headings.
    Row {
        id: heading
        x: Theme.space(3)
        y: Theme.space(1.5)
        spacing: 0
        component Heading: Text {
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            font.weight: Font.Medium
        }
        Heading { width: list.nameWidth; text: qsTr("Clip") }
        Heading { width: list.timeWidth; text: qsTr("Start") }
        Heading { width: list.timeWidth; text: qsTr("Duration") }
        Heading { text: qsTr("Effects") }
    }

    ListView {
        id: list
        objectName: "timelineListView"
        anchors.fill: parent
        anchors.topMargin: heading.y + heading.height + Theme.space(1.5)
        anchors.leftMargin: Theme.space(1)
        anchors.rightMargin: Theme.space(1)
        anchors.bottomMargin: Theme.space(1)
        clip: true
        focus: true
        activeFocusOnTab: true
        boundsBehavior: Flickable.StopAtBounds
        keyNavigationEnabled: true
        highlightMoveDuration: 0
        model: ProjectController.clipList
        currentIndex: -1
        ScrollBar.vertical: ThinScrollBar {}

        readonly property real nameWidth: Math.max(Theme.space(40), width * 0.32)
        readonly property real timeWidth: Theme.space(28)

        Accessible.role: Accessible.List
        Accessible.name: qsTr("Timeline list: %n clip(s)", "", count)
        Accessible.description: qsTr("Up and Down move between clips; Enter selects one; Delete removes the selection")

        // Which keys the list owns: unmodified ones, and Ctrl/⌘+Space (add to the selection). Modified keys stay
        // shortcuts: Shift+Delete is Ripple Delete (review P2-41).
        function owns(event: KeyEvent): bool {
            const mods = event.modifiers & (Qt.ShiftModifier | Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)
            if (event.key === Qt.Key_Space)
                return (mods & ~(Qt.ControlModifier | Qt.MetaModifier)) === 0
            return mods === 0 && [Qt.Key_Return, Qt.Key_Enter, Qt.Key_Home, Qt.Key_End, Qt.Key_Delete,
                                  Qt.Key_Backspace, Qt.Key_Up, Qt.Key_Down, Qt.Key_Escape].includes(event.key)
        }

        Keys.onPressed: event => {
            if (!list.owns(event))
                return
            const add = (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) !== 0
            switch (event.key) {
            case Qt.Key_Return:
            case Qt.Key_Enter:
                root.choose(currentIndex, false)
                event.accepted = true
                break
            case Qt.Key_Space:
                root.choose(currentIndex, add)
                event.accepted = true
                break
            case Qt.Key_Home:
                currentIndex = count > 0 ? 0 : -1
                event.accepted = true
                break
            case Qt.Key_End:
                currentIndex = count - 1
                event.accepted = true
                break
            case Qt.Key_Delete:
            case Qt.Key_Backspace:
                ProjectController.deleteSelection()
                event.accepted = true
                break
            case Qt.Key_Escape:
                root.closeRequested()
                event.accepted = true
                break
            }
        }
        // The list keeps the keys it owns from the global shortcuts while it has focus.
        Keys.onShortcutOverride: event => event.accepted = list.owns(event)

        section.property: "trackName"
        section.delegate: Item {
            id: trackHeading
            required property string section
            width: ListView.view.width
            height: Theme.controlHeightSmall
            Accessible.role: Accessible.Heading
            Accessible.name: qsTr("Track %1").arg(trackHeading.section)
            SliceMark {
                id: mark
                x: Theme.space(2)
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.space(2)
                height: Theme.space(3)
                color: Theme.accentStroke
            }
            Text {
                anchors.left: mark.right
                anchors.leftMargin: Theme.space(2)
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Track %1").arg(trackHeading.section)
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                font.weight: Font.DemiBold
            }
        }

        delegate: ClipRow {}

        FocusRing {
            anchors.margins: 0
            targetRadius: Theme.radiusCard
            visible: list.activeFocus
        }
    }

    component ClipRow: Rectangle {
        id: row

        required property int index
        required property string clipId
        required property string name
        required property string startText
        required property string durationText
        required property string effectsText
        required property string accessibleName
        required property bool selected
        required property bool clipEnabled
        required property bool locked

        objectName: "timelineListRow_" + clipId
        width: ListView.view.width
        height: Theme.controlHeight
        radius: Theme.radiusField
        color: row.selected ? Theme.accentWash : ListView.isCurrentItem ? Theme.hover : "transparent"
        border.width: ListView.isCurrentItem && ListView.view.activeFocus ? Theme.hairline : 0
        border.color: Theme.accentStroke

        Accessible.role: Accessible.ListItem
        Accessible.name: row.accessibleName
        Accessible.selected: row.selected
        Accessible.focusable: true
        Accessible.focused: ListView.isCurrentItem && ListView.view.activeFocus

        Row {
            x: Theme.space(2)
            anchors.verticalCenter: parent.verticalCenter
            spacing: 0
            Text {
                width: list.nameWidth
                text: row.name
                elide: Text.ElideRight
                color: row.clipEnabled ? Theme.textPrimary : Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: row.selected ? Font.DemiBold : Font.Normal
            }
            Text {
                width: list.timeWidth
                text: row.startText
                color: Theme.textSecondary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textSm
            }
            Text {
                width: list.timeWidth
                text: row.durationText
                color: Theme.textSecondary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textSm
            }
            Text {
                width: Math.max(0, list.width - list.nameWidth - 2 * list.timeWidth - Theme.space(6))
                text: row.effectsText !== "" ? row.effectsText : qsTr("No effects")
                elide: Text.ElideRight
                color: row.effectsText !== "" ? Theme.textSecondary : Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: mouse => {
                list.currentIndex = row.index
                list.forceActiveFocus(Qt.MouseFocusReason)
                root.choose(row.index, (mouse.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) !== 0)
            }
        }
    }
}
