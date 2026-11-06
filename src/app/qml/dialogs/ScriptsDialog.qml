pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// File > Run Python Script… (0.7, bridge `Scripts`, docs/python.md). Three steps in one dialog:
//   1. choose a script (a .py file, or one of the recent ones);
//   2. review it: its name and description, the permissions it asks for as switches (Read is always on; a script gets only
//      what it asks for AND the switches allow, like an extension), whether to remember the choice for this exact version
//      of the script, and the Python Wabcut found (3.11 or newer; none is bundled);
//   3. run it: its output and errors stream into the log, Stop ends it and discards what it did. A script that finishes is
//      applied to the project as ONE undo step; one that fails changes nothing.
// The dialog is modal while a script runs, so the project is not edited under it.
WabcutDialog {
    id: dialog
    objectName: "scriptsDialog"

    title: Scripts.state === "running" ? qsTr("Running script")
         : Scripts.state === "finished" ? qsTr("Script finished") : qsTr("Run Python script")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(150), Theme.space(160))
    closePolicy: Scripts.running ? Popup.NoAutoClose : Popup.CloseOnEscape
    /// The most a list of rows may take: what the window has left once the rest of the dialog is there (the smallest
    /// window is 1100 x 700), so nothing runs off the window; a longer list scrolls.
    readonly property real listLimit: Math.max(Theme.space(30), (parent ? parent.height : Theme.space(175)) - Theme.space(95))

    /// Shows the dialog on its first step (or on the chosen script's, when one is chosen already).
    function openDialog() {
        if (Scripts.state === "finished")
            Scripts.reset()
        open()
    }
    /// A chosen file (the file dialog's, or a test's).
    function pick(url) {
        Scripts.choose(url)
    }
    function pickPython(url) {
        Scripts.choosePython(url)
    }

    // ---- 1. Choose --------------------------------------------------------------------------------------------------
    ColumnLayout {
        objectName: "scriptsChoose"
        Layout.fillWidth: true
        visible: Scripts.state === "idle"
        spacing: Theme.space(3)

        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            text: qsTr("A script is Python code that uses Wabcut's commands on the open project. It runs on this computer with your Python. Everything it changes becomes one undo step, and nothing is kept if it fails or you stop it.")
        }
        PillButton {
            objectName: "scriptsChooseButton"
            Layout.alignment: Qt.AlignLeft
            variant: "primary"
            iconName: "folder"
            text: qsTr("Choose script…")
            onClicked: scriptDialog.open()
        }
        Text {
            objectName: "scriptsProblem"
            Layout.fillWidth: true
            visible: Scripts.problem !== ""
            text: Scripts.problem
            wrapMode: Text.Wrap
            color: Theme.danger
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Text {
            Layout.fillWidth: true
            visible: Scripts.recent.length > 0
            text: qsTr("Recent")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Font.Medium
        }
        ScrollView {
            id: recentScroll
            objectName: "scriptsRecentList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(recentList.implicitHeight, dialog.listLimit)
            visible: Scripts.recent.length > 0
            contentWidth: availableWidth
            clip: true
            ScrollBar.vertical: ThinScrollBar {}
            ColumnLayout {
                id: recentList
                width: recentScroll.availableWidth
                spacing: Theme.space(2)
                Repeater {
                    model: Scripts.recent
                    delegate: Rectangle {
                        id: recentRow
                        required property var modelData
                        required property int index
                        objectName: "scriptsRecent_" + index
                        Layout.fillWidth: true
                        implicitHeight: recentColumn.implicitHeight + Theme.space(3)
                        radius: Theme.radiusField
                        color: recentArea.containsMouse ? Theme.hover : Theme.sunken
                        border.width: Theme.hairline
                        border.color: Theme.border
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Run %1").arg(recentRow.modelData.name)
                        Accessible.description: recentRow.modelData.folder
                        Accessible.onPressAction: Scripts.chooseRecent(recentRow.modelData.path)
                        activeFocusOnTab: true
                        Keys.onReturnPressed: Scripts.chooseRecent(recentRow.modelData.path)
                        Keys.onSpacePressed: Scripts.chooseRecent(recentRow.modelData.path)
                        FocusRing {
                            targetRadius: recentRow.radius
                            visible: recentRow.activeFocus
                        }
                        ColumnLayout {
                            id: recentColumn
                            anchors.fill: parent
                            anchors.margins: Theme.space(1.5)
                            spacing: Theme.space(0.5)
                            Text {
                                Layout.fillWidth: true
                                text: recentRow.modelData.name
                                elide: Text.ElideRight
                                color: Theme.textPrimary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                                font.weight: Font.DemiBold
                            }
                            Text {
                                Layout.fillWidth: true
                                text: recentRow.modelData.folder
                                elide: Text.ElideMiddle
                                color: Theme.textTertiary
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.textXs
                            }
                        }
                        MouseArea {
                            id: recentArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: Scripts.chooseRecent(recentRow.modelData.path)
                        }
                    }
                }
            }
        }
    }

    // ---- 2. Review --------------------------------------------------------------------------------------------------
    ColumnLayout {
        objectName: "scriptsReview"
        Layout.fillWidth: true
        visible: Scripts.state === "ready"
        spacing: Theme.space(3)

        Text {
            objectName: "scriptsReviewLead"
            Layout.fillWidth: true
            textFormat: Text.StyledText
            wrapMode: Text.Wrap
            text: qsTr("Allow <b>%1</b> to:").arg(dialog.escaped(Scripts.scriptName))
            color: Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            Accessible.name: qsTr("Allow %1 to:").arg(Scripts.scriptName)
        }
        Text {
            Layout.fillWidth: true
            visible: Scripts.scriptDescription !== ""
            text: Scripts.scriptDescription
            textFormat: Text.PlainText // the script's own words: markup in its header must not look like Wabcut's review text
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Text {
            Layout.fillWidth: true
            text: Scripts.scriptPath
            elide: Text.ElideMiddle
            color: Theme.textTertiary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
        }

        ScrollView {
            id: permissionScroll
            objectName: "scriptsPermissionList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(permissionList.implicitHeight, dialog.listLimit)
            contentWidth: availableWidth
            clip: true
            ScrollBar.vertical: ThinScrollBar {}
            ColumnLayout {
                id: permissionList
                width: permissionScroll.availableWidth
                spacing: Theme.space(2)
                Repeater {
                    model: Scripts.permissions
                    delegate: Rectangle {
                        id: permissionRow
                        required property var modelData
                        required property int index
                        objectName: "scriptPermission_" + modelData.key
                        Layout.fillWidth: true
                        visible: modelData.requested === true
                        implicitHeight: permissionColumn.implicitHeight + Theme.space(3)
                        radius: Theme.radiusField
                        color: Theme.sunken
                        border.width: Theme.hairline
                        border.color: permissionSwitch.checked ? Theme.accentStroke : Theme.border
                        opacity: modelData.available === true ? 1 : 0.6
                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: Theme.space(1.5)
                            spacing: Theme.space(3)
                            ColumnLayout {
                                id: permissionColumn
                                Layout.fillWidth: true
                                spacing: Theme.space(0.5)
                                Text {
                                    Layout.fillWidth: true
                                    text: permissionRow.modelData.title
                                    wrapMode: Text.Wrap
                                    color: Theme.textPrimary
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textMd
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: permissionRow.modelData.explanation
                                    wrapMode: Text.Wrap
                                    color: Theme.textSecondary
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textSm
                                }
                                Text {
                                    Layout.fillWidth: true
                                    visible: permissionRow.modelData.note !== ""
                                    text: permissionRow.modelData.note
                                    wrapMode: Text.Wrap
                                    color: Theme.textTertiary
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textXs
                                }
                            }
                            Toggle {
                                id: permissionSwitch
                                objectName: "scriptPermissionSwitch_" + permissionRow.modelData.key
                                Layout.alignment: Qt.AlignVCenter
                                checked: permissionRow.modelData.chosen === true
                                enabled: permissionRow.modelData.fixed !== true && permissionRow.modelData.available === true
                                Accessible.name: permissionRow.modelData.title
                                Accessible.description: permissionRow.modelData.explanation
                                onToggled: Scripts.setChosen(permissionRow.modelData.key, checked)
                            }
                        }
                    }
                }
            }
        }
        Text {
            Layout.fillWidth: true
            visible: Scripts.unknownPermissions.length > 0
            text: qsTr("Ignored: %1 is not a permission Wabcut gives to scripts.").arg(Scripts.unknownPermissions.join(", "))
            wrapMode: Text.Wrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("A script has no network access through Wabcut. The Python it runs on is yours: Wabcut cannot stop a script from using your files or the internet by itself, so run only scripts you trust.")
            wrapMode: Text.Wrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(3)
            Text {
                Layout.fillWidth: true
                text: qsTr("Remember this choice for this version of the script")
                wrapMode: Text.Wrap
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Toggle {
                objectName: "scriptsRemember"
                checked: Scripts.remember
                Accessible.name: qsTr("Remember this choice for this version of the script")
                onToggled: Scripts.remember = checked
            }
        }

        // Python
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Icon {
                Layout.alignment: Qt.AlignTop
                name: Scripts.pythonOk ? "check" : "triangle-alert"
                size: Theme.iconSm
                color: Scripts.pythonOk ? Theme.success : (Scripts.pythonChecked ? Theme.warning : Theme.textTertiary)
            }
            Text {
                objectName: "scriptsPython"
                Layout.fillWidth: true
                text: Scripts.pythonText
                wrapMode: Text.Wrap
                color: Scripts.pythonOk ? Theme.textSecondary : Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            PillButton {
                objectName: "scriptsChoosePython"
                visible: Scripts.pythonChecked && !Scripts.pythonOk
                compact: true
                variant: "ghost"
                text: qsTr("Choose Python…")
                onClicked: pythonDialog.open()
            }
            PillButton {
                objectName: "scriptsFindPython"
                visible: Scripts.pythonChecked && !Scripts.pythonOk
                compact: true
                variant: "ghost"
                iconName: "rotate-cw"
                text: qsTr("Look again")
                onClicked: Scripts.findPython()
            }
        }
    }

    // ---- 3. Run -----------------------------------------------------------------------------------------------------
    ColumnLayout {
        objectName: "scriptsRun"
        Layout.fillWidth: true
        visible: Scripts.state === "running" || Scripts.state === "finished"
        spacing: Theme.space(2)

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Icon {
                visible: Scripts.state === "finished"
                name: Scripts.succeeded ? "check" : "triangle-alert"
                size: Theme.iconSm
                color: Scripts.succeeded ? Theme.success : Theme.danger
            }
            Text {
                objectName: "scriptsStatus"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Scripts.state === "finished" && !Scripts.succeeded ? Theme.danger : Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Font.DemiBold
                text: Scripts.state === "running"
                      ? qsTr("%1 is running… %n command(s) so far", "", Scripts.commandCount).arg(Scripts.scriptName)
                      : Scripts.summary
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.space(70)
            radius: Theme.radiusField
            color: Theme.sunken
            border.width: Theme.hairline
            border.color: Theme.border
            clip: true

            ListView {
                id: log
                objectName: "scriptsOutput"
                anchors.fill: parent
                anchors.margins: Theme.space(1.5)
                model: Scripts.output
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ThinScrollBar {}
                // Follow the output while it grows, unless the user scrolled up to read.
                property bool follow: true
                onMovementEnded: follow = atYEnd
                onCountChanged: if (follow) Qt.callLater(positionViewAtEnd)
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Script output")
                delegate: Text {
                    required property string kind
                    required property string line
                    width: log.width - Theme.space(2)
                    wrapMode: Text.WrapAnywhere
                    text: line
                    color: kind === "err" ? Theme.danger : kind === "info" ? Theme.textTertiary : Theme.textPrimary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textSm
                }
            }
            Text {
                anchors.centerIn: parent
                visible: log.count === 0
                text: qsTr("No output yet")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }
    }

    /// Escapes text for a StyledText label (a script's name is not markup).
    function escaped(s) {
        return String(s ?? "").replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }

    FileDialog {
        id: scriptDialog
        title: qsTr("Choose a Python script")
        nameFilters: [qsTr("Python scripts (*.py *.pyw)"), qsTr("All files (*)")]
        onAccepted: dialog.pick(selectedFile)
    }
    FileDialog {
        id: pythonDialog
        title: qsTr("Choose the Python to run scripts with")
        onAccepted: dialog.pickPython(selectedFile)
    }

    footerData: [
        PillButton {
            objectName: "scriptsCancel"
            visible: Scripts.state === "idle" || Scripts.state === "ready"
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "scriptsChooseAnother"
            visible: Scripts.state === "ready"
            variant: "ghost"
            text: qsTr("Choose another")
            onClicked: Scripts.reset()
        },
        PillButton {
            objectName: "scriptsRunButton"
            visible: Scripts.state === "ready"
            variant: "primary"
            iconName: "play"
            enabled: Scripts.canRun
            text: qsTr("Run")
            onClicked: Scripts.run()
        },
        PillButton {
            objectName: "scriptsStop"
            visible: Scripts.state === "running"
            variant: "danger"
            iconName: "square"
            text: qsTr("Stop")
            tooltip: qsTr("End the script and discard what it did")
            onClicked: Scripts.cancel()
        },
        PillButton {
            objectName: "scriptsCopy"
            visible: Scripts.state === "finished"
            variant: "ghost"
            iconName: "copy"
            text: qsTr("Copy output")
            onClicked: Scripts.copyOutput()
        },
        PillButton {
            objectName: "scriptsRunAgain"
            visible: Scripts.state === "finished"
            variant: "ghost"
            text: qsTr("Run again")
            onClicked: Scripts.chooseRecent(Scripts.scriptPath)
        },
        PillButton {
            objectName: "scriptsClose"
            visible: Scripts.state === "finished"
            variant: "primary"
            text: qsTr("Close")
            onClicked: dialog.close()
        }
    ]
}
