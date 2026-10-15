// M5m (C.11): the app's clipboard on the model, offscreen; no window is shown.
//   * Cut / Copy / Paste / Paste Insert run clipboard.* through ProjectController: a paste keeps speed, reverse, freeze
//     frames, time remapping, effects and keyframes, transitions and markers, is one undo step, selects the pasted
//     clips and parks the playhead after them; copy takes exactly the selection (linked selection decides);
//   * the paste's notes become a message; the clipboard is shared with another engine (the assistant's);
//   * the system clipboard (application/x-wabcut-clipboard) carries a copy into another window / project, and a
//     document that cannot be read is refused (the tests save the system clipboard and put it back);
//   * Paste Attributes / Remove Attributes (bridge `Clipboard`): the dialog's data, the dry-run preview (applied /
//     skipped with reasons), apply as one undo step, Scale Attribute Times;
//   * the clip menu's Paste Insert / Paste Attributes… / Remove Attributes… and the dialog in the shell (screenshot).

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "ClipboardBridge.h"
#include "ProjectController.h"

#include <wabcut/command/Command.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/Clipboard.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QJSValue>
#include <QMimeData>
#include <QQuickItem>
#include <QQuickWindow>

#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using command::Json;

namespace {

std::vector<ClipId> allClips(const ProjectState& state, SequenceId seq) {
    std::vector<ClipId> out;
    state.findSequence(seq)->forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips)
            out.push_back(c.id);
    });
    return out;
}

const Clip& clipIn(const ProjectState& state, SequenceId seq, std::string_view ref) {
    return state.findSequence(seq)->findClip(clipAt(state, seq, ref))->clip();
}

/// "V1@12" of a clip in `state`.
std::string refOf(const ProjectState& state, SequenceId seq, ClipId id, Ticks shift = {}) {
    const Sequence& s = *state.findSequence(seq);
    const auto loc = s.findClip(id);
    const auto pos = s.trackPosition(loc->track->id);
    return std::string(pos->kind == TrackKind::Video ? "V" : "A") + std::to_string(pos->index + 1) + "@" +
           std::to_string((loc->clip().start() + shift).value / kRate.ticksPerUnit());
}

/// What a paste must carry (ids, start and link group are the paste's own).
void requireSameClip(const Clip& pasted, const Clip& original, bool sameMedia = true) {
    INFO("original clip " << original.id.value << ", pasted " << pasted.id.value);
    CHECK(pasted.id != original.id);
    if (sameMedia)
        CHECK(pasted.media == original.media);
    CHECK(pasted.duration() == original.duration());
    CHECK(pasted.sourceIn == original.sourceIn);
    CHECK(pasted.speed == original.speed);
    CHECK(pasted.reversed == original.reversed);
    CHECK(pasted.isFreeze() == original.isFreeze());
    CHECK(pasted.properties() == original.properties());
    CHECK(pasted.isRemapped() == original.isRemapped());
    if (original.isRemapped())
        CHECK(*pasted.timeRemap() == *original.timeRemap());
    REQUIRE(pasted.effects().size() == original.effects().size());
    for (std::size_t i = 0; i < original.effects().size(); ++i) {
        EffectInstance a = pasted.effects()[i];
        const EffectInstance& b = original.effects()[i];
        CHECK(a.id != b.id);
        a.id = b.id;
        CHECK(a == b);
    }
}

std::vector<ClipId> ids(const Timeline& tl, std::initializer_list<std::string_view> refs) {
    std::vector<ClipId> out;
    for (const std::string_view ref : refs)
        out.push_back(tl.clip(ref));
    return out;
}

/// The test machine's clipboard is the real one: kept, emptied for the test, and put back afterwards.
class SystemClipboardGuard {
public:
    SystemClipboardGuard() : m_saved(std::make_unique<QMimeData>()) {
        QClipboard* clipboard = QGuiApplication::clipboard();
        if (const QMimeData* now = clipboard->mimeData()) {
            for (const QString& format : now->formats())
                m_saved->setData(format, now->data(format));
        }
        clipboard->setMimeData(new QMimeData);
        spin(50);
    }
    ~SystemClipboardGuard() {
        QGuiApplication::clipboard()->setMimeData(m_saved.release());
        spin(50);
    }
    SystemClipboardGuard(const SystemClipboardGuard&) = delete;
    SystemClipboardGuard& operator=(const SystemClipboardGuard&) = delete;

private:
    std::unique_ptr<QMimeData> m_saved;
};

QByteArray systemClipboardDocument() {
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasFormat(ProjectController::clipboardMimeType()))
        return {};
    QByteArray bytes = mime->data(ProjectController::clipboardMimeType());
    while (!bytes.isEmpty() && bytes.back() == '\0')
        bytes.chop(1);
    return bytes;
}

QStringList strings(const QVariant& value) {
    return value.toStringList();
}

QVariantMap row(const QVariantMap& preview, int index) {
    return preview.value(QStringLiteral("rows")).toList().value(index).toMap();
}

/// A QML `var` holding a JS object, as a map.
QVariantMap mapOf(const QVariant& value) {
    if (value.metaType() == QMetaType::fromType<QJSValue>())
        return value.value<QJSValue>().toVariant().toMap();
    return value.toMap();
}

