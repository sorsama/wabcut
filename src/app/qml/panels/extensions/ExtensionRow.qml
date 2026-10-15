pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// One extension in the Extensions manager (C.12, M5n; a row of Extensions.extensions): its name, version, author and
// licence, a Project micro-tag for one that came with the project, the status badge (On, Off, Review needed, Not
// trusted, Error, Broken), the capabilities it was granted as chips (an update that asks for more is marked), its
// commands with their shortcuts, why it is off or broken, a crash badge, the actions its status allows, and its log.
// Review… / Enable… / Trust… ask the manager for the permission dialog (reviewRequested); the rest act at once
// (Uninstall asks first, in the row). The rows are rebuilt when the list changes, so whether the log is open and
// whether Uninstall is being confirmed are the manager's (showLog, confirmingUninstall; the row asks with logToggled
// and uninstallAsked).
Rectangle {
    id: root

    required property var modelData
    required property int index

    readonly property var ext: modelData
    readonly property bool on: ext.badgeKind === "on"
    readonly property bool user: !ext.project
    property bool showLog: false
    property bool confirmingUninstall: false
    property string error: ""

    /// Review / enable / trust: the manager opens the permission dialog for it.
    signal reviewRequested(string id)
    signal logToggled(string id, bool open)
    signal uninstallAsked(string id, bool asking)

    objectName: "extensionRow_" + ext.id
    implicitHeight: column.implicitHeight + Theme.space(5)
    radius: Theme.radiusField
    color: Theme.sunken
    border.width: Theme.hairline
    border.color: root.ext.badgeKind === "error" || root.ext.badgeKind === "broken" ? Theme.danger : Theme.border

    Accessible.role: Accessible.ListItem
    Accessible.name: qsTr("%1 %2, %3").arg(root.ext.name).arg(root.ext.version).arg(root.ext.badge)

    // A micro-tag: a short upper-case label on a tinted chip.
    component Tag: Rectangle {
        property string label: ""
        property color tint: Theme.textTertiary
        property bool filled: true
        implicitWidth: tagText.implicitWidth + Theme.space(2)
        implicitHeight: tagText.implicitHeight + Theme.space(1)
        radius: Theme.radiusBadge
        color: filled ? tint : "transparent"
        border.width: filled ? 0 : Theme.hairline
        border.color: tint
        Accessible.role: Accessible.StaticText
        Accessible.name: label
        Text {
            id: tagText
            anchors.centerIn: parent
            text: parent.label
            color: parent.filled ? Theme.canvas : parent.tint
            font.family: Theme.fontSans
            font.capitalization: Font.AllUppercase
            font.pixelSize: Theme.textMicro
            font.letterSpacing: Theme.microTracking
            font.weight: Font.DemiBold
        }
    }
    component Caption: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
        font.weight: Font.Medium
    }

    function act(error) {
        root.error = error ?? ""
    }

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.space(2.5)
        spacing: Theme.space(1.5)

        // Name, version, scope, status.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Icon {
                name: "puzzle"
                size: Theme.iconSm
                color: root.on ? Theme.accentStroke : Theme.textTertiary
            }
            Text {
                Layout.fillWidth: true
                text: root.ext.name
                elide: Text.ElideRight
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Font.DemiBold
            }
            Text {
                visible: root.ext.version !== ""
                text: root.ext.version
                color: Theme.textTertiary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
            }
            Tag {
                objectName: "extensionProjectTag"
                visible: root.ext.project
                //: Micro-tag: the extension came with the project
                label: qsTr("Project")
                tint: Theme.info
                filled: false
            }
            Tag {
                objectName: "extensionCrashTag"
                visible: root.ext.crashes > 0 || root.ext.crashed
                //: Micro-tag on an extension that crashed (n crashes in a row)
                label: root.ext.crashes > 1 ? qsTr("Crashed ×%1").arg(root.ext.crashes) : qsTr("Crashed")
                tint: Theme.danger
            }
            Tag {
                objectName: "extensionStatus"
                label: root.ext.badge
                tint: root.ext.badgeKind === "on" ? Theme.success
                    : root.ext.badgeKind === "review" || root.ext.badgeKind === "trust" ? Theme.warning
                    : root.ext.badgeKind === "off" ? Theme.textTertiary : Theme.danger
            }
        }
        Text {
            Layout.fillWidth: true
            visible: text !== ""
            text: [root.ext.author, root.ext.licence].filter(s => s !== "").join(" · ")
            elide: Text.ElideRight
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Text {
            Layout.fillWidth: true
            visible: text !== ""
            text: root.ext.description
            wrapMode: Text.Wrap
            maximumLineCount: 3
            elide: Text.ElideRight
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        // Granted capabilities.
        Flow {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            visible: root.ext.granted.length > 0 || root.ext.needsReview
            Repeater {
                model: root.ext.granted
                delegate: Rectangle {
                    id: chip
                    required property var modelData
                    implicitWidth: chipText.implicitWidth + Theme.space(3)
                    implicitHeight: chipText.implicitHeight + Theme.space(1)
                    radius: Theme.radiusPill
                    color: Theme.accentWash
                    border.width: Theme.hairline
                    border.color: Theme.accentStroke
                    Accessible.role: Accessible.StaticText
                    Accessible.name: chipText.text
                    Text {
                        id: chipText
                        anchors.centerIn: parent
                        text: chip.modelData.title
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                    }
                }
            }
            Tag {
                objectName: "extensionNeedsReview"
                visible: root.ext.needsReview && root.on
                //: Micro-tag: an update of the extension asks for more permissions than it was given
                label: qsTr("Asks for more")
                tint: Theme.warning
                filled: false
            }
        }

        // Commands.
        Caption {
            visible: root.ext.commands.length > 0
            text: qsTr("%n command(s)", "", root.ext.commands.length)
        }
        Repeater {
            model: root.ext.commands
            delegate: RowLayout {
                id: commandRow
                required property var modelData
                Layout.fillWidth: true
                spacing: Theme.space(2)
                Text {
                    Layout.fillWidth: true
                    text: commandRow.modelData.title
                    elide: Text.ElideRight
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                Text {
                    text: commandRow.modelData.shortcut
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
            }
        }

        // Why it is off, failed or broken.
        Text {
            objectName: "extensionReason"
            Layout.fillWidth: true
            visible: text !== ""
            text: root.ext.reason
            wrapMode: Text.Wrap
            color: root.ext.badgeKind === "error" || root.ext.badgeKind === "broken" || root.ext.crashed ? Theme.danger
                                                                                                         : Theme.textSecondary
            font.family: root.ext.badgeKind === "broken" || root.ext.badgeKind === "error" ? Theme.fontMono : Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Text {
            Layout.fillWidth: true
            visible: root.error !== ""
            text: root.error
            wrapMode: Text.Wrap
            color: Theme.danger
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        // Actions (docs/internal/c12-extensions-handoff.md: the status table).
        RowLayout {
            Layout.fillWidth: true
            visible: !root.confirmingUninstall
            spacing: Theme.space(1)
            PillButton {
                objectName: "extensionReview"
                visible: root.ext.badgeKind === "review" || (root.on && root.ext.needsReview)
                compact: true
                variant: "primary"
                iconName: "shield-check"
                text: qsTr("Review…")
                tooltip: qsTr("What it asks for, and what you allow")
                onClicked: root.reviewRequested(root.ext.id)
            }
            PillButton {
                objectName: "extensionTrust"
                visible: root.ext.badgeKind === "trust"
                compact: true
                variant: "primary"
                iconName: "shield-check"
                text: qsTr("Trust…")
                tooltip: qsTr("It came with this project: check it before it runs")
                onClicked: root.reviewRequested(root.ext.id)
            }
            PillButton {
                objectName: "extensionEnable"
                visible: root.ext.badgeKind === "off" || root.ext.badgeKind === "error"
                compact: true
                variant: "primary"
                iconName: root.ext.badgeKind === "error" ? "rotate-cw" : "power"
                text: root.ext.badgeKind === "error" ? qsTr("Retry…") : qsTr("Enable…")
                onClicked: root.reviewRequested(root.ext.id)
            }
            PillButton {
                objectName: "extensionDisable"
                visible: root.on
                compact: true
                iconName: "power"
                text: qsTr("Disable")
                tooltip: qsTr("Stop it; its permissions stay")
                onClicked: root.act(Extensions.disable(root.ext.id))
            }
            PillButton {
                objectName: "extensionRevoke"
                visible: root.on || root.ext.badgeKind === "off" && root.ext.granted.length > 0
                compact: true
                variant: "ghost"
                text: qsTr("Revoke")
                tooltip: qsTr("Take every permission back and turn it off")
                onClicked: root.act(Extensions.revoke(root.ext.id))
            }
            PillButton {
                objectName: "extensionDistrust"
                visible: root.ext.project && root.on
                compact: true
                variant: "ghost"
                text: qsTr("Stop trusting")
                tooltip: qsTr("It will not run with this project until you trust it again")
                onClicked: root.act(Extensions.distrust(root.ext.id))
            }
            Item { Layout.fillWidth: true }
            IconButton {
                objectName: "extensionShowLog"
                visible: root.ext.badgeKind !== "broken"
                compact: true
                checkable: true
                checked: root.showLog
                iconName: "list"
                tooltip: root.showLog ? qsTr("Hide the log") : qsTr("Show the log")
                onClicked: {
                    root.logToggled(root.ext.id, !root.showLog)
                    checked = Qt.binding(() => root.showLog)
                }
            }
            IconButton {
                objectName: "extensionUninstall"
                visible: root.user
                compact: true
                iconName: "trash-2"
                tooltip: root.ext.badgeKind === "broken" ? qsTr("Remove the folder") : qsTr("Uninstall")
                onClicked: root.uninstallAsked(root.ext.id, true)
            }
        }
        // Uninstall asks first: the folder, the permissions and the stored data go.
        RowLayout {
            Layout.fillWidth: true
            visible: root.confirmingUninstall
            spacing: Theme.space(1)
            Text {
                Layout.fillWidth: true
                text: qsTr("Uninstall %1? Its folder, permissions and stored data are removed.").arg(root.ext.name)
                wrapMode: Text.Wrap
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            PillButton {
                compact: true
                variant: "ghost"
                text: qsTr("Cancel")
                onClicked: root.uninstallAsked(root.ext.id, false)
            }
            PillButton {
                objectName: "extensionConfirmUninstall"
                compact: true
                variant: "primary"
                text: qsTr("Uninstall")
                onClicked: {
                    root.uninstallAsked(root.ext.id, false)
                    root.act(Extensions.uninstall(root.ext.id))
                }
            }
        }

        // The log (the last 500 lines).
        Rectangle {
            id: logBox
            objectName: "extensionLog"
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.space(32)
            visible: root.showLog
            radius: Theme.radiusField
            color: Theme.canvas
            border.width: Theme.hairline
            border.color: Theme.border

            property var lines: []
            function reload() {
                lines = Extensions.log(root.ext.id)
                if (logView.count > 0)
                    logView.positionViewAtEnd()
            }
            onVisibleChanged: {
                if (visible)
                    reload()
            }
            Connections {
                target: Extensions
                enabled: root.showLog
                function onLogged(id) {
                    if (id === root.ext.id)
                        logBox.reload()
                }
            }

            Text {
                anchors.centerIn: parent
                visible: logBox.lines.length === 0
                text: qsTr("Nothing logged yet")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            ListView {
                id: logView
                anchors.fill: parent
                anchors.margins: Theme.space(1.5)
                clip: true
                model: logBox.lines
                boundsBehavior: Flickable.StopAtBounds
                delegate: Text {
                    required property var modelData
                    width: ListView.view ? ListView.view.width : 0
                    text: modelData.message
                    wrapMode: Text.WrapAnywhere
                    color: modelData.level === "error" ? Theme.danger
                         : modelData.level === "warning" || modelData.level === "warn" ? Theme.warning : Theme.textSecondary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
            }
        }
    }
}
