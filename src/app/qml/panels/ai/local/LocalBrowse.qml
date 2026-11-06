pragma ComponentBehavior: Bound
import QtQuick

// Settings > AI > Local models > Browse: GGUF models on Hugging Face that llama.cpp runs. A search field, the sort order
// (Recommended = fits this computer, then benchmark score; Benchmark score; Most downloaded; Most liked; Trending;
// Newest; Recently updated; Smallest) and task chips are always there; "More filters" opens size, parameters,
// quantization, licence, language and publisher choices and three switches (fits this computer, permissive licences
// only, downloaded only). Results are cards; Details opens a model's card with its quantizations and benchmarks. Any
// repository can be opened by its id (owner/name). Nothing is asked of Hugging Face until the user searches.
Column {
    id: root
    objectName: "localBrowse"

    property bool moreFilters: false
    signal openDetails()

    spacing: Theme.space(3)

    function runSearch() {
        LocalModels.query = searchField.text
        LocalModels.search()
    }

    Text {
        width: parent.width
        wrapMode: Text.WordWrap
        text: qsTr("Search the GGUF models on Hugging Face. Scores are the makers' published results; models that are not a direct quantization of a known model show none.")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    Flow {
        width: parent.width
        spacing: Theme.space(2)

        FieldInput {
            id: searchField
            objectName: "localSearchField"
            width: Math.min(root.width, Math.max(Theme.space(60), root.width - searchButton.width - Theme.space(2)))
            iconName: "search"
            clearOnEscape: true
            placeholderText: qsTr("Search models, e.g. qwen, gemma, coder")
            text: LocalModels.query
            enabled: !LocalModels.offline
            Accessible.name: qsTr("Search Hugging Face models")
            onAccepted: root.runSearch()
        }
        PillButton {
            id: searchButton
            objectName: "localSearchButton"
            variant: "primary"
            iconName: "search"
            text: LocalModels.searched || searchField.text !== "" ? qsTr("Search") : qsTr("Show popular models")
            enabled: !LocalModels.offline && !LocalModels.searching
            disabledReason: LocalModels.offline ? qsTr("Offline mode is on") : ""
            onClicked: root.runSearch()
        }
    }

    LocalChips {
        width: parent.width
        namePrefix: "localTask"
        accessibleName: qsTr("Good for")
        model: LocalModels.taskOptions
        current: LocalModels.filters.task
        onActivated: id => LocalModels.setFilter("task", id)
    }

    Flow {
        width: parent.width
        spacing: Theme.space(3)

        LocalOptionPicker {
            label: qsTr("Sort")
            inline: true
            buttonName: "localSortPicker"
            options: LocalModels.sortOptions
            current: LocalModels.sortKey
            onChosen: id => LocalModels.sortKey = id
        }
        PillButton {
            objectName: "localMoreFilters"
            compact: true
            variant: "ghost"
            iconName: root.moreFilters ? "chevron-up" : "funnel"
            text: root.moreFilters ? qsTr("Fewer filters") : qsTr("More filters")
            onClicked: root.moreFilters = !root.moreFilters
        }
        Row {
            spacing: Theme.space(2)
            Toggle {
                id: fitsToggle
                objectName: "localFitsOnly"
                checked: LocalModels.filters.fitsOnly
                Accessible.name: qsTr("Fits this computer")
                onToggled: LocalModels.setFilter("fitsOnly", checked)
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Fits this computer")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }
    }

    Flow {
        objectName: "localFilters"
        visible: root.moreFilters
        width: parent.width
        spacing: Theme.space(3)

        LocalOptionPicker {
            label: qsTr("Download size")
            buttonName: "localSizePicker"
            options: LocalModels.sizeOptions
            current: LocalModels.filters.size
            onChosen: id => LocalModels.setFilter("size", id)
        }
        LocalOptionPicker {
            label: qsTr("Parameters")
            buttonName: "localParamsPicker"
            options: LocalModels.paramOptions
            current: LocalModels.filters.params
            onChosen: id => LocalModels.setFilter("params", id)
        }
        LocalOptionPicker {
            label: qsTr("Quantization")
            buttonName: "localQuantPicker"
            options: LocalModels.quantOptions
            current: LocalModels.filters.quant
            onChosen: id => LocalModels.setFilter("quant", id)
        }
        LocalOptionPicker {
            label: qsTr("Language")
            buttonName: "localLanguagePicker"
            options: LocalModels.languageOptions
            current: LocalModels.filters.language
            onChosen: id => LocalModels.setFilter("language", id)
        }
        LocalOptionPicker {
            label: qsTr("Publisher")
            buttonName: "localPublisherPicker"
            options: LocalModels.publisherOptions
            current: LocalModels.filters.publisher
            onChosen: id => LocalModels.setFilter("publisher", id)
        }
        Row {
            spacing: Theme.space(2)
            Toggle {
                objectName: "localPermissiveOnly"
                checked: LocalModels.filters.permissiveOnly
                Accessible.name: qsTr("Open licences only")
                tooltip: qsTr("MIT, Apache 2.0, BSD and similar. Other licences are shown with their terms and need your acceptance before a download.")
                onToggled: LocalModels.setFilter("permissiveOnly", checked)
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Open licences only")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }
        Row {
            spacing: Theme.space(2)
            Toggle {
                objectName: "localInstalledOnly"
                checked: LocalModels.filters.installedOnly
                Accessible.name: qsTr("Downloaded only")
                onToggled: LocalModels.setFilter("installedOnly", checked)
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Downloaded only")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }
        PillButton {
            objectName: "localResetFilters"
            compact: true
            variant: "ghost"
            text: qsTr("Reset filters")
            onClicked: LocalModels.resetFilters()
        }
    }

    // Any repository by its id, for people who know what they want.
    Flow {
        visible: root.moreFilters
        width: parent.width
        spacing: Theme.space(2)

        FieldInput {
            id: repoField
            objectName: "localRepoField"
            width: Math.min(root.width, Theme.space(90))
            placeholderText: qsTr("Open a repository: owner/name")
            enabled: !LocalModels.offline
            Accessible.name: qsTr("Hugging Face repository id")
            onAccepted: openRepo.clicked()
        }
        PillButton {
            id: openRepo
            objectName: "localOpenRepo"
            text: qsTr("Open")
            enabled: repoField.text.trim() !== "" && !LocalModels.offline
            onClicked: {
                LocalModels.openModel(repoField.text.trim())
                root.openDetails()
            }
        }
    }

    LoaderPill {
        visible: LocalModels.searching
        text: qsTr("Searching Hugging Face")
    }

    Text {
        objectName: "localSearchMessage"
        visible: text !== ""
        width: parent.width
        wrapMode: Text.WordWrap
        text: LocalModels.searchMessage !== "" ? LocalModels.searchMessage
              : LocalModels.searched && !LocalModels.searching && LocalModels.results.length === 0
                ? (LocalModels.hiddenCount > 0 ? qsTr("The filters hide all %n model(s) found.", "", LocalModels.hiddenCount)
                                               : qsTr("No GGUF chat model matches."))
              : LocalModels.hiddenCount > 0 ? qsTr("%n more hidden by the filters.", "", LocalModels.hiddenCount)
              : ""
        color: LocalModels.searchMessage !== "" ? Theme.warningText : Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    Repeater {
        model: LocalModels.results

        delegate: LocalModelCard {
            id: result
            required property var modelData
            row: modelData
            width: root.width
            primaryText: ""
            secondaryText: qsTr("Details and download")
            onSecondary: {
                LocalModels.openModel(result.modelData.repo)
                root.openDetails()
            }
        }
    }

    PillButton {
        objectName: "localLoadMore"
        visible: LocalModels.hasMore && !LocalModels.searching
        text: qsTr("Load more")
        onClicked: LocalModels.loadMore()
    }
}