/// Two A/V pairs: A (10 frames) with Scale keyframes, opacity 0.5 and a blur on its video, -6 dB and a Gain effect on
/// its audio; B (20 frames) plain.
Timeline attributesTimeline(ProjectState& state) {
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaBbbbbbbbbbbbbbbbbbbb|
        A1 tgt |AaaaaaaaaaBbbbbbbbbbbbbbbbbbbb|
    )");
    tl.linkMatching();
    ProjectState s = tl.state;
    const ClipId a = tl.clip("V1@0");
    const ClipId aa = tl.clip("A1@0");
    s = ok(edit::addKeyframe(s, tl.seq, a, edit::ParamRef::of(ParamId::Scale), Keyframe{f(2), 0.5})).state;
    s = ok(edit::addKeyframe(s, tl.seq, a, edit::ParamRef::of(ParamId::Scale), Keyframe{f(8), 1.5})).state;
    s = ok(edit::setParameter(s, tl.seq, a, edit::ParamRef::of(ParamId::Opacity), ParamValue(0.5))).state;
    s = ok(edit::addEffect(s, tl.seq, std::span(&a, 1), "wabcut.blur.gaussian")).state;
    s = ok(edit::setParameter(s, tl.seq, aa, edit::ParamRef::of(ParamId::VolumeDb), ParamValue(-6.0))).state;
    s = ok(edit::addEffect(s, tl.seq, std::span(&aa, 1), "wabcut.audio.gain")).state;
    requireValid(s);
    state = std::move(s);
    return tl;
}

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

QObject* openDialog(Shell& shell, const char* loaderName) {
    QObject* loader = shell.object(loaderName);
    REQUIRE(loader);
    QObject* dialog = nullptr;
    REQUIRE(waitFor([&] {
        dialog = loader->property("item").value<QObject*>();
        return dialog && dialog->property("opened").toBool();
    }, 5000));
    return dialog;
}

QQuickItem* inDialog(QObject* dialog, const char* name) {
    auto* content = dialog->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    return findNamed(content->parentItem(), QString::fromLatin1(name));
}

void openMenu(QObject* menu, QQuickItem* target, const QString& clip) {
    REQUIRE(QMetaObject::invokeMethod(menu, "openAt", Q_ARG(QVariant, QVariant::fromValue<QObject*>(target)),
                                      Q_ARG(QVariant, clip), Q_ARG(QVariant, QPointF(200, 20))));
}

QObject* menuItem(QObject* menu, const char* name) {
    QObject* item = menu->findChild<QObject*>(QString::fromLatin1(name));
    REQUIRE(item);
    return item;
}

bool menuEnabled(QObject* menu, const char* name) {
    return menuItem(menu, name)->property("enabled").toBool();
}

} // namespace

TEST_CASE("Clipboard UI: copy and paste keep speed, reverse, freeze, time remapping, effects and keyframes",
          "[app][clipboard]") {
    // The app's own paste placed clips at 100 % and lost these: the model clipboard carries them.
    Timeline tl(R"(
        V2     |..Dddddd............................|
        V1 tgt |AaaaaaaaaaB5bbbbbbbbCccccccccc......|
        A1 tgt |AaaaaaaaaaB5bbbbbbbbCccccccccc......|
    )");
    tl.linkMatching();
    ProjectState s = tl.state;
    const ClipId b = tl.clip("V1@10");
    const ClipId c = tl.clip("V1@20");
    edit::SpeedChange half;
    half.speed = {1, 2};
    half.keepDuration = true;
    s = ok(edit::setSpeed(s, tl.seq, std::span(&b, 1), half)).state;
    edit::SpeedChange reverse;
    reverse.speed = {1, 1};
    reverse.reversed = true;
    reverse.keepDuration = true;
    s = ok(edit::setSpeed(s, tl.seq, std::span(&c, 1), reverse)).state;
    s = ok(edit::frameHold(s, tl.seq, tl.clip("V1@0"), f(5))).state;
    s = ok(edit::setTimeRemapEnabled(s, tl.seq, tl.clip("V2@2"), true)).state;
    s = ok(edit::addEffect(s, tl.seq, std::span(&b, 1), "wabcut.blur.gaussian")).state;
    const Clip& bClip = s.findSequence(tl.seq)->findClip(b)->clip();
    s = ok(edit::addKeyframe(s, tl.seq, b, edit::ParamRef::of(ParamId::Opacity), Keyframe{bClip.sourceIn + f(1), 0.25})).state;
    requireValid(s);

    ProjectController controller;
    controller.resetProject(s, tl.seq);
    const std::vector<ClipId> copied = allClips(s, tl.seq);
    controller.setSelection(copied);
    REQUIRE(controller.runCommand(QStringLiteral("edit.copy")));
    CHECK(controller.lastError().isEmpty());
    CHECK(controller.canPaste());
    CHECK(controller.session().undoCount() == 0); // copying changes nothing

    controller.setPlayhead(f(40));
    REQUIRE(controller.runCommand(QStringLiteral("edit.paste")));
    INFO(controller.lastError().toStdString());
    CHECK(controller.session().undoCount() == 1);
    CHECK(controller.undoText() == QStringLiteral("Undo Paste"));
    CHECK(controller.playhead() == f(70)); // after the pasted clips
    CHECK(controller.selection().size() == copied.size()); // the pasted clips are selected
    const ProjectSnapshot after = controller.snapshot();
    requireValid(*after);
    for (const ClipId id : copied) {
        const Clip& original = s.findSequence(tl.seq)->findClip(id)->clip();
        const Clip& pasted = clipIn(*after, tl.seq, refOf(s, tl.seq, id, f(40)));
        requireSameClip(pasted, original);
        CHECK(controller.isSelected(pasted.id));
    }
    // The pasted pairs are linked to each other.
    const Clip& pv = clipIn(*after, tl.seq, "V1@60");
    const Clip& pa = clipIn(*after, tl.seq, "A1@60");
    CHECK(pv.linkGroup.isValid());
    CHECK(pv.linkGroup == pa.linkGroup);
    CHECK(pv.linkGroup != clipIn(s, tl.seq, "V1@20").linkGroup);

    // One undo step takes it all back.
    REQUIRE(controller.undo());
    CHECK(render(*controller.snapshot(), tl.seq) == render(s, tl.seq));
}

