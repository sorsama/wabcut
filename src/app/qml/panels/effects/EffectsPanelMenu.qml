pragma ComponentBehavior: Bound
import QtQuick

// The Effects panel's menus (0.2; UX rework, docs/effects.md "The Effects panel"):
//   * the panel menu (…): New Custom Bin (Ctrl+/), New Presets Bin, Import Presets…, Export Presets… (every preset of
//     yours; off while you have none), Expand All, Collapse All, Show Audio Plug-ins and the plug-in settings;
//   * the filter menu (funnel): All, Video, Audio, Transitions, Presets, Favorites, Audio Plug-ins.
// What needs the panel (a new bin to name in place, the file dialogs, the folders) is asked for with the signals, after
// the menu has closed so focus goes where the action puts it.
Item {
    id: root

    /// The filter shown checked: "all", "video", "audio", "transitions", "presets", "favorites" or "plugins".
    property string filter: "all"
    /// The Audio Plug-ins folder can show (plug-ins are hosted in this build).
    readonly property bool pluginsAvailable: Plugins.available

    signal filterChosen(string key)
    signal newCustomBinRequested()
    signal newPresetsBinRequested()
    signal importRequested()
    signal exportRequested()
    signal expandAllRequested()
    signal collapseAllRequested()

    function popupAt(target: Item, x: real, y: real) {
        menu.popup(target, x, y)
    }
    function popupFilter(target: Item, x: real, y: real) {
        filterMenu.popup(target, x, y)
    }

    // Runs after the menu has closed (a dialog or an in-place rename then keeps the focus).
    property var pending: null
    function later(action: var) {
        root.pending = action
    }

    ContextMenu {
        id: menu
        objectName: "effectsPanelMenu"
        onClosed: {
            const run = root.pending
            root.pending = null
            if (run)
                Qt.callLater(run)
        }

        ContextMenuItem {
            objectName: "newCustomBinItem"
            text: qsTr("New Custom Bin")
            iconName: "folder-plus"
            command: "effects.newCustomBin"
            onTriggered: root.later(() => root.newCustomBinRequested())
        }
        ContextMenuItem {
            objectName: "newPresetsBinItem"
            text: qsTr("New Presets Bin")
            iconName: "folder-plus"
            onTriggered: root.later(() => root.newPresetsBinRequested())
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            objectName: "importPresetsItem"
            text: qsTr("Import Presets…")
            iconName: "import"
            onTriggered: root.later(() => root.importRequested())
        }
        ContextMenuItem {
            objectName: "exportPresetsItem"
            text: qsTr("Export Presets…")
            iconName: "upload"
            enabled: EffectPresets.userPresetCount > 0
            onTriggered: root.later(() => root.exportRequested())
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            objectName: "expandAllItem"
            text: qsTr("Expand All")
            iconName: "unfold-vertical"
            onTriggered: root.expandAllRequested()
        }
        ContextMenuItem {
            objectName: "collapseAllItem"
            text: qsTr("Collapse All")
            iconName: "fold-vertical"
            onTriggered: root.collapseAllRequested()
        }
        ContextMenuSeparator {
            visible: root.pluginsAvailable
            height: visible ? implicitHeight : 0
        }
        ContextMenuItem {
            id: showPlugins
            objectName: "showPluginsItem"
            visible: root.pluginsAvailable
            height: visible ? implicitHeight : 0
            text: qsTr("Show Audio Plug-ins")
            checkable: true
            checked: Preferences.effectsShowPlugins
            onTriggered: {
                Preferences.effectsShowPlugins = !Preferences.effectsShowPlugins
                showPlugins.checked = Qt.binding(() => Preferences.effectsShowPlugins)
            }
        }
        ContextMenuItem {
            objectName: "pluginSettingsItem"
            visible: root.pluginsAvailable
            height: visible ? implicitHeight : 0
            text: qsTr("Audio Plug-in Settings…")
            iconName: "settings"
            onTriggered: root.later(() => Plugins.openSettings())
        }
    }

    ContextMenu {
        id: filterMenu
        objectName: "effectsFilterMenu"

        FilterItem { key: "all"; text: qsTr("All"); current: root.filter; onChosen: key => root.filterChosen(key) }
        ContextMenuSeparator {}
        FilterItem { key: "video"; text: qsTr("Video"); current: root.filter; onChosen: key => root.filterChosen(key) }
        FilterItem { key: "audio"; text: qsTr("Audio"); current: root.filter; onChosen: key => root.filterChosen(key) }
        FilterItem { key: "transitions"; text: qsTr("Transitions"); current: root.filter; onChosen: key => root.filterChosen(key) }
        FilterItem { key: "presets"; text: qsTr("Presets"); current: root.filter; onChosen: key => root.filterChosen(key) }
        FilterItem { key: "favorites"; text: qsTr("Favorites"); current: root.filter; onChosen: key => root.filterChosen(key) }
        FilterItem {
            key: "plugins"
            text: qsTr("Audio Plug-ins")
            current: root.filter
            visible: root.pluginsAvailable && Preferences.effectsShowPlugins
            height: visible ? implicitHeight : 0
            onChosen: key => root.filterChosen(key)
        }
    }

    // One radio choice of the filter menu: checked while `current` is its key (also after a click).
    component FilterItem: ContextMenuItem {
        id: choice
        property string key: ""
        property string current: ""
        signal chosen(string key)
        objectName: "effectsFilter_" + key
        checkable: true
        checked: current === key
        onTriggered: {
            choice.chosen(choice.key)
            choice.checked = Qt.binding(() => choice.current === choice.key)
        }
    }
}
