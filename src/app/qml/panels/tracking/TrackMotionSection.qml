pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic as QC // qualified: unqualified, its ContextMenu would hide the app's

// ● TRACK MOTION (0.7, card #83; docs/masks.md "Track Motion") in Effect Controls: pin a graphic, title, picture or
// censor bar to something that moves in the shot and have it follow. The `TrackMotion` singleton runs the engine's
// track.transform job:
//   * "Subject in" - the footage clip with the moving subject (a video clip under the playhead);
//   * "Move a clip" - another clip follows the subject (its Position, and with "Position, scale, rotation" also its Scale
//     and Rotation, keyed) - or "Stabilize": the footage itself is keyed so the subject stays where it is;
//   * the subject area: Draw box (drag a box on the program monitor, then move or resize it with its handles) or a mask of
//     the footage clip (an AI Object Mask follows a person or thing this way);
//   * Track backward / forward from the playhead (put the playhead on the frame where the clip sits over the subject): a
//     job with progress, Stop and a per-frame confidence strip; it stops where the picture is lost. One undo step.
// It shows while Effect Controls' header button (or Clip > Track Motion…) has it on and a footage clip is under the playhead;
// its x (or the button) closes it. It is off by default so the panel keeps its room.
Rectangle {
    id: root
    objectName: "trackMotionSection"

    /// The owner caps the section (the effect list keeps its room); the rest scrolls under the pinned header.
    property real maximumHeight: Number.POSITIVE_INFINITY

    implicitHeight: Math.min(header.implicitHeight + column.implicitHeight + Theme.space(1.5) + Theme.space(3), maximumHeight)
    clip: true
    radius: Theme.radiusField
    color: Theme.sunken
    border.width: Theme.hairline
    border.color: Theme.border

    function clipLabel(entry): string {
        return entry.track !== "" ? qsTr("%1 · %2").arg(entry.track).arg(entry.name) : entry.name
    }

    /// A section that was closed or hidden opens at its top (header first), however far it was scrolled before.
    onVisibleChanged: {
        if (visible)
            flick.contentY = 0
    }

    // While tracking, bring the Track row (and under it the progress bar, the confidence strip and the status) into view;
    // the header stays pinned above the scrolling part.
    Connections {
        target: TrackMotion
        function onTrackingChanged() {
            if (TrackMotion.tracking)
                flick.contentY = Math.max(0, Math.min(trackerRow.y - Theme.space(1), flick.contentHeight - flick.height))
        }
    }

    ColumnLayout {
        id: frame
        anchors.fill: parent
        anchors.margins: Theme.space(1.5)
        spacing: Theme.space(1.5)

        // Header: title and close. It does not scroll with the rest.
        RowLayout {
            id: header
            objectName: "trackMotionHeader"
            Layout.fillWidth: true
            spacing: Theme.space(1)

            Text {
                Layout.fillWidth: true
                text: qsTr("Track Motion")
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            IconButton {
                objectName: "trackMotionClose"
                compact: true
                iconName: "x"
                tooltip: qsTr("Close Track Motion")
                Accessible.name: tooltip
                onClicked: TrackMotion.panelOpen = false
            }
        }

    Flickable {
        id: flick
        objectName: "trackMotionFlick"
        Layout.fillWidth: true
        Layout.fillHeight: true
        contentWidth: width
        contentHeight: column.implicitHeight
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        QC.ScrollBar.vertical: ThinScrollBar {}

    ColumnLayout {
        id: column
        width: flick.width
        spacing: Theme.space(1.5)

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1.5)

            // What it will do, in one line (the clips are set below; they default from the clip Effect Controls shows).
            Text {
                objectName: "trackSummary"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: !TrackMotion.available ? ""
                    : TrackMotion.stabilize ? qsTr("Holds %1 still where the subject is.").arg(TrackMotion.subjectName)
                    : TrackMotion.followerName === "" ? qsTr("Pick the clip that follows the subject in %1.").arg(TrackMotion.subjectName)
                    : qsTr("%1 follows the subject in %2.").arg(TrackMotion.followerName).arg(TrackMotion.subjectName)
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }

            // The subject area: a box on the program monitor, or a mask of the footage.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(1)

                PillButton {
                    objectName: "trackDrawBox"
                    compact: true
                    variant: TrackMotion.boxTool ? "primary" : "secondary"
                    iconName: "square"
                    text: TrackMotion.boxTool ? qsTr("Drag on the monitor…") : qsTr("Draw box")
                    tooltip: qsTr("Drag a box around the subject on the program monitor")
                    enabled: TrackMotion.available && !TrackMotion.tracking
                    onClicked: {
                        if (TrackMotion.boxTool)
                            TrackMotion.boxTool = false
                        else
                            TrackMotion.boxTool = true
                    }
                }
                PillButton {
                    objectName: "trackPlaceBox"
                    compact: true
                    variant: "ghost"
                    text: qsTr("Box on the clip")
                    tooltip: qsTr("Put a box where the following clip sits now (then drag it onto the subject)")
                    enabled: TrackMotion.available && !TrackMotion.tracking
                    onClicked: TrackMotion.placeBox()
                }
                PillButton {
                    id: maskButton
                    objectName: "trackMaskButton"
                    compact: true
                    variant: TrackMotion.mask !== "" ? "primary" : "ghost"
                    iconName: "chevron-down"
                    visible: TrackMotion.masks.length > 0
                    text: qsTr("Mask")
                    tooltip: qsTr("Follow the outline of a mask of the footage (an AI Object Mask follows a person or thing)")
                    enabled: !TrackMotion.tracking
                    onClicked: maskMenu.popup(maskButton, 0, maskButton.height + Theme.space(1))

                    ContextMenu {
                        id: maskMenu
                        objectName: "trackMaskMenu"
                        Instantiator {
                            model: TrackMotion.masks
                            delegate: ContextMenuItem {
                                required property var modelData
                                text: modelData.name
                                iconName: modelData.key === TrackMotion.mask ? "check" : ""
                                onTriggered: TrackMotion.mask = modelData.key === TrackMotion.mask ? "" : modelData.key
                            }
                            onObjectAdded: (index, object) => maskMenu.insertItem(index, object)
                            onObjectRemoved: (index, object) => maskMenu.removeItem(object)
                        }
                    }
                }
                Item {
                    Layout.fillWidth: true
                }
                IconButton {
                    objectName: "trackClearArea"
                    compact: true
                    iconName: "x"
                    visible: TrackMotion.box.length === 4 || TrackMotion.mask !== ""
                    tooltip: qsTr("Forget the subject area")
                    Accessible.name: tooltip
                    onClicked: {
                        TrackMotion.clearBox()
                        TrackMotion.mask = ""
                    }
                }
            }
            Text {
                objectName: "trackAreaHint"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: TrackMotion.mask !== "" ? qsTr("Following a mask of %1.").arg(TrackMotion.subjectName)
                    : TrackMotion.box.length === 4 ? qsTr("Move the box or drag its corners until it sits on the subject.")
                    : qsTr("Put the playhead on the frame where the clip sits over the subject, then mark the subject.")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }

            // The tracker.
            RowLayout {
                id: trackerRow
                Layout.fillWidth: true
                spacing: Theme.space(1)

                PillButton {
                    objectName: "trackBackward"
                    compact: true
                    variant: "secondary"
                    iconName: "step-back"
                    text: qsTr("Track back")
                    tooltip: qsTr("Track backward from the playhead to the start of the clip")
                    enabled: TrackMotion.blocker === "" && !TrackMotion.tracking
                    disabledReason: TrackMotion.blocker
                    onClicked: TrackMotion.track("backward", 0)
                }
                PillButton {
                    objectName: "trackForward"
                    compact: true
                    variant: "primary"
                    iconName: "step-forward"
                    text: qsTr("Track forward")
                    tooltip: qsTr("Track forward from the playhead to the end of the clip")
                    enabled: TrackMotion.blocker === "" && !TrackMotion.tracking
                    disabledReason: TrackMotion.blocker
                    onClicked: TrackMotion.track("forward", 0)
                }
                PillButton {
                    objectName: "trackStop"
                    compact: true
                    variant: "ghost"
                    text: qsTr("Stop")
                    visible: TrackMotion.tracking
                    onClicked: TrackMotion.cancelTrack()
                }
                Item {
                    Layout.fillWidth: true
                }
            }
            Rectangle {
                id: bar
                objectName: "trackProgressBar"
                Layout.fillWidth: true
                visible: TrackMotion.tracking
                implicitHeight: Theme.space(0.75)
                radius: height / 2
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: Theme.border
                Rectangle {
                    width: bar.width * Math.min(1, Math.max(0, TrackMotion.trackProgress))
                    height: parent.height
                    radius: height / 2
                    color: Theme.accentFill
                }
            }
            ConfidenceStrip {
                objectName: "trackConfidenceStrip"
                Layout.fillWidth: true
                visible: TrackMotion.confidence.length > 0
                values: TrackMotion.confidence
                framesPerValue: TrackMotion.confidenceFramesPerValue
                total: TrackMotion.trackTotal
            }
            Text {
                objectName: "trackStatus"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                visible: text !== ""
                text: TrackMotion.trackStatus !== "" ? TrackMotion.trackStatus : TrackMotion.available ? TrackMotion.blocker : ""
                color: TrackMotion.trackStatus === "" ? Theme.textTertiary
                    : TrackMotion.trackStatus.startsWith(qsTr("Could not")) || TrackMotion.trackStatus.startsWith(qsTr("Lost"))
                      ? Theme.warning : Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            // The two clips and what follows (defaults are usually right: they come from the clip Effect Controls shows).
            SectionHeader {
                Layout.fillWidth: true
                text: qsTr("Clips and what follows")
            }

            // The footage with the moving subject.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(1)

                Text {
                    text: qsTr("Subject in")
                    Layout.preferredWidth: Theme.propLabelWidth
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    elide: Text.ElideRight
                }
                PillButton {
                    id: subjectButton
                    objectName: "trackSubjectButton"
                    Layout.fillWidth: true
                    Layout.maximumWidth: Theme.space(60)
                    compact: true
                    variant: "ghost"
                    iconName: "chevron-down"
                    text: TrackMotion.subjectName !== "" ? TrackMotion.subjectName : qsTr("No footage here")
                    tooltip: qsTr("The clip with the moving subject: a video clip under the playhead")
                    enabled: TrackMotion.subjects.length > 0 && !TrackMotion.tracking
                    onClicked: subjectMenu.popup(subjectButton, 0, subjectButton.height + Theme.space(1))

                    ContextMenu {
                        id: subjectMenu
                        objectName: "trackSubjectMenu"
                        Instantiator {
                            model: TrackMotion.subjects
                            delegate: ContextMenuItem {
                                required property var modelData
                                text: root.clipLabel(modelData)
                                iconName: modelData.key === TrackMotion.subject ? "check" : ""
                                onTriggered: TrackMotion.subject = modelData.key
                            }
                            onObjectAdded: (index, object) => subjectMenu.insertItem(index, object)
                            onObjectRemoved: (index, object) => subjectMenu.removeItem(object)
                        }
                    }
                }
            }

            // What happens to it: another clip follows, or the footage is held still.
            PillTabs {
                objectName: "trackModeTabs"
                Layout.fillWidth: true
                compact: true
                Accessible.name: qsTr("What the track does")
                model: [qsTr("Move a clip with it"), qsTr("Hold it still")]
                currentIndex: TrackMotion.stabilize ? 1 : 0
                enabled: !TrackMotion.tracking
                onActivated: index => TrackMotion.stabilize = index === 1
            }
            RowLayout {
                Layout.fillWidth: true
                visible: !TrackMotion.stabilize
                spacing: Theme.space(1)

                Text {
                    text: qsTr("Follows")
                    Layout.preferredWidth: Theme.propLabelWidth
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    elide: Text.ElideRight
                }
                PillButton {
                    id: followerButton
                    objectName: "trackFollowerButton"
                    Layout.fillWidth: true
                    Layout.maximumWidth: Theme.space(60)
                    compact: true
                    variant: "ghost"
                    iconName: "chevron-down"
                    text: TrackMotion.followerName !== "" ? TrackMotion.followerName : qsTr("Pick a clip")
                    tooltip: qsTr("The graphic, title, picture or censor bar that follows the subject")
                    enabled: TrackMotion.followers.length > 0 && !TrackMotion.tracking
                    onClicked: followerMenu.popup(followerButton, 0, followerButton.height + Theme.space(1))

                    ContextMenu {
                        id: followerMenu
                        objectName: "trackFollowerMenu"
                        Instantiator {
                            model: TrackMotion.followers
                            delegate: ContextMenuItem {
                                required property var modelData
                                text: root.clipLabel(modelData)
                                iconName: modelData.key === TrackMotion.follower ? "check" : ""
                                onTriggered: TrackMotion.follower = modelData.key
                            }
                            onObjectAdded: (index, object) => followerMenu.insertItem(index, object)
                            onObjectRemoved: (index, object) => followerMenu.removeItem(object)
                        }
                    }
                }
            }
            PillTabs {
                objectName: "trackMatchTabs"
                Layout.fillWidth: true
                compact: true
                visible: !TrackMotion.stabilize
                Accessible.name: qsTr("What follows")
                model: [qsTr("Position"), qsTr("Position, scale, rotation")]
                currentIndex: TrackMotion.match === "full" ? 1 : 0
                enabled: !TrackMotion.tracking
                onActivated: index => TrackMotion.match = index === 1 ? "full" : "position"
            }
            Text {
                Layout.fillWidth: true
                visible: TrackMotion.stabilize
                wrapMode: Text.Wrap
                text: qsTr("The footage is moved, turned and scaled the other way, so the subject stays where it is at the frame you start from.")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }
    }
    }
    }
}
