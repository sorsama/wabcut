import QtQuick

// P.2 (E6): what the monitors need outside their panels (bridge Monitors; docs/monitors.md): the second program output
// window (Toggle Full Screen, Enable Transmit), the guide dialogs (Add / Edit Guide, Save Guides as Template, Manage
// Guides) and Export Frame. One element in Main.qml.
Item {
    id: host

    ProgramOutputWindow {
        objectName: "programOutputWindow"
        mainWindow: host.Window.window
    }
    AddGuideDialog {
        id: addGuide
    }
    GuideTemplatesDialog {
        id: guideTemplates
    }
    ExportFrameDialog {
        id: exportFrame
    }
    Connections {
        target: Monitors
        function onDialogRequested(dialog, index) {
            if (dialog === "addGuide")
                addGuide.openNew()
            else if (dialog === "editGuide")
                addGuide.openFor(index)
            else if (dialog === "saveGuides")
                guideTemplates.openSave()
            else if (dialog === "manageGuides")
                guideTemplates.openManage()
            else if (dialog === "exportFrame")
                exportFrame.openFor(index === 1)
        }
    }
}
