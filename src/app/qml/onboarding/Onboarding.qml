pragma ComponentBehavior: Bound
import QtQuick

// Onboarding (M5j; fun, skippable, never in the way): the first-run welcome ("Coming from Premiere?"), the three-step
// tour, the Keyboard Trainer and What's new. Main places it over the window, gives it the tour's targets and runs
// what it asks for (a workspace, the Library). Help (the left rail) and the command palette run any of it again
// (run()).
//   * firstRun(): the welcome, once, when it was never completed or skipped (Preferences.welcomeDone). Main calls it
//     when the launch Home screen closes, so it never covers Home or a project being opened.
Item {
    id: root
    objectName: "onboarding"

    // The tour's targets (Main's items).
    property Item libraryTarget: null
    property Item timelineTarget: null
    property Item exportTarget: null
    /// Main's workspace (0 Edit, 1 Audio, 2 Export, 3 Color, 4 Captions).
    property int workspace: 0

    signal workspaceRequested(int workspace)
    signal libraryRequested()
    /// A dialog closed or the tour ended (Main puts focus back in the editor if it was left on nothing).
    signal dialogClosed()

    readonly property alias welcome: welcome
    readonly property alias tour: tour
    readonly property alias trainer: trainer
    readonly property alias whatsNew: whatsNew
    property bool firstRunShown: false

    /// The welcome on first run; false when it is not due.
    function firstRun() {
        if (Preferences.welcomeDone || root.firstRunShown)
            return false
        root.firstRunShown = true
        welcome.begin(root.workspace)
        return true
    }
    /// Help: "welcome", "tour", "trainer" or "whatsNew".
    function run(what) {
        switch (what) {
        case "welcome":
            welcome.begin(root.workspace)
            return true
        case "tour":
            tour.start()
            return true
        case "trainer":
            trainer.open()
            return true
        case "whatsNew":
            whatsNew.open()
            return true
        }
        return false
    }
    function shortcut(command) {
        const text = KeymapDispatcher.revision >= 0 ? KeymapDispatcher.shortcutText(command) : ""
        return text !== "" ? text : qsTr("its menu command")
    }

    // Opening Export from its step completes the tour.
    onWorkspaceChanged: {
        if (tour.active && tour.current.key === "export" && root.workspace === 2)
            tour.finish(true)
    }

    WelcomeDialog {
        id: welcome
        onWorkspaceChosen: id => root.workspaceRequested(id)
        onFinished: how => {
            root.dialogClosed()
            if (how === "tour")
                Qt.callLater(() => tour.start())
            else if (how === "trainer")
                Qt.callLater(() => trainer.open())
        }
    }

    CoachMarks {
        id: tour
        anchors.fill: parent
        steps: [
            { key: "library", target: root.libraryTarget, action: "",
              title: qsTr("The Library"),
              //: Tour; %1 = the Import shortcut
              text: qsTr("Your media lives here. Import with %1 or drop files in, and double-click a clip to open it in the Source Monitor.")
                        .arg(root.shortcut("file.import")) },
            { key: "timeline", target: root.timelineTarget, action: "",
              title: qsTr("The Timeline"),
              //: Tour; %1 = Add Edit's shortcut, %2 = Ripple Delete's
              text: qsTr("Cut with the keys you know: %1 adds an edit, %2 ripple deletes, and J, K and L shuttle. The List button shows it as a list for screen readers.")
                        .arg(root.shortcut("sequence.addEdit")).arg(root.shortcut("edit.rippleDelete")) },
            { key: "export", target: root.exportTarget, action: qsTr("Open Export"),
              title: qsTr("Export"),
              //: Tour; %1 = the Export Media shortcut
              text: qsTr("When the cut is ready, the Export workspace renders it (%1). Open it to finish the tour.")
                        .arg(root.shortcut("file.exportMedia")) }
        ]
        onStepShown: key => {
            if (key === "library")
                root.libraryRequested()
            else if (key === "timeline" && root.workspace === 2)
                root.workspaceRequested(0)
        }
        onActionRequested: key => {
            if (key === "export")
                root.workspaceRequested(2)
        }
        onFinished: root.dialogClosed()
    }

    KeyboardTrainer {
        id: trainer
        onClosed: root.dialogClosed()
    }

    WhatsNewDialog {
        id: whatsNew
        onClosed: root.dialogClosed()
    }
}
