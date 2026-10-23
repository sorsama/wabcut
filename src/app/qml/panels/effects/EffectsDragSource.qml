pragma ComponentBehavior: Bound
import QtQuick

// The pointer input of an Effects panel item (a row or an Icons-view tile; UX rework): a click selects it (`tapped`), a
// double-click applies it (`doubleTapped`), a right-click asks for its menu (`menuRequested`), and a drag carries it
// onto a clip or an edit point in the timeline, onto Effect Controls or a mixer strip (mime
// application/x-wabcut-effect = the item id), or onto a custom bin or presets bin in the panel itself. The picture
// under the pointer is the row or tile (`imageItem`): grabbed when the drag starts, the system drag begins once it is
// ready. Fill the row or tile with it.
// The taps live on this item with the drag: a DragHandler takes the press, so tap handlers on an item under it would
// never see a click.
Item {
    id: root

    /// The id dragged: "wabcut.blur.gaussian", "crossDissolve", "preset:Twirl In", "plugin.clap.<id>".
    property string itemId: ""
    /// The item drawn as the drag image.
    property Item imageItem: null
    /// Off for a plug-in that cannot be used (clicks and the menu still work).
    property bool dragEnabled: true
    readonly property bool dragging: dragHandler.active

    signal tapped()
    signal doubleTapped()
    /// A right-click at (x, y) in this item.
    signal menuRequested(real x, real y)

    Drag.dragType: Drag.Automatic
    Drag.supportedActions: Qt.CopyAction
    Drag.mimeData: { "application/x-wabcut-effect": root.itemId }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: root.tapped()
        onDoubleTapped: root.doubleTapped()
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: point => root.menuRequested(point.position.x, point.position.y)
    }
    DragHandler {
        id: dragHandler
        target: null
        enabled: root.dragEnabled && root.itemId !== ""
        onActiveChanged: {
            if (!active) {
                root.Drag.active = false
                return
            }
            // A window that is not shown (tests) renders nothing to grab: the drag starts without a picture.
            const shown = root.Window.window !== null && root.Window.window.visible
            const picture = root.imageItem
            if (shown && picture && picture.width > 0 && picture.height > 0) {
                const grabbing = picture.grabToImage(result => {
                    root.Drag.imageSource = result.url
                    if (dragHandler.active)
                        root.Drag.active = true
                })
                if (grabbing)
                    return
            }
            root.Drag.active = true
        }
    }
}
