pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// ● CALLOUTS (M5o, panel key "graphics"; bridge `Graphics`; Premiere's Essential Graphics panel, C.13, made simple for
//   gaming videos):
//   * Browse: the built-in gaming callouts (Subscribe, Kill streak, Lower third, Arrow / Circle callout, Victory / GG,
//     Countdown), Lottie stickers, the project's templates and the user's, with live thumbnails; drag one onto the
//     timeline or press Enter to add it at the playhead (its first text layer selected, ready to type);
//   * Edit: the selected graphic clip - a template's fields and the selected layer's text, font, size and colour; the
//     layers, their properties, animation presets, pins and Responsive Design behind "Advanced".
// Selecting a graphic clip on the timeline turns to Edit; the Program Monitor's handles edit the same selection
// (double-click a text layer there to type in place).
Panel {
    id: root
    objectName: "essentialGraphicsPanel"

    function focusContent() {
        tabs.forceActiveFocus(Qt.TabFocusReason)
    }
    function openSaveDialog() {
        saveDialog.openForSelection()
    }

    title: qsTr("Callouts")

    // Another graphic selected on the timeline (or made by adding a template or a layer): Edit. Not a binding: the
    // handler compares with the clip it saw last.
    property string shownClip: ""
    Component.onCompleted: shownClip = Graphics.clip
    Connections {
        target: Graphics
        function onGraphicChanged() {
            if (Graphics.clip === root.shownClip)
                return
            root.shownClip = Graphics.clip
            if (Graphics.clip !== "")
                tabs.currentIndex = 1
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(2)

        PillTabs {
            id: tabs
            objectName: "graphicsTabs"
            Layout.fillWidth: true
            compact: true
            tabs: true
            model: [qsTr("Browse"), qsTr("Edit")]
            currentIndex: Graphics.clip !== "" ? 1 : 0
            Accessible.name: qsTr("Callouts view")
        }
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            GraphicsBrowsePage {
                onSaveRequested: root.openSaveDialog()
            }
            GraphicsEditPage {
                onSaveRequested: root.openSaveDialog()
            }
        }
    }

    GraphicTemplateDialog {
        id: saveDialog
    }
}
