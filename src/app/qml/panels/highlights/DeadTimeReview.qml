pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Remove dead time's review (0.5, bridge `Moments`; Premiere's Text-Based Editing > Pauses, for gameplay): a sheet over
// the Highlights panel (Moments.deadTimeReviewing; the Moments popover's Find dead time and the panel's scissors button
// open it). Sensitivity, Shortest stretch, Keep at each end and Only In to Out are remembered; Find dead time measures
// the game audio and the picture's motion off the GUI thread with its progress and Cancel, and lists each stretch: a
// thumbnail of its middle frame (image://wabcut-frames, loaded as rows scroll in, dropped when the sheet closes), its
// timecode, its length and a remove switch. An option changed after a find says so ("Options changed: Find again", the
// list dimmed, Remove waiting): nothing is measured again until Find. While the sheet is open the switched-on stretches are red Removal bands on
// the timeline (beside the transcript's own highlights). A click on a row moves the playhead to the stretch. Keyboard
// (the list): Up / Down move (and jump), K keeps, Delete removes (both go on to the next stretch), Space flips, Enter
// jumps. Remove takes out only the switched-on stretches (handles kept, every unlocked track) in one undo step.
Rectangle {
    id: root
    objectName: "deadTimeReview"

    color: Theme.panel

    readonly property var dead: Moments.deadTime
    readonly property var options: Moments.deadTimeOptions
    readonly property bool busy: root.dead.busy === true
    readonly property bool found: root.dead.found === true
    readonly property int removeCount: root.dead.removeCount ?? 0
    /// An option changed after the find: the list is from the earlier settings (no automatic re-measure).
    readonly property bool staleOptions: root.dead.optionsChanged === true
    property string problem: ""
    /// The options show until stretches are listed; then they fold away (the sliders button opens them again), so the
    /// list has the room.
    property bool optionsOpen: false
    readonly property real rowHeight: Theme.controlHeight + Theme.space(2)
    readonly property bool optionsShown: list.count === 0 || root.optionsOpen

    function focusContent() {
        if (list.count > 0)
            list.forceActiveFocus(Qt.TabFocusReason)
        else
            findButton.forceActiveFocus(Qt.TabFocusReason)
    }
    function find() {
        root.problem = ""
        Moments.findDeadTime({})
    }
    function remove() {
        root.problem = Moments.removeDeadTime()
    }
    /// "1.0 s" with the locale's decimal point.
    function seconds(value: real): string {
        return qsTr("%1 s").arg(Number(value).toLocaleString(Qt.locale(), 'f', 1))
    }
    function positionCurrent() {
        if (Moments.deadTimeCurrent >= 0 && Moments.deadTimeCurrent < list.count)
            list.positionViewAtIndex(Moments.deadTimeCurrent, ListView.Contain)
    }

    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }
    component RowLabel: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    component ValueText: Text {
        Layout.preferredWidth: Theme.space(14)
        horizontalAlignment: Text.AlignRight
        color: Theme.textPrimary
        font.family: Theme.fontMono
        font.pixelSize: Theme.textSm
    }

    Connections {
        target: Moments
        function onDeadTimeCurrentChanged() {
            root.positionCurrent()
        }
        function onDeadTimeRangesChanged() {
            Qt.callLater(root.positionCurrent)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(2)

        // ---- The sheet's header ----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Icon {
                name: "scissors"
                size: Theme.iconSm
                color: Theme.accentStroke
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Dead time")
                elide: Text.ElideRight
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Font.DemiBold
            }
            IconButton {
                objectName: "deadTimeOptionsButton"
                visible: list.count > 0
                compact: true
                iconName: "sliders-horizontal"
                checkable: true
                checked: root.optionsOpen
                tooltip: qsTr("Sensitivity, shortest stretch, what is kept at each end, In to Out")
                onClicked: {
                    root.optionsOpen = !root.optionsOpen
                    checked = Qt.binding(() => root.optionsOpen)
                }
            }
            IconButton {
                objectName: "deadTimeClose"
                compact: true
                iconName: "x"
                tooltip: qsTr("Close the review (the timeline bands go)")
                onClicked: Moments.deadTimeReviewing = false
            }
        }
        // ---- Options (remembered) ----
        GridLayout {
            objectName: "deadTimeOptions"
            Layout.fillWidth: true
            visible: root.optionsShown
            columns: 3
            columnSpacing: Theme.space(2)
            rowSpacing: Theme.space(1)
            enabled: !root.busy

            RowLabel { text: qsTr("Sensitivity") }
            ValueSlider {
                objectName: "deadTimeSensitivity"
                Layout.fillWidth: true
                Accessible.name: qsTr("Sensitivity: higher finds more dead time")
                from: 0
                to: 100
                stepSize: 5
                value: Math.round(Number(root.options.sensitivity ?? 0.5) * 100)
                onMoved: Moments.setDeadTimeOption("sensitivity", value / 100)
            }
            ValueText {
                objectName: "deadTimeSensitivityValue"
                text: qsTr("%1 %").arg(Math.round(Number(root.options.sensitivity ?? 0.5) * 100))
            }

            RowLabel { text: qsTr("Shortest stretch") }
            ValueSlider {
                objectName: "deadTimeMinSeconds"
                Layout.fillWidth: true
                Accessible.name: qsTr("Shortest stretch that counts, in seconds")
                from: 1
                to: 30
                stepSize: 0.5
                value: Number(root.options.minSeconds ?? 3)
                onMoved: Moments.setDeadTimeOption("minSeconds", value)
            }
            ValueText {
                objectName: "deadTimeMinSecondsValue"
                text: root.seconds(Number(root.options.minSeconds ?? 3))
            }

            RowLabel { text: qsTr("Keep at each end") }
            NumberField {
                objectName: "deadTimeHandle"
                Layout.preferredWidth: Theme.numberFieldWidth
                Accessible.name: qsTr("Kept at each end of a stretch, in seconds")
                from: 0
                to: 5
                stepSize: 0.1
                dragStep: 0.05
                decimals: 1
                suffix: qsTr(" s")
                value: Number(root.options.handleSeconds ?? 0.3)
                onValueModified: value => Moments.setDeadTimeOption("handleSeconds", value)
            }
            Toggle {
                objectName: "deadTimeInOut"
                Layout.alignment: Qt.AlignRight
                text: qsTr("Only In to Out")
                tooltip: qsTr("Look only between the sequence's In and Out marks")
                checked: root.options.inOut === true
                onToggled: {
                    Moments.setDeadTimeOption("inOut", checked)
                    checked = Qt.binding(() => root.options.inOut === true)
                }
            }
        }

        // ---- Find, with its progress and Cancel ----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            PillButton {
                id: findButton
                objectName: "deadTimeFindButton"
                Layout.fillWidth: true
                variant: root.found && !root.staleOptions ? "secondary" : "primary"
                iconName: root.staleOptions ? "rotate-cw" : "search"
                text: root.busy ? qsTr("Measuring…")
                      : root.staleOptions ? qsTr("Options changed: Find again")
                      : root.found || root.dead.searched === true ? qsTr("Find again") : qsTr("Find dead time")
                tooltip: root.staleOptions ? qsTr("The list is from the earlier settings: measure again with these")
                                             : qsTr("Measures the game audio and the picture's motion with these settings")
                enabled: !root.busy && Moments.gamingEdits.deadTime === true
                onClicked: root.find()
            }
            PillButton {
                objectName: "deadTimeCancelButton"
                visible: root.busy
                compact: true
                text: qsTr("Cancel")
                tooltip: qsTr("Stop now: nothing is listed or changed")
                onClicked: Moments.cancelDeadTime()
            }
        }
        BeamProgress {
            objectName: "deadTimeProgress"
            Layout.fillWidth: true
            visible: root.busy
            indeterminate: Number(root.dead.progress ?? -1) < 0
            value: Math.max(0, Number(root.dead.progress ?? 0))
            active: root.busy
            label: root.dead.phase ?? ""
            loader: ""
        }
        // What the last find or Remove said, or why not (with stretches listed the summary row says it).
        Note {
            objectName: "deadTimeStatus"
            visible: text !== "" && !root.busy && (list.count === 0 || root.problem !== "" || root.dead.error !== undefined)
            text: root.problem !== "" ? root.problem : (root.dead.error ?? root.dead.text ?? "")
            color: root.problem !== "" || root.dead.error !== undefined ? Theme.warningText : Theme.textSecondary
        }
        Note {
            objectName: "deadTimeOptionsChangedNote"
            visible: root.staleOptions
            text: root.found ? qsTr("Options changed: this list is from the earlier settings. Find again to update it (or set them back to remove these).")
                             : qsTr("Options changed: find again to look with them.")
            color: Theme.warningText
        }
        Note {
            objectName: "deadTimeStale"
            visible: root.dead.stale === true || root.dead.otherSequence === true
            text: root.dead.otherSequence === true ? qsTr("Found on another sequence: open it again to review and remove.")
                                                   : qsTr("The sequence changed since: find again before you remove.")
            color: Theme.warningText
        }

        // ---- The stretches ----
        RowLayout {
            Layout.fillWidth: true
            visible: list.count > 0
            spacing: Theme.space(1)
            Text {
                objectName: "deadTimeSummary"
                Layout.fillWidth: true
                text: root.dead.removeText ?? ""
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            PillButton {
                objectName: "deadTimeAllOn"
                compact: true
                variant: "ghost"
                text: qsTr("All")
                tooltip: qsTr("Switch every stretch on: remove them all")
                onClicked: Moments.setAllDeadTimeRemove(true)
            }
            PillButton {
                objectName: "deadTimeAllOff"
                compact: true
                variant: "ghost"
                text: qsTr("None")
                tooltip: qsTr("Switch every stretch off: keep them all")
                onClicked: Moments.setAllDeadTimeRemove(false)
            }
        }
        ListView {
            id: list
            objectName: "deadTimeList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: list.count > 0 ? root.rowHeight * 2 + Theme.space(1) : 0
            clip: true
            spacing: Theme.space(0.5)
            // Only while the sheet is open: closing it drops the rows and with them their thumbnails' pending loads.
            model: Moments.deadTimeReviewing ? Moments.deadTimeRanges : []
            cacheBuffer: root.rowHeight * 2 // rows (and thumbnails) are made as they scroll in
            opacity: root.staleOptions ? 0.55 : 1
            boundsBehavior: Flickable.StopAtBounds
            activeFocusOnTab: count > 0 || activeFocus // (never off while it has the focus: Remove empties it)
            keyNavigationEnabled: false // the keys below move the bridge's current stretch; the rows follow it
            highlightFollowsCurrentItem: false
            currentIndex: -1
            ScrollBar.vertical: ThinScrollBar {}
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Dead time stretches: %1. Up and Down move, K keeps, Delete removes, Enter jumps")
                                 .arg(count)

            // The list keeps its keys from the editor's shortcuts (Up / Down are edit points, K stops, Delete clears).
            Keys.onShortcutOverride: event => {
                event.accepted = (event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier
                    && [Qt.Key_Up, Qt.Key_Down, Qt.Key_Home, Qt.Key_End, Qt.Key_K, Qt.Key_Delete, Qt.Key_Backspace,
                        Qt.Key_Space, Qt.Key_Return, Qt.Key_Enter].includes(event.key)
            }
            Keys.onPressed: event => {
                if ((event.modifiers & ~Qt.KeypadModifier) !== Qt.NoModifier)
                    return
                const actions = { [Qt.Key_Up]: "previous", [Qt.Key_Down]: "next", [Qt.Key_Home]: "first",
                                  [Qt.Key_End]: "last", [Qt.Key_K]: "keep", [Qt.Key_Delete]: "remove",
                                  [Qt.Key_Backspace]: "remove", [Qt.Key_Space]: "toggle" }
                const action = actions[event.key]
                if (action !== undefined) {
                    Moments.reviewDeadTime(action)
                    event.accepted = true
                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    Moments.jumpToDeadTime(Math.max(0, Moments.deadTimeCurrent))
                    event.accepted = true
                }
            }

            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                readonly property bool current: row.index === Moments.deadTimeCurrent
                readonly property bool removing: row.modelData.remove === true

                objectName: "deadTimeRow_" + row.index
                Accessible.role: Accessible.ListItem
                Accessible.name: row.removing ? qsTr("%1, %2, removed").arg(row.modelData.startText ?? "").arg(row.modelData.lengthText ?? "")
                                              : qsTr("%1, %2, kept").arg(row.modelData.startText ?? "").arg(row.modelData.lengthText ?? "")
                width: list.width - Theme.space(2)
                x: Theme.space(1)
                height: root.rowHeight
                radius: Theme.radiusControl
                color: row.current ? Theme.hover : rowHover.hovered ? Qt.alpha(Theme.hover, 0.5) : "transparent"

                HoverHandler {
                    id: rowHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: {
                        Moments.jumpToDeadTime(row.index)
                        list.forceActiveFocus()
                    }
                }
                FocusRing {
                    anchors.margins: 0
                    targetRadius: row.radius
                    visible: row.current && list.activeFocus
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space(3)
                    anchors.rightMargin: Theme.space(1)
                    spacing: Theme.space(3)
                    // A red bar like the timeline's band for the stretches that go.
                    Rectangle {
                        Layout.preferredWidth: Theme.space(1)
                        Layout.preferredHeight: Theme.iconSm
                        radius: width / 2
                        color: row.removing ? Theme.danger : Theme.textDisabled
                    }
                    // The frame at the stretch's middle: decoded off the GUI thread by image://wabcut-frames (cached there
                    // and in Qt's pixmap cache), only for the rows made.
                    Rectangle {
                        Layout.preferredWidth: Theme.space(13)
                        Layout.preferredHeight: Theme.space(13) * 9 / 16
                        radius: Theme.space(1)
                        color: Theme.sunken
                        clip: true
                        Image {
                            objectName: "deadTimeThumb"
                            anchors.fill: parent
                            source: row.modelData.thumbnail ?? ""
                            asynchronous: true
                            cache: true
                            fillMode: Image.PreserveAspectCrop
                            sourceSize.width: Theme.space(26)
                            sourceSize.height: Theme.space(26) * 9 / 16
                            opacity: row.removing ? 1 : 0.5
                        }
                    }
                    Text {
                        objectName: "deadTimeRowTime"
                        text: row.modelData.startText ?? ""
                        color: row.removing ? Theme.textPrimary : Theme.textTertiary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textSm
                    }
                    Text {
                        objectName: "deadTimeRowLength"
                        Layout.fillWidth: true
                        text: row.removing ? (row.modelData.lengthText ?? "")
                                           : qsTr("%1 · kept").arg(row.modelData.lengthText ?? "")
                        elide: Text.ElideRight
                        color: row.removing ? Theme.textSecondary : Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    Toggle {
                        objectName: "deadTimeSwitch_" + row.index
                        focusPolicy: Qt.NoFocus // the list is one Tab stop: Space, K and Delete switch the current row
                        tooltip: qsTr("Remove the stretch at %1").arg(row.modelData.startText ?? "")
                        checked: row.removing
                        onToggled: Moments.setDeadTimeRemove(row.index, checked)
                    }
                }
            }

            EmptyState {
                objectName: "deadTimeEmpty"
                anchors.centerIn: parent
                width: parent.width - Theme.space(4)
                visible: list.count === 0 && !root.busy
                illustration: "markers"
                text: root.dead.searched === true
                      ? qsTr("No dead time found. Raise the Sensitivity or shorten the Shortest stretch, then find again.")
                      : qsTr("Loading screens, walking and waiting: quiet game audio and little motion. Find them to review each one here, then remove the ones you do not keep in one undo step.")
            }
        }

        // ---- Remove ----
        Note {
            objectName: "deadTimeKeysHint"
            visible: list.count > 0
            text: qsTr("Up / Down move · K keeps · Delete removes · Enter jumps")
            wrapMode: Text.NoWrap
            elide: Text.ElideRight
        }
        PillButton {
            objectName: "deadTimeRemoveButton"
            Layout.fillWidth: true
            visible: list.count > 0
            variant: "primary"
            iconName: "scissors"
            text: root.removeCount === 1 ? qsTr("Remove 1 stretch") : qsTr("Remove %1 stretches").arg(root.removeCount)
            tooltip: qsTr("Ripple-deletes the switched-on stretches on every unlocked track, keeping the handles (one undo step)")
            enabled: root.found && root.removeCount > 0 && root.dead.stale !== true && root.dead.otherSequence !== true
                     && !root.staleOptions
            onClicked: root.remove()
        }
    }
}
