pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Review notes (M5m, bridge `Review`): the filters (open / resolved / all, this sequence or every sequence, text), the
// notes in time order (ReviewNoteCard), and the composer: a note at the playhead or over In-Out, with a point or a
// rectangle drawn on the Program Monitor ("Mark on frame"). Below: notes to markers, and review packages out (a folder
// with a page a client opens in any browser) and back in (replies and status update the notes).
Item {
    id: root
    objectName: "reviewNotesPage"
    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Review notes")

    readonly property bool hasInOut: ProjectController.markInText !== "" && ProjectController.markOutText !== ""

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(2)

        PillTabs {
            id: statusTabs
            objectName: "reviewStatusFilter"
            Layout.fillWidth: true
            tabs: true
            readonly property var ids: ["all", "open", "resolved"]
            model: [
                { label: qsTr("All") },
                { label: qsTr("Open (%1)").arg(Review.openCount) },
                { label: qsTr("Resolved (%1)").arg(Review.resolvedCount) }
            ]
            currentIndex: Math.max(0, ids.indexOf(Review.statusFilter))
            onActivated: index => Review.statusFilter = ids[index]
            Accessible.name: qsTr("Show notes")
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            FieldInput {
                objectName: "reviewSearch"
                Layout.fillWidth: true
                iconName: "search"
                clearOnEscape: true
                placeholderText: qsTr("Search notes")
                text: Review.query
                onTextChanged: Review.query = text
                Accessible.name: qsTr("Search notes")
            }
            Toggle {
                objectName: "reviewAllSequences"
                text: qsTr("All sequences")
                checked: Review.allSequences
                onToggled: Review.allSequences = checked
            }
        }

        ListView {
            id: list
            objectName: "reviewNotesList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: Theme.space(1.5)
            model: Review.notes
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ThinScrollBar {}
            delegate: ReviewNoteCard {
                required property var modelData
                note: modelData
                width: ListView.view.width
            }

            EmptyState {
                anchors.centerIn: parent
                width: parent.width - Theme.space(8)
                visible: list.count === 0
                illustration: "timeline"
                text: Review.openCount + Review.resolvedCount === 0
                      ? qsTr("No review notes yet. Write one below: it is pinned to the frame at the playhead.")
                      : qsTr("No notes match the filters.")
            }
        }

        // The composer.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: composer.implicitHeight + Theme.space(4)
            radius: Theme.radiusCard
            color: Theme.sunken
            border.width: Theme.hairline
            border.color: Review.drawing ? Theme.accentStroke : Theme.border

            ColumnLayout {
                id: composer
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Theme.space(2)
                spacing: Theme.space(1.5)

                FieldInput {
                    id: noteField
                    objectName: "reviewNoteField"
                    Layout.fillWidth: true
                    placeholderText: overInOut.checked && root.hasInOut
                                     ? qsTr("Note over %1–%2").arg(ProjectController.markInText).arg(ProjectController.markOutText)
                                     : qsTr("Note at %1").arg(ProjectController.playheadText)
                    Accessible.name: qsTr("New review note")
                    onAccepted: addButton.clicked()
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(1.5)
                    IconButton {
                        objectName: "reviewMarkToggle"
                        compact: true
                        checked: Review.drawing
                        iconName: "crosshair"
                        tooltip: qsTr("Mark on frame: click a spot or drag a rectangle on the Program Monitor")
                        onClicked: Review.drawing = !Review.drawing
                    }
                    Text {
                        Layout.fillWidth: true
                        text: Review.pendingMark.x !== undefined
                              ? ((Review.pendingMark.width ?? 0) > 0 ? qsTr("Rectangle marked") : qsTr("Spot marked"))
                              : Review.drawing ? qsTr("Click or drag on the monitor") : ""
                        elide: Text.ElideRight
                        color: Review.pendingMark.x !== undefined ? Theme.accentStroke : Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    IconButton {
                        compact: true
                        visible: Review.pendingMark.x !== undefined
                        iconName: "x"
                        tooltip: qsTr("Remove the mark")
                        onClicked: Review.clearPendingMark()
                    }
                    Toggle {
                        id: overInOut
                        objectName: "reviewOverInOut"
                        visible: root.hasInOut
                        text: qsTr("In–Out")
                        tooltip: qsTr("The note covers the sequence's In to Out")
                    }
                    PillButton {
                        id: addButton
                        objectName: "reviewAddButton"
                        variant: "primary"
                        compact: true
                        iconName: "plus"
                        text: qsTr("Add note")
                        enabled: noteField.text.trim() !== ""
                        onClicked: {
                            if (Review.add(noteField.text, overInOut.checked && root.hasInOut) === "")
                                noteField.text = ""
                        }
                    }
                }
            }
        }

        // Markers and packages.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1.5)
            PillButton {
                objectName: "reviewToMarkers"
                compact: true
                variant: "ghost"
                iconName: "bookmark"
                text: qsTr("To markers")
                tooltip: qsTr("The sequence's notes as markers: red while open, green once resolved")
                enabled: Review.notes.length > 0
                onClicked: Review.toMarkers(false)
            }
            Item { Layout.fillWidth: true }
            PillButton {
                objectName: "reviewExportPackage"
                compact: true
                variant: "ghost"
                iconName: "upload"
                text: qsTr("Export…")
                tooltip: qsTr("A review package: a page with a thumbnail per note that a client opens in any browser")
                enabled: !Review.busy && Review.openCount + Review.resolvedCount > 0
                onClicked: exportDialog.open()
            }
            PillButton {
                objectName: "reviewImportPackage"
                compact: true
                variant: "ghost"
                iconName: "download"
                text: qsTr("Import…")
                tooltip: qsTr("Read a review package back: replies and status update the notes")
                onClicked: importDialog.open()
            }
        }
        BeamProgress {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.hairline * 3
            visible: Review.busy
            indeterminate: true
            active: Review.busy
            label: qsTr("Writing the review package")
        }
        Text {
            objectName: "reviewPackageResult"
            Layout.fillWidth: true
            visible: text !== ""
            wrapMode: Text.Wrap
            color: Review.packageResult.ok === false ? Theme.danger
                   : (Review.packageResult.conflicts ?? []).length > 0 ? Theme.warning : Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            text: {
                const r = Review.packageResult
                if (r.kind === undefined)
                    return ""
                if (r.ok === false)
                    return r.error ?? ""
                if (r.kind === "export")
                    return qsTr("Wrote %n note(s) with their thumbnails.", "", r.notes ?? 0)
                const conflicts = r.conflicts ?? []
                return qsTr("%1 added, %2 updated, %3 unchanged.").arg(r.added ?? 0).arg(r.updated ?? 0).arg(r.unchanged ?? 0)
                       + (conflicts.length > 0 ? " " + qsTr("Conflicts: %1").arg(conflicts.join("; ")) : "")
            }
        }
    }

    FileDialog {
        id: exportDialog
        title: qsTr("Export review package")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "wabreview"
        nameFilters: [qsTr("Review packages (*.wabreview)")]
        currentFolder: ProjectFile.folder
        onAccepted: Review.exportPackage(selectedFile, ProjectController.sequenceName, false)
    }
    FolderDialog {
        id: importDialog
        title: qsTr("Import review package")
        currentFolder: ProjectFile.folder
        onAccepted: Review.importPackage(selectedFolder)
    }
}
