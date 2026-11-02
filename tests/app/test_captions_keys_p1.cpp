// 0.6 (P1 round, #35): Premiere's four caption keys. Add New Caption Track (Ctrl+Alt+A), Add Caption at Playhead
// (Ctrl+Alt+C) and Go to Next / Previous Caption Segment (Ctrl+Alt+Down / Up) on the shown caption track:
//   * the bridge says whether each one can run and why ("Open a sequence first", "Add a caption track first", "No caption
//     after / before the playhead"), which the palette, its rows and the Sequence > Captions menu show;
//   * each one is one undo step; a new track is "Captions", then "Captions 2", ..., and becomes the shown track;
//   * Ctrl+Alt+C adds the caption and the Captions tab shows its text selected, so typing replaces "New caption";
//   * Ctrl+Alt+Down / Up move the playhead onto the next / previous caption and select it (the lane outlines it), with
//     no edit in the history;
//   * the keys work in the shell, also while a caption's text field has the focus.
// Offscreen; no window is shown.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "CaptionsBridge.h"
#include "CommandPalette.h"
#include "ProjectController.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QKeyEvent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>

#include <memory>
#include <string>

#include <algorithm>
#include <cmath>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

const char* const kCaptionKeys[] = {"sequence.addCaptionTrack", "sequence.addCaptionAtPlayhead",
                                    "sequence.nextCaptionSegment", "sequence.previousCaptionSegment"};

QVariantMap stateOf(const CaptionsBridge& captions, const char* id) {
    return captions.commandAvailability(QString::fromLatin1(id));
}

QString reasonOf(const CaptionsBridge& captions, const char* id) {
    return stateOf(captions, id).value(QStringLiteral("reason")).toString();
}

/// 40 frames (25 fps) of linked A/V: a sequence to caption.
const char* const kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
)";

QQuickItem* findNamed(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findNamed(child, name))
            return found;
    }
    return nullptr;
}

void tap(QQuickWindow* window, int key, Qt::KeyboardModifiers mods) {
    QKeyEvent press(QEvent::KeyPress, key, mods);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods);
    QCoreApplication::sendEvent(window, &release);
}

/// A project with a sequence and the caption bridge on it, no window.
struct KeyRig {
    Timeline tl{kPicture};
    ProjectController controller;
    CaptionsBridge captions;

    KeyRig() {
        tl.linkMatching();
        captions.setController(&controller);
    }
};

} // namespace

