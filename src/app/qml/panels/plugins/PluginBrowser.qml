pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// "Audio Plugins" in the Effects panel (A.6, bridge `Plugins`): the CLAP and VST3 effects this computer has, scanned
// by the helper (a plugin that crashes or hangs while scanned goes on the blocklist instead), grouped by vendor or
// category and filtered by the panel's search. Rows drag onto clips, Effect Controls and mixer strips like any
// effect. The header rescans and opens the settings (folders, the blocklist with "Try again").
Column {
    id: root
    objectName: "pluginBrowser"
    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Audio plugins")

    /// The Effects panel's search text.
    property string query: ""

    /// A row's double-click outcome, for the panel's status line.
    signal status(string message)

    spacing: Theme.space(1)

    Binding {
        target: Plugins
        property: "query"
        value: root.query
    }

    // ---- Header: title, count, rescan, settings ----
    RowLayout {
        width: root.width
        spacing: Theme.space(1.5)

        Icon {
            name: "sliders-horizontal"
            size: Theme.iconXs
            color: Theme.textTertiary
        }
        Text {
            Layout.fillWidth: true
            text: Plugins.available && Plugins.pluginCount > 0 ? qsTr("Audio Plugins · %1").arg(Plugins.pluginCount) : qsTr("Audio Plugins")
            elide: Text.ElideRight
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Font.DemiBold
            font.letterSpacing: Theme.labelTracking
        }
        IconButton {
            objectName: "pluginRescan"
            visible: Plugins.available
            enabled: !Plugins.scanning
            compact: true
            iconName: "rotate-cw"
            iconSize: Theme.iconSm
            tooltip: qsTr("Rescan audio plugins")
            onClicked: root.status(Plugins.rescan())
        }
        IconButton {
            objectName: "pluginSettings"
            visible: Plugins.available
            compact: true
            iconName: "settings"
            iconSize: Theme.iconSm
            tooltip: qsTr("Plugin folders and the blocklist")
            onClicked: Plugins.openSettings()
        }
    }

    // Grouping, on its own row so the title keeps its room in a narrow panel.
    PillTabs {
        id: grouping
        objectName: "pluginGrouping"
        width: root.width
        visible: Plugins.available && Plugins.pluginCount > 0
        compact: true
        model: [qsTr("By vendor"), qsTr("By category")]
        currentIndex: Plugins.groupBy === "category" ? 1 : 0
        onCurrentIndexChanged: Plugins.groupBy = currentIndex === 1 ? "category" : "vendor"
        Accessible.name: qsTr("Group audio plugins by")
    }

    // ---- Scanning ----
    BeamProgress {
        objectName: "pluginScanProgress"
        width: root.width
        height: Theme.hairline * 3
        visible: Plugins.scanning
        indeterminate: Plugins.progress < 0
        value: Math.max(0, Plugins.progress)
        active: Plugins.scanning
        label: Plugins.scanText
    }

    // ---- Blocked plugins (crashed or hung while scanned) ----
    AbstractButtonLink {
        objectName: "pluginBlockedNotice"
        width: root.width
        visible: Plugins.available && Plugins.blocked.length > 0
        text: qsTr("%n plugin(s) blocked: they crashed or hung while scanned. Review…", "", Plugins.blocked.length)
        onClicked: Plugins.openSettings()
    }

    // ---- Groups ----
    Repeater {
        model: Plugins.available ? Plugins.groups : []

        delegate: Column {
            id: group
            required property var modelData
            width: root.width
            spacing: Theme.space(1) / 2

            Text {
                leftPadding: Theme.space(3)
                text: qsTr("%1 · %2").arg(group.modelData.title).arg(group.modelData.count)
                elide: Text.ElideRight
                width: group.width
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
                font.weight: Font.Medium
            }
            Repeater {
                model: group.modelData.items
                delegate: PluginRow {
                    required property var modelData
                    width: group.width
                    plugin: modelData
                    onApplied: message => root.status(message)
                }
            }
        }
    }

    // ---- Empty states ----
    Text {
        objectName: "pluginsUnavailable"
        width: root.width
        visible: !Plugins.available
        leftPadding: Theme.space(3)
        wrapMode: Text.Wrap
        text: qsTr("Audio plugins (CLAP and VST3) are not available in this build.")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    Text {
        objectName: "pluginsNone"
        width: root.width
        visible: Plugins.available && !Plugins.scanning && Plugins.pluginCount === 0
        leftPadding: Theme.space(3)
        wrapMode: Text.Wrap
        text: qsTr("No audio plugins found. Install CLAP or VST3 effects, or add their folder in the plugin settings.")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    Text {
        objectName: "pluginsNoMatch"
        width: root.width
        visible: Plugins.available && Plugins.pluginCount > 0 && Plugins.plugins.length === 0 && root.query !== ""
        leftPadding: Theme.space(3)
        wrapMode: Text.Wrap
        text: qsTr("No audio plugin matches “%1”").arg(root.query)
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    // A text button (the blocked notice): secondary text, accent on hover.
    component AbstractButtonLink: Text {
        id: link
        signal clicked()
        leftPadding: Theme.space(3)
        wrapMode: Text.Wrap
        color: linkHover.hovered ? Theme.accentStroke : Theme.warning
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        Accessible.role: Accessible.Link
        Accessible.name: text
        Accessible.onPressAction: link.clicked()
        HoverHandler {
            id: linkHover
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            onTapped: link.clicked()
        }
    }
}