TEST_CASE("Clipboard UI: transitions and markers among the copied clips come along", "[app][clipboard]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaB5bbbbbbbb......................|
        A1 tgt |AaaaaaaaaaB5bbbbbbbb......................|
    )");
    tl.linkMatching();
    ProjectState s = tl.state;
    edit::TransitionSpec spec;
    spec.outgoing = tl.clip("V1@0");
    spec.incoming = tl.clip("V1@10");
    spec.before = f(2);
    spec.after = f(2);
    s = ok(edit::addTransition(s, tl.seq, tl.track("V1"), spec)).state;
    s = ok(edit::addMarker(s, tl.seq, Marker{f(12), Ticks{}, "Logo"})).state;
    s = ok(edit::addMarker(s, tl.seq, Marker{f(25), Ticks{}, "Outside"})).state;

    ProjectController controller;
    controller.resetProject(s, tl.seq);
    controller.setSelection(ids(tl, {"V1@0", "A1@0", "V1@10", "A1@10"}));
    controller.copySelection();
    controller.setPlayhead(f(22));
    controller.paste();
    INFO(controller.lastError().toStdString());
    CHECK(controller.session().undoCount() == 1);
    const ProjectSnapshot after = controller.snapshot();
    CHECK_TIMELINE(*after, tl.seq, R"(
        V1 |AaaaaaaaaaB5bbbbbbbb..AaaaaaaaaaB5bbbbbbbb|
        A1 |AaaaaaaaaaB5bbbbbbbb..AaaaaaaaaaB5bbbbbbbb|
    )");
    CHECK(links(*after, tl.seq) == "V1@0+A1@0 V1@10+A1@10 V1@22+A1@22 V1@32+A1@32");
    const Track& v1 = *after->findSequence(tl.seq)->findTrack(tl.track("V1"));
    REQUIRE(v1.transitions.size() == 2);
    CHECK(v1.transitions[1].outgoing == clipAt(*after, tl.seq, "V1@22"));
    CHECK(v1.transitions[1].incoming == clipAt(*after, tl.seq, "V1@32"));
    const auto& markers = after->findSequence(tl.seq)->markers;
    REQUIRE(markers.size() == 3); // "Logo" again at 22 + 12; "Outside" was not in the copied range
    CHECK(markers[2].start == f(34));
    CHECK(markers[2].name == "Logo");
    CHECK(controller.playhead() == f(42));
}

TEST_CASE("Clipboard UI: paste insert, cut, linked selection, the playhead and canPaste", "[app][clipboard]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbbCccc........|
        A1 tgt |AaaaBbbbCccc........|
    )");
    tl.linkMatching();
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    const auto available = [&](const char* id) {
        return controller.commandAvailability(QString::fromLatin1(id)).value(QStringLiteral("enabled")).toBool();
    };
    CHECK_FALSE(controller.canPaste());
    CHECK_FALSE(available("edit.paste"));
    CHECK_FALSE(available("edit.pasteInsert"));
    int clipboardChanges = 0;
    QObject::connect(&controller, &ProjectController::clipboardChanged, [&] { ++clipboardChanges; });

    controller.setSelection(ids(tl, {"V1@4", "A1@4"}));
    controller.copySelection();
    CHECK(clipboardChanges >= 1);
    CHECK(controller.canPaste());
    CHECK(available("edit.paste"));
    CHECK(available("edit.pasteInsert"));

    SECTION("Paste Insert opens the clipboard's length at the playhead") {
        controller.setPlayhead(f(0));
        REQUIRE(controller.runCommand(QStringLiteral("edit.pasteInsert")));
        CHECK_TIMELINE(*controller.snapshot(), tl.seq, R"(
            V1 |BbbbAaaaBbbbCccc....|
            A1 |BbbbAaaaBbbbCccc....|
        )");
        CHECK(controller.session().undoCount() == 1);
        CHECK(controller.undoText() == QStringLiteral("Undo Paste Insert"));
        CHECK(controller.playhead() == f(4));
        std::vector<ClipId> selected = controller.selection();
        std::ranges::sort(selected);
        std::vector<ClipId> pasted{clipAt(*controller.snapshot(), tl.seq, "V1@0"), clipAt(*controller.snapshot(), tl.seq, "A1@0")};
        std::ranges::sort(pasted);
        CHECK(selected == pasted);
    }
    SECTION("Paste overwrites at the playhead") {
        controller.setPlayhead(f(12));
        controller.paste();
        CHECK_TIMELINE(*controller.snapshot(), tl.seq, R"(
            V1 |AaaaBbbbCcccBbbb....|
            A1 |AaaaBbbbCcccBbbb....|
        )");
        CHECK(controller.playhead() == f(16));
        CHECK(controller.undoText() == QStringLiteral("Undo Paste"));
        // The clipboard stays: a second paste at the new playhead.
        controller.paste();
        CHECK_TIMELINE(*controller.snapshot(), tl.seq, R"(
            V1 |AaaaBbbbCcccBbbbBbbb|
            A1 |AaaaBbbbCcccBbbbBbbb|
        )");
        CHECK(controller.session().undoCount() == 2);
    }
    SECTION("Copy takes exactly the selection: a video clip selected without its audio pastes alone") {
        controller.setSelection({tl.clip("V1@4")});
        controller.copySelection();
        controller.setPlayhead(f(12));
        controller.paste();
        CHECK_TIMELINE(*controller.snapshot(), tl.seq, R"(
            V1 |AaaaBbbbCcccBbbb....|
            A1 |AaaaBbbbCccc........|
        )");
    }
    SECTION("Cut leaves a gap and fills the clipboard; Undo Cut") {
        controller.setSelection(ids(tl, {"V1@8", "A1@8"}));
        controller.cutSelection();
        CHECK_TIMELINE(*controller.snapshot(), tl.seq, R"(
            V1 |AaaaBbbb............|
            A1 |AaaaBbbb............|
        )");
        CHECK(controller.undoText() == QStringLiteral("Undo Cut"));
        controller.setPlayhead(f(14));
        controller.paste();
        CHECK_TIMELINE(*controller.snapshot(), tl.seq, R"(
            V1 |AaaaBbbb......Cccc..|
            A1 |AaaaBbbb......Cccc..|
        )");
        CHECK(controller.session().undoCount() == 2);
    }
    SECTION("A locked destination refuses with a message, and nothing changes") {
        controller.setTrackFlag(trackKey(tl.track("V1")), QStringLiteral("locked"), true);
        const auto before = controller.session().undoCount();
        controller.setPlayhead(f(12));
        controller.paste();
        CHECK_FALSE(controller.lastError().isEmpty());
        CHECK(controller.session().undoCount() == before);
    }
}

