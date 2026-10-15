import QtQuick
import QtQuick.Controls.Basic
import Wabcut

// Test scene for KeymapDispatcher (tests/app/test_keymap_dispatcher.cpp). It is never shown: the tests send
// synthetic key events to the hidden window. It also shows how panels use the bridge:
//   - a panel's root item declares its scope with `KeymapScope.scope`, and descendants inherit it;
//   - tooltips read `KeymapDispatcher.shortcutText(id)` (reading `revision` too, so edits refresh them).
Window {
    id: root

    readonly property string undoShortcut: KeymapDispatcher.revision >= 0
                                           ? KeymapDispatcher.shortcutText("edit.undo") : ""

    width: 800
    height: 600
    visible: false

    Item {
        objectName: "timeline"
        KeymapScope.scope: "timeline"
        width: 400
        height: 200

        Item {
            objectName: "timelineClip"
            width: 40
            height: 20
        }
    }

    Item {
        objectName: "programMonitor"
        KeymapScope.scope: "programMonitor"
        y: 200
        width: 400
        height: 200
    }

    Item {
        objectName: "plain"
        x: 400
        width: 100
        height: 100
    }

    TextField {
        objectName: "textField"
        x: 400
        y: 200
        width: 200
    }

    // A custom control that wants J for itself claims it through ShortcutOverride.
    Item {
        objectName: "claimsJ"
        x: 400
        y: 300
        width: 50
        height: 50
        Keys.onShortcutOverride: event => event.accepted = (event.key === Qt.Key_J)
    }

    // A control that handles every key itself opts out of editor shortcuts.
    Item {
        objectName: "optedOut"
        KeymapScope.suppressShortcuts: true
        x: 400
        y: 400
        width: 50
        height: 50
    }

    Popup {
        objectName: "popup"
        focus: true
        width: 100
        height: 100

        Item {
            objectName: "popupContent"
            focus: true
            anchors.fill: parent
        }
    }
}
