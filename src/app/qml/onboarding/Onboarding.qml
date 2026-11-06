pragma ComponentBehavior: Bound
import QtQuick

// Onboarding (M5j; fun, skippable, never in the way): the first-run welcome ("Coming from Premiere?"), the seven-step
// tour (Library, Timeline, menu bar, Program Monitor, Effects, command palette, Export), the Keyboard Trainer and What's new. Main places it over the window, gives it the tour's targets and runs
// what it asks for (a workspace, the Library). The Help menu and the command palette run any of it again
// (run()).
//   * firstRun(): the welcome, once, when it was never completed or skipped (Preferences.welcomeDone). Main calls it
//     when the launch Home screen closes, so it never covers Home or a project being opened.
Item {
    id: root
    objectName: "onboarding"

    // The tour's targets (Main's items).
    property Item libraryTarget: null
    property Item timelineTarget: null
    property Item menuBarTarget: null
    property Item programTarget: null
    property Item effectsTarget: null
    property Item exportTarget: null
    /// Main's workspace (0 Edit, 1 Audio, 2 Export, 3 Color, 4 Captions).
    property int workspace: 0

    signal workspaceRequested(int workspace)
    signal libraryRequested()
    /// A step points at a panel that may be closed or sit in a hidden tab ("program", "effects"): Main shows it (without
    /// moving the keyboard focus off the tour card).
    signal panelRequested(string key)
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
            { key: "menubar", target: root.menuBarTarget, action: "",
              title: qsTr("The menu bar"),
              text: qsTr("Every command is in these menus, with its shortcut beside it. Window opens any panel as a tab of the panel group you clicked last. In a narrow window the menus fold into one Menu button.") },
            { key: "program", target: root.programTarget, action: "",
              title: qsTr("The Program Monitor"),
              text: qsTr("This is your cut. The playback resolution button in its header trades sharpness for speed on a slow machine, and clicking the timecode lets you type a time to jump to.") },
            { key: "effects", target: root.effectsTarget, action: "",
              title: qsTr("The Effects panel"),
              text: qsTr("Search the effects and transitions here, then drag one onto a clip or double-click it to put it on the selected clips.") },
            { key: "palette", target: null, action: "",
              title: qsTr("The command palette"),
              //: Tour; %1 = the command palette's shortcut
              text: qsTr("Press %1 and type what you want to do: it finds any command, panel or setting, and shows its shortcut when it has one.")
                        .arg(Qt.platform.os === "osx" ? "⇧⌘P" : "Ctrl+Shift+P") },
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
            else if (key === "program" || key === "effects")
                root.panelRequested(key)
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