TEST_CASE("Clipboard UI: paste notes become a message; another engine shares the clipboard", "[app][clipboard]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbbCccc........|
        A1 tgt |AaaaBbbbCccc........|
    )");
    tl.linkMatching();
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    ClipboardBridge bridge;
    bridge.setController(&controller);
    QStringList notices;
    QObject::connect(&controller, &ProjectController::clipboardNotice, [&](const QString& text) { notices << text; });

    // The assistant's engine bundles the same clipboard: it sees the timeline's copy, and its copy pastes here.
    controller.setSelection(ids(tl, {"V1@4", "A1@4"}));
    controller.copySelection();
    command::EnginePolicy policy;
    policy.services = controller.clipboardServices();
    command::CommandEngine assistant(controller.session(), command::builtinCommands(), policy);
    command::CommandRequest query;
    query.command = "clipboard.get";
    query.params = Json{{"sequence", tl.seq.value}};
    const command::CommandResult held = assistant.execute(query);
    REQUIRE(held.ok());
    CHECK(held.data.value("clips", 0) == 2);
    command::CommandRequest copy;
    copy.command = "clipboard.copy";
    copy.params = Json{{"sequence", tl.seq.value}, {"clips", Json::array({tl.clip("V1@8").value})}, {"linked", false}};
    REQUIRE(assistant.execute(copy).ok());
    controller.notifyClipboardChanged();
    controller.setPlayhead(f(16));
    controller.paste();
    CHECK_TIMELINE(*controller.snapshot(), tl.seq, R"(
        V1 |AaaaBbbbCccc....Cccc|
        A1 |AaaaBbbbCccc........|
    )");
    CHECK(notices.isEmpty()); // the project's own media: nothing to say

    // Another project without media B: B is added, and the paste says so.
    controller.setSelection(ids(tl, {"V1@4", "A1@4"}));
    controller.copySelection();
    Timeline other(R"(
        V1 tgt |Zzzz........|
        A1 tgt |Zzzz........|
    )");
    auto withoutB = edit::removeMedia(other.state, other.media('B'));
    REQUIRE(withoutB);
    controller.resetProject(*withoutB, other.seq);
    controller.setPlayhead(f(4));
    controller.paste();
    INFO(controller.lastError().toStdString());
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].contains(QStringLiteral("'B': added to the project")));
    CHECK(bridge.message() == notices[0]);
    CHECK(controller.snapshot()->media.size() == withoutB->media.size() + 1);
    CHECK_TIMELINE(*controller.snapshot(), other.seq, R"(
        V1 |ZzzzBbbb....|
        A1 |ZzzzBbbb....|
    )");
}

