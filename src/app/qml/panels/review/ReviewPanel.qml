pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// ● REVIEW (M5m, panel key "review"): client review and the version history (C.9).
//   Notes      frame-accurate review notes on the sequence: at the playhead or over In-Out, a point or rectangle drawn on
//              the Program Monitor, threads, resolve, filters; notes to markers; review packages out and back in.
//   Versions   the project's version history: save a named version, restore (one undo step) or restore a sequence as
//              a new one, export a version as a project, compare two versions (the changes as lines, the timeline
//              highlights the changed ranges).
// Bridges: Review (ReviewBridge) and History (HistoryBridge); every edit is one undo step.
Panel {
    id: root
    objectName: "reviewPanel"

    title: qsTr("Review")

    /// Show a page: 0 notes, 1 versions.
    function showPage(index) {
        tabs.currentIndex = index
    }
    // Focus target for Window > Review.
    function focusContent() {
        tabs.forceActiveFocus(Qt.TabFocusReason)
    }

    // The notes' author is the name in Preferences.
    Binding {
        target: Review
        property: "author"
        value: Preferences.reviewAuthor
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(3)

        PillTabs {
            id: tabs
            objectName: "reviewTabs"
            Layout.fillWidth: true
            compact: true
            model: [
                { label: qsTr("Notes"), icon: "bookmark" },
                { label: qsTr("Versions"), icon: "clock" }
            ]
            Accessible.name: qsTr("Review view")
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            ReviewNotesPage {}
            ReviewVersionsPage {}
        }
    }
}
