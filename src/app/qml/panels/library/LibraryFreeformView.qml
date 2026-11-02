pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// The Library's Freeform view (0.2, P.3: Premiere's Project panel Freeform view). Thumbnails sit where they were put
// (Library.freeform, saved in the project per bin): drag cards to move them (onto others to stack them), Ctrl+wheel
// over a stack brings its next card to the front (over the empty canvas it changes the clip size), drag on the empty
// canvas to select with a rectangle, and the canvas menu has Reset to Grid, Align to Grid, the clip size, saved
// layouts and the two metadata lines under each card. Cards still drag onto the timeline and into bins.
Flickable {
    id: root
    objectName: "libraryFreeform"

    signal activated(string key, string kind)
    signal menuRequested(string key, string kind, Item target, point position, real fraction)
    signal saveLayoutRequested()
    /// P1 round (#40): a card's usage badge was clicked (the panel lists where the item is used).
    signal usageRequested(string key, Item target, point position)
    /// P1 round (#41): a bin card drags inside the Library only (LibraryCard.binDragMoved); on release the panel moves
    /// it into the bin under the pointer, else by (`dx`, `dy`) on the canvas like any card.
    signal binDragMoved(point scenePosition)
    signal binDragFinished(point scenePosition, real dx, real dy)

    /// P1 round (#41): the bin card a Library drag of bins would drop into ("none": none).
    property string dropTarget: "none"
    /// The key of the front card under the scene point ("" = none).
    function keyAt(sceneX: real, sceneY: real): string {
        const p = canvas.mapFromItem(null, sceneX, sceneY)
        const inView = root.mapFromItem(null, sceneX, sceneY)
        if (inView.x < 0 || inView.y < 0 || inView.x >= root.width || inView.y >= root.height)
            return ""
        let found = ""
        let front = -Infinity
        const places = Library.freeform
        for (const key in places) {
            const place = places[key]
            const w = root.cardWidth * place.size
            const h = Math.round(w * 9 / 16) + Theme.space(16)
            if (p.x >= place.x && p.x < place.x + w && p.y >= place.y && p.y < place.y + h && place.z > front) {
                found = key
                front = place.z
            }
        }
        return found
    }

    /// Where the card being dragged was pressed, in canvas coordinates (the drop moves the selection by the difference).
    property point dragOrigin: Qt.point(-1, -1)
    property bool dragFromHere: false
    readonly property real cardWidth: Library.freeformCardWidth * Library.freeformScale
    readonly property real margin: Theme.space(6)
    // The canvas covers every card.
    readonly property var extent: {
        let right = 0
        let bottom = 0
        const places = Library.freeform
        for (const key in places) {
            const p = places[key]
            right = Math.max(right, p.x + root.cardWidth * p.size)
            bottom = Math.max(bottom, p.y + (root.cardWidth * p.size) * 9 / 16 + Theme.space(16))
        }
        return { right: right, bottom: bottom }
    }

    clip: true
    contentWidth: Math.max(width, extent.right + margin)
    contentHeight: Math.max(height, extent.bottom + margin)
    boundsBehavior: Flickable.StopAtBounds
    interactive: false // drags move cards or draw the selection rectangle; the wheel and the bars scroll
    ScrollBar.vertical: ThinScrollBar {}
    ScrollBar.horizontal: ThinScrollBar {}
    Accessible.role: Accessible.List
    Accessible.name: qsTr("Library, freeform: %n item(s)", "", Library.items.count)

    // Empty canvas: click clears the selection, a drag draws the selection rectangle, right-click opens the menu.
    Item {
        id: canvas
        width: root.contentWidth
        height: root.contentHeight

        TapHandler {
            acceptedButtons: Qt.LeftButton
            onTapped: Library.clearSelection()
        }
        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: eventPoint => freeformMenu.popup(canvas, eventPoint.position.x, eventPoint.position.y)
        }
        DragHandler {
            id: marquee
            objectName: "libraryFreeformMarquee"
            target: null
            acceptedButtons: Qt.LeftButton
            // P1 round (#41): a card's own drag (a pointer handler) keeps the pointer; the rectangle never takes it over.
            grabPermissions: PointerHandler.CanTakeOverFromItems | PointerHandler.ApprovesTakeOverByAnything
            onActiveChanged: {
                if (active)
                    return
                // Select the cards the rectangle touches (Ctrl/Shift add to the selection).
                const r = band.rect()
                const add = (centroid.modifiers & (Qt.ControlModifier | Qt.ShiftModifier | Qt.MetaModifier)) !== 0
                if (!add)
                    Library.clearSelection()
                const places = Library.freeform
                for (const key in places) {
                    const p = places[key]
                    const w = root.cardWidth * p.size
                    const h = Math.round(w * 9 / 16) + Theme.space(16)
                    if (p.x < r.x + r.width && p.x + w > r.x && p.y < r.y + r.height && p.y + h > r.y)
                        Library.select(key, Library.Add)
                }
            }
        }
        Rectangle {
            id: band
            function rect() {
                const a = marquee.centroid.pressPosition
                const b = marquee.centroid.position
                return Qt.rect(Math.min(a.x, b.x), Math.min(a.y, b.y), Math.abs(a.x - b.x), Math.abs(a.y - b.y))
            }
            visible: marquee.active
            x: rect().x
            y: rect().y
            width: rect().width
            height: rect().height
            z: 100000
            color: Theme.accentWash
            border.width: Theme.hairline
            border.color: Theme.accentStroke
        }

        // The canvas does not flick (drags move cards or select): the wheel scrolls it, Shift sideways, and Ctrl+wheel
        // changes the clip size.
        WheelHandler {
            objectName: "libraryFreeformZoom"
            onWheel: event => {
                if ((event.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) !== 0) {
                    Library.setFreeformScale(Library.freeformScale + (event.angleDelta.y > 0 ? 0.25 : -0.25))
                    return
                }
                const horizontal = (event.modifiers & Qt.ShiftModifier) !== 0
                const delta = event.angleDelta.y !== 0 ? event.angleDelta.y : event.angleDelta.x
                if (horizontal)
                    root.contentX = Math.max(0, Math.min(root.contentWidth - root.width, root.contentX - delta))
                else
                    root.contentY = Math.max(0, Math.min(root.contentHeight - root.height, root.contentY - delta))
            }
        }

        Repeater {
            id: cards
            // Only while the view is shown: a hidden Freeform view would otherwise hold a card (and load a thumbnail)
            // for every item of the bin next to the grid's.
            model: root.visible ? Library.items : null

            delegate: Item {
                id: cell
                required property int index
                required property string key
                required property string kind
                required property string name
                required property int label
                required property string thumbnail
                required property string durationText
                required property string meta
                required property bool offline
                required property string proxyState
                required property real proxyProgress
                required property bool generated
                required property var subclip
                required property bool selected
                required property bool filmstrip
                required property int itemCount
                required property string binPath
                required property string derivedFrom
                required property bool searchBin
                required property var marks // UX rework (stream T): the In/Out band
                required property bool cut // P1 round (#41)
                required property int uses // P1 round (#40)
                required property bool unused
                required property int masterEffects // 0.6 (#71): the fx badge

                readonly property var place: Library.freeform[key] ?? ({ x: 0, y: 0, size: 1, z: 0 })
                x: place.x
                y: place.y
                z: place.z
                width: root.cardWidth * place.size
                height: Math.round(width * 9 / 16) + Theme.space(16)
                objectName: "libraryFreeformCell_" + key

                LibraryCard {
                    id: card
                    anchors.fill: parent
                    index: cell.index
                    key: cell.key
                    kind: cell.kind
                    name: cell.name
                    label: cell.label
                    thumbnail: cell.thumbnail
                    durationText: cell.durationText
                    meta: cell.meta
                    offline: cell.offline
                    proxyState: cell.proxyState
                    proxyProgress: cell.proxyProgress
                    generated: cell.generated
                    subclip: cell.subclip
                    selected: cell.selected
                    filmstrip: cell.filmstrip
                    itemCount: cell.itemCount
                    binPath: cell.binPath
                    derivedFrom: cell.derivedFrom
                    searchBin: cell.searchBin
                    marks: cell.marks
                    cut: cell.cut
                    uses: cell.uses
                    unused: cell.unused
                    masterEffects: cell.masterEffects
                    onUsageRequested: position => root.usageRequested(cell.key, card, position)
                    dropHighlight: root.dropTarget === cell.key
                    metaLines: ProjectController.revision >= 0
                               ? Library.freeformLines.map(id => id !== "" ? Library.cellText(cell.key, id) : "") : []
                    onActivated: root.activated(cell.key, cell.kind)
                    onMenuRequested: point => root.menuRequested(cell.key, cell.kind, card, point, card.scrubFraction)
                    onDragStarted: position => {
                        root.dragOrigin = Qt.point(cell.x + position.x, cell.y + position.y)
                        root.dragFromHere = true
                    }
                    onBinDragMoved: scenePosition => root.binDragMoved(scenePosition)
                    onBinDragFinished: scenePosition => {
                        const at = canvas.mapFromItem(null, scenePosition.x, scenePosition.y)
                        const dx = root.dragOrigin.x < 0 ? 0 : at.x - root.dragOrigin.x
                        const dy = root.dragOrigin.y < 0 ? 0 : at.y - root.dragOrigin.y
                        root.dragFromHere = false
                        root.dragOrigin = Qt.point(-1, -1)
                        root.binDragFinished(scenePosition, dx, dy)
                    }
                }
                // Ctrl+wheel over a stack: its next card to the front.
                WheelHandler {
                    acceptedModifiers: Qt.ControlModifier
                    onWheel: event => {
                        if (Library.freeformStackOf(cell.key).length > 1)
                            Library.cycleFreeformStack(cell.key, event.angleDelta.y > 0 ? 1 : -1)
                        else
                            Library.setFreeformScale(Library.freeformScale + (event.angleDelta.y > 0 ? 0.25 : -0.25))
                    }
                }
            }
        }

        // Cards dropped on the canvas move by the drag (the selection with them); dropped on a bin card they go into it.
        DropArea {
            objectName: "libraryFreeformDrop"
            anchors.fill: parent
            keys: ["application/x-wabcut-library"]
            onDropped: drop => {
                if (!root.dragFromHere || root.dragOrigin.x < 0) {
                    drop.accept(Qt.IgnoreAction)
                    return
                }
                const keys = drop.getDataAsString("application/x-wabcut-library").split(",")
                Library.moveFreeform(keys, drop.x - root.dragOrigin.x, drop.y - root.dragOrigin.y)
                root.dragFromHere = false
                root.dragOrigin = Qt.point(-1, -1)
                drop.accept(Qt.MoveAction)
            }
        }
    }

    FreeformMenu {
        id: freeformMenu
        canvasWidth: root.width
        onSaveLayoutRequested: root.saveLayoutRequested()
    }
}