TEST_CASE("Clipboard UI: the system clipboard carries a copy to another window and project", "[app][clipboard][system]") {
    SystemClipboardGuard guard;
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaB5bbbbbbbb|
        A1 tgt |AaaaaaaaaaB5bbbbbbbb|
    )");
    tl.linkMatching();
    ProjectState s = tl.state;
    const ClipId b = tl.clip("V1@10");
    edit::SpeedChange half;
    half.speed = {1, 2};
    half.keepDuration = true;
    s = ok(edit::setSpeed(s, tl.seq, std::span(&b, 1), half)).state;
    s = ok(edit::addEffect(s, tl.seq, std::span(&b, 1), "wabcut.blur.gaussian")).state;

    ProjectController first;
    first.setSystemClipboardEnabled(true);
    first.resetProject(s, tl.seq);
    CHECK_FALSE(first.canPaste());
    first.setSelection(ids(tl, {"V1@10", "A1@10"}));
    first.copySelection();
    REQUIRE(waitFor([] { return !systemClipboardDocument().isEmpty(); }, 5000));
    const QByteArray document = systemClipboardDocument();
    const auto parsed = clipboardFromJson(std::string_view(document.constData(), static_cast<std::size_t>(document.size())));
    REQUIRE(parsed);
    CHECK(parsed->clipCount() == 2);
    spin(50);
    CHECK_FALSE(first.adoptSystemClipboard()); // its own copy: nothing to take

    // Another window with another project (no media B): it can paste, and the paste is the copied clips.
    Timeline other(R"(
        V1 tgt |Zzzz........|
        A1 tgt |Zzzz........|
    )");
    auto withoutB = edit::removeMedia(other.state, other.media('B'));
    REQUIRE(withoutB);
    ProjectController second;
    second.resetProject(*withoutB, other.seq);
    CHECK_FALSE(second.canPaste());
    second.setSystemClipboardEnabled(true);
    REQUIRE(waitFor([&] { return second.canPaste(); }, 5000));
    QStringList notices;
    QObject::connect(&second, &ProjectController::clipboardNotice, [&](const QString& text) { notices << text; });
    second.setPlayhead(f(4));
    second.paste();
    INFO(second.lastError().toStdString());
    CHECK(second.session().undoCount() == 1);
    const Clip& pasted = clipIn(*second.snapshot(), other.seq, "V1@4");
    requireSameClip(pasted, s.findSequence(tl.seq)->findClip(b)->clip(), false); // B was added to that project
    CHECK(second.snapshot()->findMedia(pasted.media)->name == "B");
    CHECK(links(*second.snapshot(), other.seq) == "V1@4+A1@4");
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].contains(QStringLiteral("added to the project")));
    CHECK(second.playhead() == f(14));
    CHECK_FALSE(second.adoptSystemClipboard()); // taken once: now its own clipboard

    // A clipboard document that cannot be read is refused with a message, not swapped for the older copy.
    int changes = 0;
    QObject::connect(&second, &ProjectController::clipboardChanged, [&] { ++changes; });
    auto* junk = new QMimeData;
    junk->setData(ProjectController::clipboardMimeType(), QByteArrayLiteral("{\"format\":\"not a clipboard\"}"));
    QGuiApplication::clipboard()->setMimeData(junk);
    REQUIRE(waitFor([&] { return changes > 0; }, 5000));
    CHECK(second.canPaste());
    const auto before = second.session().undoCount();
    second.paste();
    CHECK(second.lastError().contains(QStringLiteral("cannot read")));
    CHECK(second.session().undoCount() == before);

    // Off: the controller ignores the system clipboard.
    second.setSystemClipboardEnabled(false);
    CHECK_FALSE(second.adoptSystemClipboard());
}

