pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts

// Settings > Content Credentials (0.2; Premiere's Content Credentials preferences): who signs the C2PA manifests of
// exports - the per-user local identity Wabcut makes on the first signed export (a local root and an ECDSA P-256
// signing certificate; the key in Windows Credential Manager), or the user's own certificate from a CA on the C2PA
// trust list (a PEM chain, signing certificate first, and its PEM private key) - and whether to ask generative AI not
// to train on the exports. Opened from the Export drawer's Content Credentials row. Every change is saved at once
// (ExportController.credentials*; docs/export.md "Content Credentials").
WabcutDialog {
    id: dialog
    objectName: "contentCredentialsSettings"

    property var identity: ({})
    property string choosing: "" // "certificate" or "key" while the file dialog is open

    function refresh() {
        identity = ExportController.signingIdentity()
    }

    title: qsTr("Content Credentials")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(170), Theme.space(170))
    onOpened: refresh()

    component Label: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }
    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    SectionHeader {
        Layout.fillWidth: true
        text: qsTr("Signing certificate")
    }
    Note {
        objectName: "credentialsSigner"
        text: dialog.identity.error
              ? qsTr("Cannot sign with it: %1").arg(dialog.identity.error)
              : dialog.identity.local
                ? (dialog.identity.ready
                   ? qsTr("Wabcut's local identity on this computer (%1, valid until %2). Viewers can check that an export is unchanged, but show the signer as not trusted.")
                         .arg(dialog.identity.subject).arg(dialog.identity.notAfter)
                   : qsTr("Wabcut's local identity: made on this computer with the first signed export. Viewers can check that an export is unchanged, but show the signer as not trusted."))
                : qsTr("Your certificate: %1, valid until %2.").arg(dialog.identity.subject ?? "").arg(dialog.identity.notAfter ?? "")
    }
    GridLayout {
        Layout.fillWidth: true
        columns: 3
        columnSpacing: Theme.space(3)
        rowSpacing: Theme.space(2)

        Label { text: qsTr("Certificate (PEM)") }
        FieldInput {
            objectName: "credentialsCertificatePath"
            Layout.fillWidth: true
            readOnly: true
            text: ExportController.credentialsCertificate
            placeholderText: qsTr("Local identity")
        }
        PillButton {
            text: qsTr("Choose…")
            compact: true
            onClicked: {
                dialog.choosing = "certificate"
                pemDialog.open()
            }
        }
        Label { text: qsTr("Private key (PEM)") }
        FieldInput {
            objectName: "credentialsKeyPath"
            Layout.fillWidth: true
            readOnly: true
            text: ExportController.credentialsKey
            placeholderText: qsTr("Local identity")
        }
        PillButton {
            text: qsTr("Choose…")
            compact: true
            onClicked: {
                dialog.choosing = "key"
                pemDialog.open()
            }
        }
    }
    PillButton {
        objectName: "credentialsUseLocal"
        visible: ExportController.credentialsCertificate !== "" || ExportController.credentialsKey !== ""
        text: qsTr("Use the local identity")
        compact: true
        onClicked: {
            ExportController.credentialsCertificate = ""
            ExportController.credentialsKey = ""
            dialog.refresh()
        }
    }

    SectionHeader {
        Layout.fillWidth: true
        text: qsTr("Generative AI")
    }
    Toggle {
        objectName: "credentialsDoNotTrain"
        Layout.fillWidth: true
        text: qsTr("Ask generative AI models not to train on or use my exports")
        checked: ExportController.credentialsDoNotTrain
        onToggled: ExportController.credentialsDoNotTrain = checked
    }
    Note {
        text: qsTr("AI-generated media in an export is always declared in its Content Credentials: what was generated, by which provider and model, and where it plays.")
    }

    FileDialog {
        id: pemDialog
        title: dialog.choosing === "key" ? qsTr("The certificate's private key") : qsTr("The signing certificate chain")
        nameFilters: [qsTr("PEM files (*.pem *.crt *.cer *.key)"), qsTr("All files (*)")]
        onAccepted: {
            if (dialog.choosing === "key")
                ExportController.credentialsKey = selectedFile.toString()
            else
                ExportController.credentialsCertificate = selectedFile.toString()
            dialog.refresh()
        }
    }

    footerData: [
        PillButton {
            objectName: "contentCredentialsDone"
            variant: "primary"
            text: qsTr("Done")
            onClicked: dialog.close()
        }
    ]
}
