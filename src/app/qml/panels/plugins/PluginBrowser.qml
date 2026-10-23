pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// The Audio Plug-ins folder's tools in the Effects panel (A.6; UX rework, bridge `Plugins`), shown under the folder's
// row while it is open: the grouping of its sub-folders (by vendor or by category, remembered), Rescan, the plug-in
// settings (folders, the blocklist with "Try again"), the scan's progress, the notice about plug-ins that crashed or
// hung while scanned (they go on the blocklist), and what to do when there are none. The plug-ins themselves are rows
// of the panel's tree (EffectsTree): searched, dragged, favourited, put in custom bins and applied like any effect.
Column {
    id: root
    objectName: "pluginBrowser"
    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Audio plug-ins")

    /// Rescan's outcome, for the panel's status line.
    signal status(string message)

    spacing: Theme.space(1.5)

    RowLayout {
        width: root.width
        spacing: Theme.space(1)
        visible: Plugins.available

        PillTabs {
            id: grouping
            objectName: "pluginGrouping"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            visible: Plugins.pluginCount > 0
            compact: true
            model: [qsTr("By vendor"), qsTr("By category")]
            currentIndex: Plugins.groupBy === "category" ? 1 : 0
            Accessible.name: qsTr("Group audio plug-ins by")
            onActivated: index => {
                Plugins.groupBy = index === 1 ? "category" : "vendor"
                grouping.currentIndex = Qt.binding(() => Plugins.groupBy === "category" ? 1 : 0)
            }
        }
        Item {
            Layout.fillWidth: true
            visible: !grouping.visible
        }
        IconButton {
            objectName: "pluginRescan"
            enabled: !Plugins.scanning
            compact: true
            iconName: "rotate-cw"
            iconSize: Theme.iconSm
            tooltip: qsTr("Rescan audio plug-ins")
            onClicked: {
                const message = Plugins.rescan()
                root.status(message !== "" ? message : qsTr("Scanning the audio plug-ins"))
            }
        }
        IconButton {
            objectName: "pluginSettings"
            compact: true
            iconName: "settings"
            iconSize: Theme.iconSm
            tooltip: qsTr("Plug-in folders and the blocklist")
            onClicked: Plugins.openSettings()
        }
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

    // ---- Blocked plug-ins (crashed or hung while scanned) ----
    Text {
        id: blocked
        objectName: "pluginBlockedNotice"
        width: root.width
        visible: Plugins.available && Plugins.blocked.length > 0
        wrapMode: Text.Wrap
        text: Plugins.blocked.length === 1 ? qsTr("1 plug-in blocked: it crashed or hung while scanned. Review…")
              : qsTr("%1 plug-ins blocked: they crashed or hung while scanned. Review…").arg(Plugins.blocked.length)
        color: blockedHover.hovered || blocked.activeFocus ? Theme.textPrimary : Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.underline: blockedHover.hovered
        leftPadding: Theme.iconSm + Theme.space(1.5)
        activeFocusOnTab: true
        Accessible.role: Accessible.Link
        Accessible.name: text
        Accessible.onPressAction: Plugins.openSettings()
        // Space (Play/Stop) and Enter (Render) are the keymap's elsewhere: here they open the settings.
        Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Space || event.key === Qt.Key_Return
                                                           || event.key === Qt.Key_Enter
        Keys.onReturnPressed: Plugins.openSettings()
        Keys.onEnterPressed: Plugins.openSettings()
        Keys.onSpacePressed: Plugins.openSettings()

        Icon {
            anchors.left: parent.left
            y: (blocked.fontInfo.pixelSize * 1.35 - height) / 2
            name: "triangle-alert"
            size: Theme.iconSm
            color: Theme.warning
        }
        HoverHandler {
            id: blockedHover
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            onTapped: Plugins.openSettings()
        }
        FocusRing {
            targetRadius: Theme.radiusBadge
            visible: blocked.activeFocus
        }
    }

    // ---- Empty states ----
    Text {
        objectName: "pluginsUnavailable"
        width: root.width
        visible: !Plugins.available
        wrapMode: Text.Wrap
        text: qsTr("Audio plug-ins (CLAP and VST3) are not available in this build.")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    Text {
        objectName: "pluginsNone"
        width: root.width
        visible: Plugins.available && !Plugins.scanning && Plugins.pluginCount === 0
        wrapMode: Text.Wrap
        text: qsTr("No audio plug-ins found. Install CLAP or VST3 effects, or add their folder in the plug-in settings.")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
}
