pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// One audio plugin in the Effects panel's browser (A.6): its name, vendor and format. Drag it onto a clip in the
// timeline, onto Effect Controls or onto a mixer strip's effect slots (the same mime as the built-in effects:
// application/x-wabcut-effect = "plugin.<format>.<id>"); a double-click or Enter adds it to the selected audio clip.
AbstractButton {
    id: row

    /// { id, name, vendor, category, format, version, latencyText, hasEditor, problem } (Plugins.plugins).
    required property var plugin

    /// The outcome of a double-click or Enter ("" or the reason), for the panel's status line.
    signal applied(string message)

    function apply() {
        const error = Plugins.applyToSelection(row.plugin.id)
        row.applied(error === "" ? qsTr("Added %1").arg(row.plugin.name) : error)
    }

    objectName: "pluginItem_" + plugin.id
    height: Theme.controlHeight
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    enabled: (plugin.problem ?? "") === ""
    Accessible.role: Accessible.ListItem
    Accessible.name: qsTr("%1 by %2").arg(plugin.name).arg(plugin.vendor)
    Accessible.description: (plugin.problem ?? "") !== "" ? plugin.problem : qsTr("%1 audio plugin").arg(plugin.format)
    onDoubleClicked: apply()
    Keys.onReturnPressed: apply()
    Keys.onEnterPressed: apply()

    contentItem: Item {
        Icon {
            id: icon
            anchors.left: parent.left
            anchors.leftMargin: Theme.space(3)
            anchors.verticalCenter: parent.verticalCenter
            name: "sliders-horizontal"
            size: Theme.iconSm
            color: row.hovered || dragSource.Drag.active ? Theme.accentStroke : Theme.textTertiary
        }
        Text {
            anchors.left: icon.right
            anchors.leftMargin: Theme.space(2)
            anchors.right: meta.left
            anchors.rightMargin: Theme.space(2)
            anchors.verticalCenter: parent.verticalCenter
            text: row.plugin.name
            elide: Text.ElideRight
            color: row.enabled ? Theme.textPrimary : Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        Row {
            id: meta
            anchors.right: parent.right
            anchors.rightMargin: Theme.space(3)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space(1.5)
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: (row.plugin.latencyText ?? "") !== ""
                text: row.plugin.latencyText ?? ""
                color: Theme.textTertiary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
            }
            // Format micro-tag: CLAP or VST3.
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: formatText.implicitWidth + Theme.space(2)
                height: formatText.implicitHeight + Theme.space(1)
                radius: Theme.radiusBadge
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: Theme.border
                Text {
                    id: formatText
                    anchors.centerIn: parent
                    text: row.plugin.format
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.capitalization: Font.AllUppercase
                    font.pixelSize: Theme.textMicro
                    font.letterSpacing: Theme.microTracking
                    font.weight: Font.DemiBold
                }
            }
        }
    }

    background: Rectangle {
        radius: Theme.radiusField
        color: row.down ? Theme.raised : row.hovered ? Theme.hover : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.durFast } }

        FocusRing {
            anchors.margins: 0
            targetRadius: Theme.radiusField
            visible: row.visualFocus
        }
    }

    HoverHandler {
        enabled: row.enabled
        cursorShape: Qt.PointingHandCursor
    }

    Loader {
        active: row.hovered
        sourceComponent: Tooltip {
            parent: row
            visible: true
            text: (row.plugin.problem ?? "") !== "" ? row.plugin.problem
                : qsTr("%1 · %2 %3").arg(row.plugin.vendor).arg(row.plugin.format).arg(row.plugin.version)
        }
    }

    // Drag source: onto a clip in the timeline, Effect Controls or a mixer strip.
    Item {
        id: dragSource
        anchors.fill: parent
        Drag.active: dragHandler.active
        Drag.dragType: Drag.Automatic
        Drag.supportedActions: Qt.CopyAction
        Drag.mimeData: { "application/x-wabcut-effect": row.plugin.id }

        DragHandler {
            id: dragHandler
            target: null
            enabled: row.enabled
        }
    }
}
