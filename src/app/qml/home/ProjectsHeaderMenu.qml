pragma ComponentBehavior: Bound
import QtQuick

// The Projects page's menu ("..." in its toolbar): Clear Unstarred Projects... (asks first). The choice is reported
// after the menu has closed, so the confirmation keeps the focus. QtQuick only, so the Wabcut ContextMenu is not
// shadowed by QtQuick.Controls'.
ContextMenu {
    id: menu
    objectName: "homeProjectsMenu"

    property int unstarredCount: 0
    property string pendingAction: ""

    /// "clearUnstarred"
    signal chosen(string action)

    onClosed: {
        const action = menu.pendingAction
        menu.pendingAction = ""
        if (action !== "")
            menu.chosen(action)
    }

    ContextMenuItem {
        objectName: "homeClearUnstarred"
        text: qsTr("Clear Unstarred Projects…")
        iconName: "trash-2"
        enabled: menu.unstarredCount > 0
        onTriggered: menu.pendingAction = "clearUnstarred"
    }
}
