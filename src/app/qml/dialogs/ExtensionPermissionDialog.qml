pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// The install / enable permission dialog of an extension (C.12, M5n; docs/internal/c12-extensions-handoff.md): "Allow <name>
// to:", one row per capability it asks for (what it allows in bold, one sentence under it, a switch, on by default or
// when it was granted already: a capability left off is simply not granted), the notes (comes with the project, an
// update, unknown manifest keys), and the author, licence and folder. Network never appears: such a manifest does not
// load. For a per-project extension the user has not trusted (for this content) it also says where the code came from,
// and Allow becomes "Trust and enable". openFor(id) reviews an extension; openForFolder(url) one to install.
WabcutDialog {
    id: dialog
    objectName: "extensionPermissions"

    /// Extensions.review(id) or Extensions.reviewFolder(url).
    property var review: ({})
    /// The folder being installed (openForFolder), else empty.
    property url folder
    readonly property bool installing: folder.toString() !== ""
    readonly property bool trusting: review.needsTrust === true
    /// Capability name -> switched on.
    property var chosen: ({})
    property string error: ""

    function openFor(id) {
        folder = ""
        load(Extensions.review(id))
    }
    function openForFolder(url) {
        folder = url
        load(Extensions.reviewFolder(url))
    }
    function load(r) {
        review = r
        error = r.ok ? "" : r.error
        const c = {}
        for (const request of (r.requests ?? []))
            c[request.capability] = true
        chosen = c
        open()
    }
    function chosenList() {
        return Object.keys(dialog.chosen).filter(k => dialog.chosen[k])
    }
    /// Allow / Trust and enable / Install.
    function accept() {
        const caps = chosenList()
        const e = dialog.installing ? Extensions.install(dialog.folder, caps)
                : dialog.trusting ? Extensions.trustAndEnable(dialog.review.id, caps)
                : Extensions.enable(dialog.review.id, caps)
        if (e !== "") {
            dialog.error = e
            return
        }
        dialog.close()
    }
    /// Escapes text for a StyledText label (an extension's name is not markup).
    function escaped(s) {
        return String(s ?? "").replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }

    title: dialog.installing ? qsTr("Install extension")
         : dialog.trusting ? qsTr("Project extension")
         : dialog.review.update === true ? qsTr("Review an update") : qsTr("Review permissions")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(140), Theme.space(140))

    // "Allow Beat Markers to:"
    Text {
        objectName: "extensionPermissionsLead"
        Layout.fillWidth: true
        visible: dialog.review.ok === true
        textFormat: Text.StyledText
        wrapMode: Text.Wrap
        text: qsTr("Allow <b>%1</b> to:").arg(dialog.escaped(dialog.review.name))
        color: Theme.textPrimary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        Accessible.name: qsTr("Allow %1 to:").arg(dialog.review.name ?? "")
    }

    // One row per capability asked for.
    Repeater {
        model: dialog.review.requests ?? []
        delegate: Rectangle {
            id: requestRow
            required property var modelData
            required property int index
            objectName: "extensionCapability_" + modelData.capability
            Layout.fillWidth: true
            implicitHeight: requestColumn.implicitHeight + Theme.space(3)
            radius: Theme.radiusField
            color: Theme.sunken
            border.width: Theme.hairline
            border.color: capabilitySwitch.checked ? Theme.accentStroke : Theme.border
            RowLayout {
                anchors.fill: parent
                anchors.margins: Theme.space(1.5)
                spacing: Theme.space(3)
                ColumnLayout {
                    id: requestColumn
                    Layout.fillWidth: true
                    spacing: Theme.space(0.5)
                    Text {
                        Layout.fillWidth: true
                        text: requestRow.modelData.title
                        wrapMode: Text.Wrap
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        text: requestRow.modelData.explanation
                        wrapMode: Text.Wrap
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    Text {
                        visible: requestRow.modelData.granted === true
                        text: qsTr("Allowed before")
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                    }
                }
                Toggle {
                    id: capabilitySwitch
                    objectName: "extensionCapabilitySwitch_" + requestRow.modelData.capability
                    Layout.alignment: Qt.AlignVCenter
                    checked: dialog.chosen[requestRow.modelData.capability] === true
                    Accessible.name: requestRow.modelData.title
                    Accessible.description: requestRow.modelData.explanation
                    onToggled: {
                        const c = Object.assign({}, dialog.chosen)
                        c[requestRow.modelData.capability] = checked
                        dialog.chosen = c
                    }
                }
            }
        }
    }
    Text {
        Layout.fillWidth: true
        visible: dialog.review.ok === true && (dialog.review.requests ?? []).length === 0
        text: qsTr("It asks for no permissions: it cannot read or change your project.")
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    // A per-project extension: it came with the project, not from the user.
    Rectangle {
        objectName: "extensionTrustWarning"
        Layout.fillWidth: true
        visible: dialog.trusting
        implicitHeight: trustText.implicitHeight + Theme.space(3)
        radius: Theme.radiusField
        color: "transparent"
        border.width: Theme.hairline
        border.color: Theme.warning
        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.space(1.5)
            spacing: Theme.space(2)
            Icon {
                Layout.alignment: Qt.AlignTop
                name: "triangle-alert"
                size: Theme.iconSm
                color: Theme.warning
            }
            Text {
                id: trustText
                Layout.fillWidth: true
                textFormat: Text.StyledText
                wrapMode: Text.Wrap
                text: qsTr("<b>Project extension</b>: <i>%1</i> came with this project, not from you. It will run code from %2 with the permissions you grant. Trust it for this project?")
                          .arg(dialog.escaped(dialog.review.name)).arg(dialog.escaped(dialog.review.folder))
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }
    }

    // Notes: comes with the project, an update, unknown manifest keys.
    Rectangle {
        objectName: "extensionNotes"
        Layout.fillWidth: true
        visible: (dialog.review.notes ?? []).length > 0
        implicitHeight: notesColumn.implicitHeight + Theme.space(3)
        radius: Theme.radiusField
        color: Theme.accentWash
        border.width: Theme.hairline
        border.color: Theme.accentStroke
        ColumnLayout {
            id: notesColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.space(1.5)
            spacing: Theme.space(0.5)
            Repeater {
                model: dialog.review.notes ?? []
                delegate: Text {
                    required property string modelData
                    Layout.fillWidth: true
                    text: modelData
                    wrapMode: Text.Wrap
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
            }
        }
    }

    // Who made it, under which licence, where it lives.
    ColumnLayout {
        Layout.fillWidth: true
        visible: dialog.review.ok === true
        spacing: Theme.space(0.5)
        Text {
            objectName: "extensionPermissionsAuthor"
            Layout.fillWidth: true
            text: [dialog.review.author ? qsTr("By %1").arg(dialog.review.author) : "",
                   dialog.review.licence ? qsTr("Licence: %1").arg(dialog.review.licence) : "",
                   dialog.review.version ? qsTr("Version %1").arg(dialog.review.version) : ""].filter(s => s !== "").join(" · ")
            elide: Text.ElideRight
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Text {
            Layout.fillWidth: true
            text: dialog.review.folder ?? ""
            elide: Text.ElideMiddle
            color: Theme.textTertiary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
        }
    }

    Text {
        objectName: "extensionPermissionsError"
        Layout.fillWidth: true
        visible: dialog.error !== ""
        text: dialog.error
        wrapMode: Text.Wrap
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    footerData: [
        PillButton {
            objectName: "extensionPermissionsCancel"
            variant: "ghost"
            text: dialog.trusting ? qsTr("Don't trust") : qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "extensionPermissionsAllow"
            variant: "primary"
            enabled: dialog.review.ok === true
            iconName: dialog.trusting ? "shield-check" : ""
            text: dialog.installing ? qsTr("Install and allow")
                : dialog.trusting ? qsTr("Trust and enable") : qsTr("Allow")
            onClicked: dialog.accept()
        }
    ]
}
