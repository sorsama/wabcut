pragma ComponentBehavior: Bound
import QtQuick

// Graphics (the menu bar; Premiere's Graphics and Titles): new layers (they open the Callouts panel), arranging and
// selecting layers. Every row is a keymap command (its shortcut shows; greyed out while it cannot run).
MenuBarMenu {
    id: root
    objectName: "graphicsMenu"
    title: qsTr("Graphics")
    key: "graphics"
    mnemonic: "G"

    MenuBarMenu {
        objectName: "graphicsMenuNewLayer"
        title: qsTr("New Layer")
        nested: true
        bar: root.bar

        BarMenuItem {
            text: qsTr("Text")
            command: "graphics.newTextLayer"
        }
        BarMenuItem {
            text: qsTr("Rectangle")
            command: "graphics.newRectangleLayer"
        }
        BarMenuItem {
            text: qsTr("Ellipse")
            command: "graphics.newEllipseLayer"
        }
    }
    MenuBarMenu {
        objectName: "graphicsMenuArrange"
        title: qsTr("Arrange")
        nested: true
        bar: root.bar

        BarMenuItem {
            text: qsTr("Bring to Front")
            command: "graphics.bringToFront"
        }
        BarMenuItem {
            text: qsTr("Bring Forward")
            command: "graphics.bringForward"
        }
        BarMenuItem {
            text: qsTr("Send Backward")
            command: "graphics.sendBackward"
        }
        BarMenuItem {
            text: qsTr("Send to Back")
            command: "graphics.sendToBack"
        }
    }
    ContextMenuSeparator {}
    BarMenuItem {
        text: qsTr("Select Next Layer")
        command: "graphics.selectNextLayer"
    }
    BarMenuItem {
        text: qsTr("Select Previous Layer")
        command: "graphics.selectPreviousLayer"
    }
    // P1 round (#50): template files. The window's file dialog imports a .mogrt (Premiere's, approximately) or a
    // .wabcut-template.json into This project or My templates; it opens after the menu closed.
    ContextMenuSeparator {}
    BarMenuItem {
        objectName: "installTemplateMenuItem"
        text: qsTr("Install Motion Graphics Template…")
        runsCommand: false
        onTriggered: root.run(() => Graphics.openInstallDialog("project"))
    }
    BarMenuItem {
        objectName: "installTemplateUserMenuItem"
        text: qsTr("Install Motion Graphics Template into My Templates…")
        runsCommand: false
        onTriggered: root.run(() => Graphics.openInstallDialog("user"))
    }
}
