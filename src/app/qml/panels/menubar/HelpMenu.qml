pragma ComponentBehavior: Bound
import QtQuick

// Help (the menu bar): the User Guide on the website (Home.guideUrl / Home.links: one source with Home's Learn page),
// the Keyboard Shortcuts editor, the onboarding again (Welcome and Setup, the tour, the Keyboard Trainer, What's new),
// the release notes, the sample project, Report an Issue and About. Everything runs once the menu has closed, so a dialog
// it opens keeps the focus (window.showHelp).
MenuBarMenu {
    id: root
    objectName: "helpMenu"
    title: qsTr("Help")
    key: "help"
    mnemonic: "H"

    /// The website page of Home.links `id` ("" when Home has no such link).
    function linkUrl(id: string): string {
        return (Home.links.find(l => l.id === id)?.url ?? "").toString()
    }

    component LinkItem: BarMenuItem {
        id: link
        /// The page it opens.
        property string url: ""
        available: link.url !== ""
        onTriggered: (link.menu as MenuBarMenu)?.run(() => Qt.openUrlExternally(link.url))
    }
    component HelpItem: BarMenuItem {
        id: help
        /// What window.showHelp runs: "welcome", "tour", "trainer", "whatsNew" or "shortcuts".
        property string what: ""
        objectName: "helpItem_" + help.what
        runsCommand: false
        onTriggered: (help.menu as MenuBarMenu)?.request("help", help.what)
    }

    LinkItem {
        objectName: "helpItem_guide"
        text: qsTr("Wabcut User Guide")
        url: Home.guideUrl("").toString()
    }
    LinkItem {
        objectName: "helpItem_gettingStarted"
        text: qsTr("Getting Started")
        url: root.linkUrl("gettingStarted")
    }
    LinkItem {
        objectName: "helpItem_fromPremiere"
        text: qsTr("Coming from Premiere Pro")
        url: root.linkUrl("fromPremiere")
    }
    LinkItem {
        objectName: "helpItem_shortcutsGuide"
        text: qsTr("Keyboard Shortcuts Guide")
        url: root.linkUrl("shortcuts")
    }
    ContextMenuSeparator {}
    HelpItem {
        what: "shortcuts"
        text: qsTr("Keyboard Shortcuts…")
        command: "edit.keyboardShortcuts"
    }
    HelpItem {
        what: "welcome"
        text: qsTr("Welcome and Setup…")
    }
    HelpItem {
        what: "tour"
        text: qsTr("Take the Tour")
    }
    HelpItem {
        what: "trainer"
        text: qsTr("Keyboard Trainer…")
    }
    HelpItem {
        what: "whatsNew"
        text: qsTr("What's New")
    }
    LinkItem {
        objectName: "helpItem_releaseNotes"
        text: qsTr("Release Notes")
        url: root.linkUrl("releaseNotes")
    }
    ContextMenuSeparator {}
    // The demo project as at launch (after the unsaved-changes prompt).
    BarMenuItem {
        objectName: "helpItem_sample"
        text: qsTr("Open the Sample Project")
        onTriggered: root.request("sample", null)
    }
    LinkItem {
        objectName: "helpItem_reportIssue"
        text: qsTr("Report an Issue")
        url: root.linkUrl("reportIssue")
    }
    ContextMenuSeparator {}
    BarMenuItem {
        objectName: "helpItem_about"
        text: qsTr("About Wabcut")
        onTriggered: root.request("about", null)
    }
}
