pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs
import QtCore

// Settings > Appearance (0.1.3; it replaces the title bar's Appearance popover): themes (built-in presets, the user's
// saved ones, import/export as .wabcut-theme.json files through ThemeFiles), light or dark, the accent ("Light
// sources"), high contrast, reduce motion, the interface size, and every colour (ThemeColorEditor). Everything applies
// at once and is kept by Preferences; a theme is {name, dark, accent, colors, clipColors} (ThemeFiles.h).
SettingsPage {
    id: page
    objectName: "appearanceSettingsPage"

    title: qsTr("Appearance")
    description: qsTr("Themes, colours and the size of the interface.")

    // The accents' names (Violet, Pink, Lime, Orange, Blue: design language v3; the keys are v2's).
    readonly property var accentLabels: Theme.accentLabels
    // Built-in themes. The first two are the design language's own dark and light looks (no custom colours; "Echo" is
    // stored under its v2 name, Ultraviolet); the others only re-tint surfaces and text, keeping meta text at 4.5:1 on
    // panels and cards.
    readonly property var presets: [
        { name: "Ultraviolet", label: qsTr("Echo"), dark: true, accent: "ultraviolet", colors: {} },
        { name: "Daylight", label: qsTr("Daylight"), dark: false, accent: "ultraviolet", colors: {} },
        { name: "Graphite", label: qsTr("Graphite"), dark: true, accent: "ultraviolet", colors: Theme.themePresetColors["Graphite"] },
        { name: "Midnight", label: qsTr("Midnight"), dark: true, accent: "ice", colors: Theme.themePresetColors["Midnight"] },
        { name: "Paper", label: qsTr("Paper"), dark: false, accent: "sodium", colors: Theme.themePresetColors["Paper"] }
    ]
    readonly property var savedThemes: {
        const list = Preferences.parsedJson(Preferences.savedThemes, [])
        return Array.isArray(list) ? list.filter(t => t && typeof t.name === "string" && t.name !== "") : []
    }
    /// What is on screen now, as a theme.
    readonly property var currentTheme: ({
        format: "wabcut-theme", version: 1, name: Preferences.themeName, dark: Theme.dark,
        accent: Theme.canonicalAccent(Theme.accentName), colors: Theme.customColors ?? {},
        clipColors: Theme.customClipColors ?? []
    })
    /// The theme the screen matches (the one last chosen first, then any preset or saved one), or "" when the colours
    /// were edited since.
    readonly property string matchingTheme: {
        const all = page.presets.concat(page.savedThemes)
        const named = all.find(t => t.name === Preferences.themeName)
        if (named && page.sameTheme(named, page.currentTheme))
            return named.name
        const any = all.find(t => page.sameTheme(t, page.currentTheme))
        return any ? any.name : ""
    }
    property string message: ""
    property bool messageOk: true

    function sameTheme(a: var, b: var): bool {
        return a.dark === b.dark && Theme.canonicalAccent(a.accent ?? Theme.accentName) === b.accent
               && JSON.stringify(Theme.sanitizedColors(a.colors)) === JSON.stringify(Theme.sanitizedColors(b.colors))
               && JSON.stringify(Theme.sanitizedClipColors(a.clipColors ?? [])) === JSON.stringify(b.clipColors ?? [])
    }
    /// Puts `theme` on screen (a preset, a saved or an imported theme).
    function applyTheme(theme: var) {
        Theme.dark = theme.dark !== false
        if (theme.accent)
            Theme.accentName = Theme.canonicalAccent(theme.accent)
        Theme.customColors = Theme.sanitizedColors(theme.colors ?? {})
        Theme.customClipColors = Theme.sanitizedClipColors(theme.clipColors ?? [])
        Preferences.themeName = theme.name ?? ""
    }
    /// Keeps `theme` among the saved themes (replacing one of the same name).
    function keepTheme(theme: var) {
        const list = page.savedThemes.filter(t => t.name !== theme.name)
        list.push(theme)
        Preferences.savedThemes = JSON.stringify(list)
    }
    function deleteTheme(name: string) {
        Preferences.savedThemes = JSON.stringify(page.savedThemes.filter(t => t.name !== name))
        if (Preferences.themeName === name)
            Preferences.themeName = ""
    }
    /// Saves what is on screen as a named theme; false (with a message) when the name is empty or a preset's.
    function saveCurrentAs(name: string): bool {
        const trimmed = name.trim()
        if (trimmed === "" || page.presets.some(p => p.name.toLowerCase() === trimmed.toLowerCase())) {
            page.setMessage(false, trimmed === "" ? qsTr("Give the theme a name.") : qsTr("That name belongs to a built-in theme."))
            return false
        }
        const theme = Object.assign({}, page.currentTheme, { name: trimmed })
        const checked = ThemeFiles.normalize(theme)
        if (!checked.ok) {
            page.setMessage(false, checked.error)
            return false
        }
        page.keepTheme(checked.theme)
        Preferences.themeName = trimmed
        page.setMessage(true, qsTr("Saved \"%1\".").arg(trimmed))
        return true
    }
    /// Reads a theme file, keeps it among the saved themes and applies it. Returns "" or the reason it failed.
    function importFrom(url: url): string {
        const result = ThemeFiles.importTheme(url)
        if (!result.ok) {
            page.setMessage(false, result.error)
            return result.error
        }
        const theme = result.theme
        if (theme.name === "") {
            const file = String(url).split("/").pop()
            theme.name = decodeURIComponent(file).replace(/\.wabcut-theme\.json$|\.json$/i, "") || qsTr("Imported theme")
        }
        if (page.presets.some(p => p.name === theme.name))
            theme.name = qsTr("%1 (imported)").arg(theme.name)
        page.keepTheme(theme)
        page.applyTheme(theme)
        page.setMessage(true, qsTr("Imported \"%1\".").arg(theme.name))
        return ""
    }
    /// Writes what is on screen to a theme file. Returns "" or the reason it failed.
    function exportTo(url: url): string {
        const name = Preferences.themeName !== "" ? Preferences.themeName : qsTr("My theme")
        const error = ThemeFiles.exportTheme(url, Object.assign({}, page.currentTheme, { name: name }))
        page.setMessage(error === "", error === "" ? qsTr("Exported to %1.").arg(decodeURIComponent(String(url).split("/").pop())) : error)
        return error
    }
    function setMessage(ok: bool, text: string) {
        page.messageOk = ok
        page.message = text
    }

    FileDialog {
        id: importDialog
        title: qsTr("Import theme")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Wabcut themes (*.json)"), qsTr("All files (*)")]
        onAccepted: page.importFrom(selectedFile)
    }
    FileDialog {
        id: exportDialog
        title: qsTr("Export theme")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: [qsTr("Wabcut themes (*.json)")]
        onAccepted: page.exportTo(selectedFile)
    }

    // ---- Themes ----
    SettingsSection {
        title: qsTr("Theme")
        description: page.matchingTheme !== "" ? qsTr("Using %1.").arg(page.matchingTheme)
                   : Preferences.themeName !== "" ? qsTr("Edited from %1: save it to keep it as a theme.").arg(Preferences.themeName)
                   : qsTr("Custom colours: save them to keep them as a theme.")
        flat: true

        Flow {
            width: parent.width
            spacing: Theme.space(3)

            Repeater {
                model: page.presets.concat(page.savedThemes)

                delegate: ThemeCard {
                    required property var modelData
                    required property int index
                    objectName: "themeCard_" + modelData.name
                    theme: modelData
                    label: modelData.label ?? modelData.name
                    builtIn: index < page.presets.length
                    current: page.matchingTheme === modelData.name
                    onChosen: page.applyTheme(modelData)
                    onDeleteRequested: page.deleteTheme(modelData.name)
                }
            }
        }

        Item {
            width: parent.width
            height: Theme.space(3)
        }

        Row {
            spacing: Theme.space(2)

            FieldInput {
                id: themeNameField
                objectName: "themeNameField"
                width: Theme.space(48)
                placeholderText: qsTr("Theme name")
                Accessible.name: qsTr("Name for the saved theme")
                onAccepted: saveButton.clicked()
            }
            PillButton {
                id: saveButton
                objectName: "saveThemeButton"
                iconName: "save"
                text: qsTr("Save theme")
                enabled: themeNameField.text.trim() !== ""
                onClicked: {
                    if (page.saveCurrentAs(themeNameField.text))
                        themeNameField.clear()
                }
            }
            PillButton {
                objectName: "importThemeButton"
                variant: "ghost"
                iconName: "download"
                text: qsTr("Import…")
                onClicked: importDialog.open()
            }
            PillButton {
                objectName: "exportThemeButton"
                variant: "ghost"
                iconName: "upload"
                text: qsTr("Export…")
                onClicked: {
                    exportDialog.selectedFile = StandardPaths.writableLocation(StandardPaths.DocumentsLocation) + "/"
                                                + ThemeFiles.suggestedFileName(Preferences.themeName)
                    exportDialog.open()
                }
            }
        }
        Text {
            objectName: "themeMessage"
            width: parent.width
            visible: page.message !== ""
            topPadding: Theme.space(2)
            text: page.message
            wrapMode: Text.WordWrap
            color: page.messageOk ? Theme.textSecondary : Theme.danger
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    // ---- Light, accent, accessibility ----
    SettingsSection {
        title: qsTr("Mode and accent")

        SettingsRow {
            label: qsTr("Mode")
            PillTabs {
                objectName: "themeTabs"
                width: Theme.space(56)
                compact: true
                model: [{ label: qsTr("Dark"), icon: "moon" }, { label: qsTr("Light"), icon: "sun" }]
                currentIndex: Theme.dark ? 0 : 1
                onActivated: index => Theme.dark = index === 0
                Accessible.name: qsTr("Theme")
            }
        }
        SettingsRow {
            label: qsTr("Accent: %1").arg(page.accentLabels[Theme.canonicalAccent(Theme.accentName)])
            description: qsTr("Primary buttons, the active workspace and segment, selections and ranges take it.")
            Repeater {
                model: Theme.accentOrder

                delegate: Swatch {
                    required property string modelData
                    objectName: "accentSwatch_" + modelData
                    swatchColor: Theme.accents[modelData].fill
                    tooltip: qsTr("Accent: %1").arg(page.accentLabels[modelData])
                    checked: Theme.canonicalAccent(Theme.accentName) === modelData
                    onClicked: {
                        checked = Qt.binding(() => Theme.canonicalAccent(Theme.accentName) === modelData)
                        Theme.accentName = modelData
                    }
                }
            }
        }
        SettingsRow {
            label: qsTr("High contrast")
            description: qsTr("Black or white surfaces, brighter text and strong borders. Custom colours pause while it is on.")
            Toggle {
                objectName: "highContrastToggle"
                tooltip: qsTr("High contrast")
                checked: Theme.highContrast
                onToggled: Theme.highContrast = checked
            }
        }
        SettingsRow {
            label: qsTr("Reduce motion")
            description: qsTr("No slides, glows, sparks or loops: loaders show the still mark; changes cross-fade quickly.")
            Toggle {
                objectName: "reduceMotionToggle"
                tooltip: qsTr("Reduce motion")
                checked: Theme.reduceMotion
                onToggled: Theme.reduceMotion = checked
            }
        }
        SettingsRow {
            objectName: "launchIntroRow"
            label: qsTr("Play the intro when Wabcut starts")
            description: Theme.reduceMotion ? qsTr("Reduce motion skips it.")
                                            : qsTr("The mark assembles for two seconds; a click or Esc skips it.")
            Toggle {
                objectName: "launchIntroToggle"
                tooltip: qsTr("Play the intro when Wabcut starts")
                checked: Preferences.launchIntro
                onToggled: Preferences.launchIntro = checked
            }
        }

        SettingsRow {
            objectName: "uiScaleRow"
            label: qsTr("Interface size")
            description: Preferences.effectiveUiScale !== Theme.uiScale ? qsTr("The new size applies when Wabcut restarts.")
                                                                        : qsTr("Applies when Wabcut starts.")
            PillTabs {
                objectName: "uiScaleTabs"
                width: Theme.space(56)
                compact: true
                model: [{ label: qsTr("100%") }, { label: qsTr("125%") }, { label: qsTr("150%") }]
                currentIndex: [100, 125, 150].indexOf(Preferences.effectiveUiScale)
                onActivated: index => Preferences.uiScale = [100, 125, 150][index]
                Accessible.name: qsTr("Interface size")
            }
        }
    }

    // ---- Every colour ----
    SettingsSection {
        title: qsTr("Custom colours")
        description: Theme.highContrast ? qsTr("Custom colours are paused while high contrast is on.")
                                        : qsTr("Pick any colour; the interface follows as you drag. Reset brings back the theme's.")

        Item {
            width: parent.width
            height: colorEditor.implicitHeight + Theme.space(4)

            ThemeColorEditor {
                id: colorEditor
                y: Theme.space(2)
                width: parent.width
            }
        }
        Item {
            width: parent.width
            height: Theme.controlHeight + Theme.space(3)

            PillButton {
                objectName: "resetAllColors"
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                compact: true
                variant: "ghost"
                iconName: "rotate-ccw"
                text: qsTr("Reset every colour")
                enabled: Object.keys(Theme.customColors ?? {}).length > 0 || Theme.customClipColors.length > 0
                onClicked: {
                    Theme.customColors = ({})
                    Theme.customClipColors = []
                }
            }
        }
    }
}
