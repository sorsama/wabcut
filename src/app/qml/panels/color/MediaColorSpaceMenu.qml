pragma ComponentBehavior: Bound
import QtQuick
import QtQml.Models

// The Color panel's Media Color Space menu (0.2, I.9; Premiere Pro's "Override Media Color Space"): Auto (the stream's
// tags, or the camera log its metadata names) or an input colour space for the targeted clip - Rec.709 / sRGB /
// Rec.2020, HDR (PQ, HLG), camera log (Sony, Panasonic, ARRI, Canon, RED, Apple), ACES. One undo step
// (color.inputSpace.set).
ContextMenu {
    id: menu

    function openAt(target) {
        popup(target, 0, target.height)
    }

    ContextMenuItem {
        objectName: "mediaColorSpace_auto"
        text: qsTr("Auto (%1)").arg(Color.inputColorSpace.autoName ?? qsTr("from the media"))
        onTriggered: Color.setInputColorSpace("auto")
    }
    ContextMenuSeparator {}
    Instantiator {
        model: Color.inputColorSpaces
        delegate: ContextMenuItem {
            required property var modelData
            objectName: "mediaColorSpace_" + modelData.id
            text: modelData.name
            iconName: (Color.inputColorSpace.override ?? "") === modelData.id ? "check" : ""
            onTriggered: Color.setInputColorSpace(modelData.id)
        }
        onObjectAdded: (index, object) => menu.insertItem(index + 2, object)
        onObjectRemoved: (index, object) => menu.removeItem(object)
    }
}