TEST_CASE("Caption keys: each command says why it cannot run and runs as one undo step", "[app][captions][keys]") {
    KeyRig rig;
    rig.controller.resetProject(ProjectState{}, SequenceId{}); // a project with no sequence open
    REQUIRE(rig.controller.currentSequence() == nullptr);

    // No sequence: all four are known and refused for the same reason.
    for (const char* id : kCaptionKeys) {
        INFO(id);
        const QVariantMap state = stateOf(rig.captions, id);
        CHECK(state.value(QStringLiteral("handled")).toBool());
        CHECK_FALSE(state.value(QStringLiteral("enabled")).toBool());
        CHECK(state.value(QStringLiteral("reason")).toString() == QStringLiteral("Open a sequence first"));
        CHECK_FALSE(rig.captions.runCommand(QString::fromLatin1(id)));
    }
    CHECK_FALSE(rig.captions.handlesCommand(QStringLiteral("sequence.trimEdit")));
    CHECK_FALSE(rig.captions.runCommand(QStringLiteral("sequence.trimEdit")));
    CHECK(rig.captions.handlesCommand(QStringLiteral("sequence.addCaptionTrack")));

    rig.controller.resetProject(rig.tl.state, rig.tl.seq);
    // A new track can always be made; the others need a caption track to act on.
    CHECK(stateOf(rig.captions, "sequence.addCaptionTrack").value(QStringLiteral("enabled")).toBool());
    CHECK(reasonOf(rig.captions, "sequence.addCaptionAtPlayhead") == QStringLiteral("Add a caption track first"));
    CHECK(reasonOf(rig.captions, "sequence.nextCaptionSegment") == QStringLiteral("Add a caption track first"));
    CHECK(reasonOf(rig.captions, "sequence.previousCaptionSegment") == QStringLiteral("Add a caption track first"));
    CHECK_FALSE(rig.captions.runCommand(QStringLiteral("sequence.addCaptionAtPlayhead")));
    CHECK(rig.captions.message() == QStringLiteral("Add a caption track first"));

    // Ctrl+Alt+A: "Captions", which becomes the shown track, in one undo step.
    std::size_t undo = rig.controller.session().undoCount();
    REQUIRE(rig.captions.runCommand(QStringLiteral("sequence.addCaptionTrack")));
    CHECK(rig.controller.session().undoCount() == undo + 1);
    REQUIRE(rig.captions.tracks().size() == 1);
    CHECK(rig.captions.tracks().at(0).toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Captions"));
    CHECK(rig.captions.trackInfo().value(QStringLiteral("name")).toString() == QStringLiteral("Captions"));
    CHECK(rig.captions.cues().isEmpty());
    // The track has no caption yet: there is nothing to go to, and the rows say so.
    CHECK(reasonOf(rig.captions, "sequence.nextCaptionSegment") == QStringLiteral("No caption after the playhead"));
    CHECK(reasonOf(rig.captions, "sequence.previousCaptionSegment") == QStringLiteral("No caption before the playhead"));
    // A second one does not take the first one's name.
    REQUIRE(rig.captions.runCommand(QStringLiteral("sequence.addCaptionTrack")));
    REQUIRE(rig.captions.tracks().size() == 2);
    CHECK(rig.captions.tracks().at(1).toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Captions 2"));
    CHECK(rig.captions.trackInfo().value(QStringLiteral("name")).toString() == QStringLiteral("Captions 2"));
    REQUIRE(rig.controller.undo());
    REQUIRE(rig.captions.tracks().size() == 1);
    REQUIRE(rig.captions.trackInfo().value(QStringLiteral("name")).toString() == QStringLiteral("Captions"));

    // Ctrl+Alt+C: a caption at the playhead, its index in cueAdded, in one undo step.
    int addedIndex = -1;
    int addedCount = 0;
    QObject::connect(&rig.captions, &CaptionsBridge::cueAdded, [&](int index) {
        ++addedCount;
        addedIndex = index;
    });
    undo = rig.controller.session().undoCount();
    REQUIRE(rig.captions.runCommand(QStringLiteral("sequence.addCaptionAtPlayhead")));
    CHECK(rig.controller.session().undoCount() == undo + 1);
    CHECK(addedCount == 1);
    CHECK(addedIndex == 0);
    REQUIRE(rig.captions.cues().size() == 1);
    CHECK(rig.captions.cues().at(0).toMap().value(QStringLiteral("text")).toString() == QStringLiteral("New caption"));
    // It covers the playhead now: where it would go again is refused, and says so.
    CHECK(reasonOf(rig.captions, "sequence.addCaptionAtPlayhead") == QStringLiteral("A caption is already at the playhead"));
    REQUIRE(rig.controller.undo());
    CHECK(rig.captions.cues().isEmpty());
    REQUIRE(rig.captions.runCommand(QStringLiteral("sequence.addCaptionAtPlayhead"))); // back for the rest

    // A second caption at 2 s: Ctrl+Alt+Down goes to it from the start, Ctrl+Alt+Up back to the first.
    rig.controller.setPlayhead(f(50));
    REQUIRE(rig.captions.runCommand(QStringLiteral("sequence.addCaptionAtPlayhead")));
    REQUIRE(rig.captions.cues().size() == 2);
    const std::size_t before = rig.controller.session().undoCount();
    rig.controller.setPlayhead(Ticks{});
    CHECK(stateOf(rig.captions, "sequence.nextCaptionSegment").value(QStringLiteral("enabled")).toBool());
    CHECK(reasonOf(rig.captions, "sequence.previousCaptionSegment") == QStringLiteral("No caption before the playhead"));
    REQUIRE(rig.captions.runCommand(QStringLiteral("sequence.nextCaptionSegment")));
    CHECK(rig.controller.playhead() == f(50));
    CHECK(rig.captions.selectedCue() == 1); // Premiere selects the caption it goes to
    CHECK(rig.controller.session().undoCount() == before); // going somewhere is not an edit
    CHECK(reasonOf(rig.captions, "sequence.nextCaptionSegment") == QStringLiteral("No caption after the playhead"));
    CHECK_FALSE(rig.captions.runCommand(QStringLiteral("sequence.nextCaptionSegment")));
    CHECK(rig.captions.message() == QStringLiteral("No caption after the playhead"));
    REQUIRE(rig.captions.runCommand(QStringLiteral("sequence.previousCaptionSegment")));
    CHECK(rig.controller.playhead() == f(0));
    CHECK(rig.captions.selectedCue() == 0);
    CHECK_FALSE(rig.captions.runCommand(QStringLiteral("sequence.previousCaptionSegment")));
    CHECK(rig.captions.message() == QStringLiteral("No caption before the playhead"));
    CHECK(rig.controller.session().undoCount() == before);

    // goToCue(-1 / 1), the keyless form, also refuses politely.
    CHECK_FALSE(rig.captions.goToCue(-1));
    CHECK(rig.captions.message() == QStringLiteral("No caption before the playhead"));
    CHECK(rig.captions.goToCue(1));
}

TEST_CASE("Caption keys in the shell: the keys act and show the Captions tab, its text selected", "[app][captions][keys][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    INFO(join(shell.errors));
    REQUIRE(shell.errors.isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* captions = shell.engine.singletonInstance<CaptionsBridge*>("Wabcut", "Captions");
    auto* palette = shell.engine.singletonInstance<CommandPalette*>("Wabcut", "CommandPalette");
    REQUIRE(controller != nullptr);
    REQUIRE(captions != nullptr);
    REQUIRE(palette != nullptr);
    Timeline tl{kPicture};
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    spin(200);
    QQuickWindow* window = shell.window.get();

    // The palette used to say "Not available yet": the rows say why they cannot run now, and carry Premiere's keys.
    const auto paletteState = [&](const char* id) { return palette->commandState(QString::fromLatin1(id)); };
    CHECK(paletteState("sequence.addCaptionAtPlayhead").value(QStringLiteral("reason")).toString()
          == QStringLiteral("Add a caption track first"));
    CHECK(paletteState("sequence.nextCaptionSegment").value(QStringLiteral("reason")).toString()
          == QStringLiteral("Add a caption track first"));
    CHECK(paletteState("sequence.previousCaptionSegment").value(QStringLiteral("reason")).toString()
          == QStringLiteral("Add a caption track first"));
    CHECK(paletteState("sequence.addCaptionTrack").value(QStringLiteral("enabled")).toBool());
    CHECK(paletteState("sequence.addCaptionTrack").value(QStringLiteral("known")).toBool());
    CHECK_FALSE(paletteState("sequence.addCaptionTrack").value(QStringLiteral("shortcut")).toString().isEmpty());
    CHECK(paletteState("sequence.nextCaptionSegment").value(QStringLiteral("shortcut")).toString().endsWith(QStringLiteral("Down")));

    // Ctrl+Alt+A: a caption track, and the Transcript panel opens on its Captions tab.
    tap(window, Qt::Key_A, Qt::ControlModifier | Qt::AltModifier);
    REQUIRE(waitFor([&] { return captions->tracks().size() == 1; }));
    QQuickItem* panel = nullptr;
    REQUIRE(waitFor([&] { panel = shell.item("transcriptPanel"); return panel != nullptr; }));
    QObject* tabs = panel->findChild<QObject*>(QStringLiteral("transcriptTabs"));
    REQUIRE(tabs != nullptr);
    CHECK(waitFor([&] { return tabs->property("currentIndex").toInt() == 2; }));

    // Ctrl+Alt+C: the new caption's text is shown and selected, so typing replaces it.
    tap(window, Qt::Key_C, Qt::ControlModifier | Qt::AltModifier);
    REQUIRE(waitFor([&] { return captions->cues().size() == 1; }));
    // A hidden window lays its list out only when it renders a frame (a shown window does that itself); the page waits
    // for the list, then selects the new caption's text. One frame here, then the page's timer finds the row.
    (void)shell.grab();
    QQuickItem* row = nullptr;
    QObject* cueText = nullptr;
    REQUIRE(waitFor([&] {
        row = findNamed(window->contentItem(), QStringLiteral("captionCue_0"));
        cueText = row ? row->findChild<QObject*>(QStringLiteral("captionText")) : nullptr;
        return cueText != nullptr && cueText->property("activeFocus").toBool();
    }));
    CHECK(cueText->property("text").toString() == QStringLiteral("New caption"));
    CHECK(cueText->property("selectedText").toString() == QStringLiteral("New caption"));

    // A caption further on, then Ctrl+Alt+Down moves to it and selects it (the text field keeps the focus: the keys
    // still reach the command), Ctrl+Alt+Up comes back.
    controller->setPlayhead(f(50));
    tap(window, Qt::Key_C, Qt::ControlModifier | Qt::AltModifier);
    REQUIRE(waitFor([&] { return captions->cues().size() == 2; }));
    const std::size_t undo = controller->session().undoCount();
    controller->setPlayhead(Ticks{});
    tap(window, Qt::Key_Down, Qt::ControlModifier | Qt::AltModifier);
    REQUIRE(waitFor([&] { return controller->playhead() == f(50); }));
    CHECK(captions->selectedCue() == 1);
    CHECK(controller->session().undoCount() == undo);
    tap(window, Qt::Key_Up, Qt::ControlModifier | Qt::AltModifier);
    REQUIRE(waitFor([&] { return controller->playhead() == Ticks{}; }));
    CHECK(captions->selectedCue() == 0);
    tap(window, Qt::Key_Up, Qt::ControlModifier | Qt::AltModifier);
    CHECK(captions->message() == QStringLiteral("No caption before the playhead"));
    CHECK(paletteState("sequence.previousCaptionSegment").value(QStringLiteral("reason")).toString()
          == QStringLiteral("No caption before the playhead"));

    // The Captions tab after the keys: two cues on the track the keys acted on.
    QQuickItem* cues = findNamed(panel, QStringLiteral("captionCues"));
    REQUIRE(cues != nullptr);
    CHECK(waitFor([&] { return cues->property("count").toInt() == 2; }));
    spin(120);
    {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(outputDir() + QStringLiteral("/captions-keys.png")));
    }

    // The focus is still in the caption's text: Ctrl+Alt+A makes another track all the same.
    tap(window, Qt::Key_A, Qt::ControlModifier | Qt::AltModifier);
    CHECK(waitFor([&] { return captions->tracks().size() == 2; }));

    // Sequence > Captions: the four rows (with their keys) and the #37 sheet.
    QObject* sequenceMenu = shell.object("sequenceMenuBar");
    REQUIRE(sequenceMenu != nullptr);
    QObject* submenu = sequenceMenu->findChild<QObject*>(QStringLiteral("sequenceMenuCaptions"));
    REQUIRE(submenu != nullptr);
    CHECK(submenu->property("title").toString() == QStringLiteral("Captions"));
    for (const char* id : kCaptionKeys) {
        INFO(id);
        QObject* menuRow = submenu->findChild<QObject*>(QStringLiteral("menuCommand_") + QString::fromLatin1(id));
        REQUIRE(menuRow != nullptr);
        CHECK_FALSE(menuRow->property("shortcut").toString().isEmpty());
    }
    CHECK(submenu->findChild<QObject*>(QStringLiteral("menuCommand_transcript.correctFromScript")) != nullptr);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Caption keys in the palette: rows carry their keys and say why they cannot run; screenshots",
          "[app][captions][keys][qml][screenshots]") {
    struct Shot {
        bool dark;
        const char* file;
    };
    for (const Shot& shot : {Shot{true, "palette-captions.png"}, Shot{false, "palette-captions-light.png"}}) {
        INFO(shot.file);
        resetSettings();
        clearMessages();
        Shell shell;
        REQUIRE(shell.window != nullptr);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(shot.dark, "ultraviolet");
        if (!shot.dark)
            REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        auto* palette = shell.engine.singletonInstance<CommandPalette*>("Wabcut", "CommandPalette");
        REQUIRE(controller != nullptr);
        REQUIRE(palette != nullptr);
        Timeline tl{kPicture};
        tl.linkMatching();
        controller->resetProject(tl.state, tl.seq);
        spin(200);

        QObject* overlay = shell.object("commandPalette");
        REQUIRE(overlay != nullptr);
        REQUIRE(QMetaObject::invokeMethod(overlay, "openPalette"));
        REQUIRE(waitFor([&] { return overlay->property("opened").toBool(); }));
        auto* field = qobject_cast<QQuickItem*>(shell.object("paletteField"));
        REQUIRE(field != nullptr);
        field->setProperty("text", QStringLiteral("caption"));
        palette->setQuery(QStringLiteral("caption"));
        spin(600);

        const auto row = [&](const char* id) {
            const QString key = QStringLiteral("command:") + QString::fromLatin1(id);
            for (const QVariant& item : palette->results()) {
                if (item.toMap().value(QStringLiteral("key")).toString() == key)
                    return item.toMap();
            }
            return QVariantMap{};
        };
        const QVariantMap track = row("sequence.addCaptionTrack");
        REQUIRE_FALSE(track.isEmpty()); // the keymap row is listed at all (it used to be "Not available yet")
        CHECK(track.value(QStringLiteral("enabled")).toBool());
        CHECK(track.value(QStringLiteral("reason")).toString().isEmpty());
        CHECK_FALSE(track.value(QStringLiteral("shortcut")).toString().isEmpty());
        const QVariantMap addCue = row("sequence.addCaptionAtPlayhead");
        REQUIRE_FALSE(addCue.isEmpty());
        CHECK_FALSE(addCue.value(QStringLiteral("enabled")).toBool());
        CHECK(addCue.value(QStringLiteral("reason")).toString() == QStringLiteral("Add a caption track first"));
        CHECK_FALSE(addCue.value(QStringLiteral("shortcut")).toString().isEmpty());
        const QVariantMap next = row("sequence.nextCaptionSegment");
        REQUIRE_FALSE(next.isEmpty());
        CHECK(next.value(QStringLiteral("reason")).toString() == QStringLiteral("Add a caption track first"));

        // Scroll the list to the caption keys' keymap rows (the typed edit commands rank above them), the last of the
        // four at the edge so all four show: the keys in the right column and why they cannot run.
        int plain = -1;
        int previousRow = -1;
        for (const QVariant& item : palette->results()) {
            const QVariantMap entry = item.toMap();
            if (entry.value(QStringLiteral("pinned")).toBool())
                continue;
            ++plain;
            if (entry.value(QStringLiteral("key")).toString() == QStringLiteral("command:sequence.previousCaptionSegment"))
                previousRow = plain;
        }
        REQUIRE(previousRow >= 0);
        REQUIRE(QMetaObject::invokeMethod(overlay, "choose", Q_ARG(QVariant, previousRow)));
        spin(300);

        // QA (fix round 1, finding 1): the selected row is a disabled command and stays readable in the light theme.
        // Its title used to be drawn at Theme.disabledOpacity (40 %: 2.7:1 on the selected row) and its reason in the
        // amber `warning` (1.5:1 on a light card); both must reach 4.5:1 on the row's `sunken` background.
        {
            auto* list = qobject_cast<QQuickItem*>(shell.object("paletteList"));
            REQUIRE(list);
            QQuickItem* delegate = nullptr;
            REQUIRE(QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, delegate),
                                              Q_ARG(int, previousRow)));
            REQUIRE(delegate);
            auto* title = findNamed(delegate, QStringLiteral("paletteRowTitle_%1").arg(previousRow));
            auto* reason = findNamed(delegate, QStringLiteral("paletteRowReason_%1").arg(previousRow));
            REQUIRE(title);
            REQUIRE(reason);
            double opacity = 1.0;
            for (QQuickItem* item = title; item; item = item->parentItem()) {
                opacity *= item->opacity();
                if (item == delegate)
                    break;
            }
            CHECK(opacity == 1.0); // nothing dims the row's text
            const auto linear = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
            const auto luminance = [&](const QColor& c) {
                return 0.2126 * linear(c.redF()) + 0.7152 * linear(c.greenF()) + 0.0722 * linear(c.blueF());
            };
            const auto contrast = [&](const QColor& a, const QColor& b) {
                const double la = luminance(a);
                const double lb = luminance(b);
                return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
            };
            const QColor selected = shell.themeColor("sunken");
            INFO("title contrast " << contrast(title->property("color").value<QColor>(), selected)
                                   << ", reason contrast " << contrast(reason->property("color").value<QColor>(), selected));
            CHECK(contrast(title->property("color").value<QColor>(), selected) >= 4.5);
            CHECK(contrast(reason->property("color").value<QColor>(), selected) >= 4.5);
        }

        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(outputDir() + QLatin1Char('/') + QLatin1String(shot.file)));

        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
}