TEST_CASE("Clipboard UI: Paste Attributes data, preview, one undo step and scaled times", "[app][clipboard][attributes]") {
    ProjectState s;
    Timeline tl = attributesTimeline(s);
    ProjectController controller;
    controller.resetProject(s, tl.seq);
    ClipboardBridge bridge;
    bridge.setController(&controller);
    CHECK_FALSE(bridge.canPasteAttributes());

    controller.setSelection(ids(tl, {"V1@0", "A1@0"}));
    controller.copySelection();
    controller.setSelection(ids(tl, {"V1@10", "A1@10"}));
    CHECK(bridge.canPasteAttributes());
    CHECK(bridge.canRemoveAttributes());

    const QVariantMap info = bridge.attributesInfo(QStringLiteral("paste"));
    CHECK(info.value(QStringLiteral("available")).toBool());
    CHECK(info.value(QStringLiteral("targets")).toInt() == 2);
    CHECK(info.value(QStringLiteral("source")).toString() == QStringLiteral("A"));
    const QVariantList videoEffects = info.value(QStringLiteral("video")).toMap().value(QStringLiteral("effects")).toList();
    REQUIRE(videoEffects.size() == 1);
    CHECK(videoEffects[0].toMap().value(QStringLiteral("key")).toString() == QStringLiteral("0"));
    CHECK(videoEffects[0].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Gaussian Blur"));
    const QVariantList audioEffects = info.value(QStringLiteral("audio")).toMap().value(QStringLiteral("effects")).toList();
    REQUIRE(audioEffects.size() == 1);
    CHECK(audioEffects[0].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Gain"));

    QVariantMap choices{{QStringLiteral("motion"), true},
                        {QStringLiteral("opacity"), true},
                        {QStringLiteral("videoEffects"), QStringList{QStringLiteral("0")}},
                        {QStringLiteral("volume"), true},
                        {QStringLiteral("panner"), true},
                        {QStringLiteral("audioEffects"), QStringList{QStringLiteral("0")}}};
    // The preview changes nothing; it lists what each clip takes and skips.
    const ProjectSnapshot base = controller.snapshot();
    const QVariantMap preview = bridge.previewAttributes(QStringLiteral("paste"), choices);
    CHECK(controller.snapshot() == base);
    CHECK(controller.session().undoCount() == 0);
    CHECK(preview.value(QStringLiteral("ok")).toBool());
    REQUIRE(preview.value(QStringLiteral("rows")).toList().size() == 2);
    CHECK(row(preview, 0).value(QStringLiteral("clip")).toString() == clipKey(tl.clip("V1@10")));
    CHECK(row(preview, 0).value(QStringLiteral("kind")).toString() == QStringLiteral("video"));
    CHECK(strings(row(preview, 0).value(QStringLiteral("applied"))) ==
          QStringList{QStringLiteral("Motion"), QStringLiteral("Opacity"), QStringLiteral("Gaussian Blur")});
    CHECK(strings(row(preview, 1).value(QStringLiteral("applied"))) ==
          QStringList{QStringLiteral("Volume"), QStringLiteral("Panner"), QStringLiteral("Gain")});

    // Apply: one undo step; keyframes keep their time from the clip's start.
    REQUIRE(bridge.applyAttributes(QStringLiteral("paste"), choices).isEmpty());
    CHECK(controller.session().undoCount() == 1);
    CHECK(controller.undoText() == QStringLiteral("Undo Paste Attributes"));
    CHECK(bridge.message() == QStringLiteral("Paste Attributes: 2 of 2 clips changed"));
    {
        const ProjectSnapshot after = controller.snapshot();
        const Clip& video = clipIn(*after, tl.seq, "V1@10");
        const Curve& scale = std::get<Curve>(video.properties()[ParamId::Scale].data);
        REQUIRE(scale.keys.size() == 2);
        CHECK(scale.keys[0].time == f(2));
        CHECK(scale.keys[1].time == f(8));
        CHECK(video.properties()[ParamId::Opacity] == ParamValue(0.5));
        REQUIRE(video.effects().size() == 1);
        CHECK(video.effects()[0].effect == "wabcut.blur.gaussian");
        const Clip& audio = clipIn(*after, tl.seq, "A1@10");
        CHECK(audio.properties()[ParamId::VolumeDb] == ParamValue(-6.0));
        REQUIRE(audio.effects().size() == 1);
        CHECK(audio.effects()[0].effect == "wabcut.audio.gain");
    }
    REQUIRE(controller.undo());

    // Scale Attribute Times: B is twice as long, so the keys land twice as far in.
    QVariantMap scaled{{QStringLiteral("motion"), true}, {QStringLiteral("scaleTimes"), true}};
    REQUIRE(bridge.applyAttributes(QStringLiteral("paste"), scaled).isEmpty());
    {
        const Curve& stretched =
            std::get<Curve>(clipIn(*controller.snapshot(), tl.seq, "V1@10").properties()[ParamId::Scale].data);
        REQUIRE(stretched.keys.size() == 2);
        CHECK(stretched.keys[0].time == f(4));
        CHECK(stretched.keys[1].time == f(16));
    }
    // Only video attributes chosen: the audio clip is skipped with the reason, the video one still takes them.
    const QVariantMap videoOnly = bridge.previewAttributes(QStringLiteral("paste"), scaled);
    CHECK(strings(row(videoOnly, 1).value(QStringLiteral("skipped"))) == QStringList{QStringLiteral("no audio attributes chosen")});
    REQUIRE(controller.undo());

    // A clipboard holding only a video clip: audio attributes have no source.
    controller.setSelection({tl.clip("V1@0")});
    controller.copySelection();
    controller.setSelection(ids(tl, {"V1@10", "A1@10"}));
    const QVariantMap noAudio = bridge.attributesInfo(QStringLiteral("paste"));
    CHECK_FALSE(noAudio.value(QStringLiteral("audio")).toMap().value(QStringLiteral("available")).toBool());
    CHECK(noAudio.value(QStringLiteral("audio")).toMap().value(QStringLiteral("reason")).toString() ==
          QStringLiteral("The clipboard has no audio clip"));
    const QVariantMap volumeOnly{{QStringLiteral("volume"), true}};
    const QVariantMap refused = bridge.previewAttributes(QStringLiteral("paste"), volumeOnly);
    CHECK_FALSE(refused.value(QStringLiteral("ok")).toBool());
    CHECK_FALSE(refused.value(QStringLiteral("error")).toString().isEmpty());
    CHECK(strings(row(refused, 1).value(QStringLiteral("skipped"))) == QStringList{QStringLiteral("the clipboard has no audio clip")});
    const auto before = controller.session().undoCount();
    CHECK_FALSE(bridge.applyAttributes(QStringLiteral("paste"), volumeOnly).isEmpty());
    CHECK(controller.session().undoCount() == before);

    // Nothing selected: the dialog does not open, and says why.
    controller.clearSelection();
    CHECK_FALSE(bridge.canPasteAttributes());
    int requested = 0;
    QObject::connect(&bridge, &ClipboardBridge::attributesRequested, [&] { ++requested; });
    CHECK_FALSE(bridge.openAttributes(QStringLiteral("paste")));
    CHECK(requested == 0);
    CHECK_FALSE(bridge.message().isEmpty());
    controller.setSelection({tl.clip("V1@10")});
    CHECK(bridge.openAttributes(QStringLiteral("paste")));
    CHECK(requested == 1);
}

TEST_CASE("Clipboard UI: Remove Attributes lists the selection's effects and removes them in one undo step",
          "[app][clipboard][attributes]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaBbbbbbbbbb|
    )");
    ProjectState s = tl.state;
    const ClipId a = tl.clip("V1@0");
    s = ok(edit::addKeyframe(s, tl.seq, a, edit::ParamRef::of(ParamId::Scale), Keyframe{f(2), 0.5})).state;
    s = ok(edit::addEffect(s, tl.seq, std::span(&a, 1), "wabcut.blur.gaussian")).state;
    s = ok(edit::addEffect(s, tl.seq, std::span(&a, 1), "wabcut.sharpen")).state;
    s = ok(edit::setTimeRemapEnabled(s, tl.seq, a, true)).state;
    ProjectController controller;
    controller.resetProject(s, tl.seq);
    ClipboardBridge bridge;
    bridge.setController(&controller);
    controller.setSelection({a});
    CHECK(bridge.canRemoveAttributes());
    CHECK_FALSE(bridge.canPasteAttributes()); // nothing copied

    const QVariantMap info = bridge.attributesInfo(QStringLiteral("remove"));
    CHECK(info.value(QStringLiteral("available")).toBool());
    const QVariantList effects = info.value(QStringLiteral("video")).toMap().value(QStringLiteral("effects")).toList();
    REQUIRE(effects.size() == 2);
    CHECK(effects[0].toMap().value(QStringLiteral("key")).toString() == QStringLiteral("wabcut.blur.gaussian"));
    CHECK(effects[1].toMap().value(QStringLiteral("key")).toString() == QStringLiteral("wabcut.sharpen"));
    CHECK(effects[1].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Sharpen"));
    CHECK_FALSE(info.value(QStringLiteral("audio")).toMap().value(QStringLiteral("available")).toBool());

    const QVariantMap choices{{QStringLiteral("motion"), true},
                              {QStringLiteral("timeRemap"), true},
                              {QStringLiteral("videoEffects"), QStringList{QStringLiteral("wabcut.blur.gaussian")}}};
    const QVariantMap preview = bridge.previewAttributes(QStringLiteral("remove"), choices);
    CHECK(preview.value(QStringLiteral("ok")).toBool());
    CHECK(strings(row(preview, 0).value(QStringLiteral("applied"))) ==
          QStringList{QStringLiteral("Time Remapping"), QStringLiteral("Motion"), QStringLiteral("Gaussian Blur")});
    CHECK(controller.session().undoCount() == 0);
    REQUIRE(bridge.applyAttributes(QStringLiteral("remove"), choices).isEmpty());
    CHECK(controller.session().undoCount() == 1);
    CHECK(controller.undoText() == QStringLiteral("Undo Remove Attributes"));
    {
        const Clip& after = clipIn(*controller.snapshot(), tl.seq, "V1@0");
        CHECK_FALSE(after.isRemapped());
        CHECK(after.properties()[ParamId::Scale] == ClipProperties()[ParamId::Scale]);
        REQUIRE(after.effects().size() == 1);
        CHECK(after.effects()[0].effect == "wabcut.sharpen");
    }
    // Again: nothing left of those to remove.
    const QVariantMap again = bridge.previewAttributes(QStringLiteral("remove"), choices);
    CHECK_FALSE(again.value(QStringLiteral("ok")).toBool());
    CHECK_FALSE(again.value(QStringLiteral("error")).toString().isEmpty());
    // All effects.
    REQUIRE(bridge.applyAttributes(QStringLiteral("remove"), QVariantMap{{QStringLiteral("allEffects"), true}}).isEmpty());
    CHECK(clipIn(*controller.snapshot(), tl.seq, "V1@0").effects().empty());
    CHECK(controller.session().undoCount() == 2);
    // Nothing chosen.
    CHECK_FALSE(bridge.applyAttributes(QStringLiteral("remove"), QVariantMap{}).isEmpty());
}

TEST_CASE("Clipboard UI in the shell: clip menu items, the attributes dialog and its preview; screenshot",
          "[app][clipboard][qml][screenshot]") {
    SystemClipboardGuard guard; // the app's controller uses the system clipboard
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* clipboard = shell.engine.singletonInstance<ClipboardBridge*>("Wabcut", "Clipboard");
    REQUIRE(controller);
    REQUIRE(clipboard);
    CHECK(controller->systemClipboardEnabled());
    ProjectState s;
    Timeline tl = attributesTimeline(s);
    controller->resetProject(s, tl.seq);
    QQuickWindow* w = shell.window.get();
    REQUIRE_FALSE(shell.grab().isNull());
    QObject* menu = w->findChild<QObject*>(QStringLiteral("clipContextMenu"));
    REQUIRE(menu);
    QQuickItem* lanes = findNamed(w->contentItem(), QStringLiteral("timelineLanesArea"));
    REQUIRE(lanes);

    // Nothing copied: the paste items are off; Remove Attributes… needs only a selection.
    controller->setSelection(ids(tl, {"V1@0", "A1@0"}));
    openMenu(menu, lanes, clipKey(tl.clip("V1@0")));
    CHECK_FALSE(menuEnabled(menu, "pasteMenuItem"));
    CHECK_FALSE(menuEnabled(menu, "pasteInsertMenuItem"));
    CHECK_FALSE(menuEnabled(menu, "pasteAttributesMenuItem"));
    CHECK(menuEnabled(menu, "removeAttributesMenuItem"));
    QMetaObject::invokeMethod(menu, "close");

    // Copy A; B selected: every item is on.
    controller->copySelection();
    REQUIRE(waitFor([] { return !systemClipboardDocument().isEmpty(); }, 5000));
    controller->setSelection(ids(tl, {"V1@10", "A1@10"}));
    openMenu(menu, lanes, clipKey(tl.clip("V1@10")));
    CHECK(menuEnabled(menu, "pasteMenuItem"));
    CHECK(menuEnabled(menu, "pasteInsertMenuItem"));
    CHECK(menuEnabled(menu, "pasteAttributesMenuItem"));
    CHECK(menuEnabled(menu, "removeAttributesMenuItem"));

    // Paste Attributes… opens the dialog (Main.qml routes Clipboard.attributesRequested to its loader).
    REQUIRE(QMetaObject::invokeMethod(menuItem(menu, "pasteAttributesMenuItem"), "triggered"));
    QMetaObject::invokeMethod(menu, "close");
    QObject* dialog = openDialog(shell, "attributesDialogLoader");
    CHECK(dialog->property("mode").toString() == QStringLiteral("paste"));
    QQuickItem* motion = inDialog(dialog, "attrMotion");
    REQUIRE(motion);
    CHECK(motion->property("checked").toBool());
    QQuickItem* blur = inDialog(dialog, "attrVideoEffect0");
    REQUIRE(blur);
    CHECK(blur->property("text").toString() == QStringLiteral("Gaussian Blur"));
    QQuickItem* gain = inDialog(dialog, "attrAudioEffect0");
    REQUIRE(gain);
    CHECK(gain->property("text").toString() == QStringLiteral("Gain"));
    QQuickItem* scaleTimes = inDialog(dialog, "attrScaleTimes");
    REQUIRE(scaleTimes);
    CHECK(scaleTimes->isVisible());
    QQuickItem* allEffects = inDialog(dialog, "attrAllEffects");
    REQUIRE(allEffects);
    CHECK_FALSE(allEffects->isVisible());
    QQuickItem* rows = inDialog(dialog, "attributesPreviewRows");
    REQUIRE(rows);
    CHECK(rows->property("count").toInt() == 2);
    {
        const QVariantMap preview = mapOf(dialog->property("preview"));
        CHECK(preview.value(QStringLiteral("ok")).toBool());
        CHECK(strings(row(preview, 0).value(QStringLiteral("applied"))).contains(QStringLiteral("Motion")));
    }
    // Motion off: the preview follows.
    motion->setProperty("checked", false);
    REQUIRE(QMetaObject::invokeMethod(motion, "toggled"));
    {
        const QVariantMap preview = mapOf(dialog->property("preview"));
        const QStringList applied = strings(row(preview, 0).value(QStringLiteral("applied")));
        CHECK_FALSE(applied.contains(QStringLiteral("Motion")));
        CHECK(applied.contains(QStringLiteral("Gaussian Blur")));
    }
    spin(150);
    {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(QDir(outputDir()).filePath(QStringLiteral("paste-attributes.png"))));
    }
    QQuickItem* okButton = inDialog(dialog, "attributesOk");
    REQUIRE(okButton);
    CHECK(okButton->property("enabled").toBool());
    const auto before = controller->session().undoCount();
    REQUIRE(QMetaObject::invokeMethod(okButton, "clicked"));
    CHECK(controller->session().undoCount() == before + 1);
    REQUIRE(waitFor([&] { return !dialog->property("opened").toBool(); }, 5000));
    CHECK(clipIn(*controller->snapshot(), tl.seq, "V1@10").effects().size() == 1);
    CHECK(clipboard->message() == QStringLiteral("Paste Attributes: 2 of 2 clips changed"));

    // Remove Attributes… on B: its own blur is listed; All effects instead of Scale Attribute Times.
    openMenu(menu, lanes, clipKey(tl.clip("V1@10")));
    REQUIRE(QMetaObject::invokeMethod(menuItem(menu, "removeAttributesMenuItem"), "triggered"));
    QMetaObject::invokeMethod(menu, "close");
    dialog = openDialog(shell, "attributesDialogLoader");
    CHECK(dialog->property("mode").toString() == QStringLiteral("remove"));
    CHECK(inDialog(dialog, "attrAllEffects")->isVisible());
    CHECK_FALSE(inDialog(dialog, "attrScaleTimes")->isVisible());
    QQuickItem* ownBlur = inDialog(dialog, "attrVideoEffect0");
    REQUIRE(ownBlur);
    CHECK(ownBlur->property("text").toString() == QStringLiteral("Gaussian Blur"));
    REQUIRE(QMetaObject::invokeMethod(inDialog(dialog, "attributesOk"), "clicked"));
    REQUIRE(waitFor([&] { return !dialog->property("opened").toBool(); }, 5000));
    CHECK(controller->session().undoCount() == before + 2);
    CHECK(clipIn(*controller->snapshot(), tl.seq, "V1@10").effects().empty());

    // Paste Insert from the menu; with nothing selected the attribute items are off.
    controller->setPlayhead(f(0));
    openMenu(menu, lanes, clipKey(tl.clip("V1@10")));
    REQUIRE(QMetaObject::invokeMethod(menuItem(menu, "pasteInsertMenuItem"), "triggered"));
    QMetaObject::invokeMethod(menu, "close");
    CHECK(controller->session().undoCount() == before + 3);
    CHECK(controller->undoText() == QStringLiteral("Undo Paste Insert"));
    CHECK(controller->playhead() == f(10));
    controller->clearSelection();
    openMenu(menu, lanes, clipKey(tl.clip("V1@0")));
    CHECK(menuEnabled(menu, "pasteMenuItem"));
    CHECK_FALSE(menuEnabled(menu, "pasteAttributesMenuItem"));
    CHECK_FALSE(menuEnabled(menu, "removeAttributesMenuItem"));
    QMetaObject::invokeMethod(menu, "close");
    spin(100);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
