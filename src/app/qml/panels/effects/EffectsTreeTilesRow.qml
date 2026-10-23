pragma ComponentBehavior: Bound
import QtQuick

// A row of the Effects panel's Icons view (UX rework; an EffectsTree row of rowType "tiles"): up to EffectsTree.columns
// items of one folder as tiles (EffectsTreeTile), under the folder's row. The tiles share the panel's width evenly.
Item {
    id: row

    // EffectsTree roles.
    required property string rowKey
    required property int depth
    required property var tiles

    /// The EffectsPanel: tile size, selection and actions.
    property var panel: null

    width: ListView.view ? ListView.view.width : 0
    height: (panel !== null ? panel.tileHeight + panel.tileGap : Theme.space(24))

    Row {
        // Deep folders do not push the tiles out of the panel: two steps of indentation at most.
        x: row.panel !== null ? Math.min(row.depth, 2) * row.panel.indentStep : 0
        y: (row.panel !== null ? row.panel.tileGap : 0) / 2
        spacing: row.panel !== null ? row.panel.tileGap : 0

        Repeater {
            model: row.tiles
            delegate: EffectsTreeTile {
                required property var modelData
                item: modelData
                panel: row.panel
                width: row.panel !== null ? row.panel.tileWidth : Theme.space(22)
                height: row.panel !== null ? row.panel.tileHeight : Theme.space(21)
            }
        }
    }
}
