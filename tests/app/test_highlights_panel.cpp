// M5o (gaming): the Highlights panel (bridge `Highlights`) on the AI platform's G.2 highlights.importChat, find, toReel
// and toShort, through a stub catalog with those commands as the G.2 shapes (draft 3) describe them (G.2 is not on
// master yet): the find as a job off the GUI thread with progress and Cancel (its Marker::highlight markers one undo
// step), the moments read back from those markers (on open, a sequence switch, undo and redo) with reasons, signals,
// frames and waveforms, accept / reject / jump / preview, the reel (accepted moments in time order, with its options)
// and the Short on layout.apply (the chosen moment, its layout and facecam region), the chat log import, the
// unavailable state with the real catalog; in the window: the cards, the keyboard path, the options popovers, the
// accessibility audit and a screenshot. Offscreen; no window is shown.

#include "A11yAudit.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "HighlightsBridge.h"
#include "LayoutBridge.h"
#include "ProjectController.h"
#include "ShortsBridge.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocale>
#include <QPainter>
#include <QQmlComponent>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using command::Json;
using Catch::Approx;

namespace {

/// 30 s of gameplay (one character = one frame at 25 fps) on V1 and A1.
const std::string& gameplayPicture() {
    static const std::string picture = [] {
        const std::string body = "G" + std::string(749, 'g');
        return "V1 tgt |" + body + "|\nA1 tgt |" + body + "|\n";
    }();
    return picture;
}

Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

/// Within a frame of `s` (the playhead snaps to frames).
bool onFrameOf(Ticks t, double s) { return std::llabs(t.value - seconds(s).value) < f(1).value; }

/// What the stub commands last received, and where highlights.find ran.
std::mutex g_mutex;
Json g_lastFind;
Json g_lastReel;
Json g_lastShort;
Json g_lastImport;
std::atomic<bool> g_chatImported{false};
std::atomic<QThread*> g_findThread{nullptr};
std::atomic<int> g_findSteps{4};
std::atomic<int> g_stepMs{0};
/// 0.4 (Make Shorts' Cancel): the stub toShort's calls, and whether its second call waits at checkpoints until cancelled.
std::atomic<int> g_shortCalls{0};
std::atomic<bool> g_shortHold{false};

Json last(const Json& which) {
    std::lock_guard lock(g_mutex);
    return which;
}

struct StubMoment {
    int rank;
    double start, end, peak, score;
    const char* title;
    std::vector<std::string> reasons;
};

/// Six moments, best first; their times are not in rank order.
const std::vector<StubMoment>& stubMoments() {
    static const std::vector<StubMoment> moments{
        {1, 18.0, 22.0, 20.5, 0.91, "Triple kill + You shouted",
         {"you shouted (+11 dB, pitch +4 semitones)", "game audio +13 dB over the moments before",
          "chat 6x busier (38 messages in 5 s), 'KEKW' x14", "OBS chapter marker 'clutch'", "kill feed flashed"}},
        {2, 4.0, 7.0, 5.5, 0.82, "Clutch round", {"game audio +9 dB over the moments before", "you laughed"}},
        {3, 25.0, 28.0, 26.0, 0.66, "Boss down", {"the picture flashed", "game audio +7 dB"}},
        {4, 10.0, 13.0, 11.0, 0.48, "Close call", {"you shouted (+6 dB)"}},
        {5, 14.0, 16.0, 15.0, 0.31, "Chat went wild", {"chat 4x busier"}},
        {6, 1.0, 2.5, 1.5, 0.2, "Warm-up", {"game audio +3 dB"}},
    };
    return moments;
}

/// highlights.find, highlights.toReel and highlights.toShort as the G.2 shapes (draft 1) describe them: deterministic
/// moments, "Highlight <rank>: <title>" markers, new sequences for the reel and the short.
command::CommandRegistry stubCatalog() {
    command::CommandRegistry registry;

    command::CommandSpec find;
    find.name = "highlights.find";
    find.title = "Find Highlights";
    find.description = "Stub";
    find.capability = command::Capability::EditSequence;
    find.params = Json{{"type", "object"}};
    find.execute = [](command::ExecutionContext& c) -> command::CommandOutput {
        g_findThread = QThread::currentThread();
        const Json p = c.params();
        {
            std::lock_guard lock(g_mutex);
            g_lastFind = p;
        }
        const int steps = g_findSteps.load();
        for (int i = 0; i < steps; ++i) {
            c.checkpoint("measuring 'Gameplay.mkv' " + std::to_string(i * 100 / steps) + "%");
            if (g_stepMs.load() > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(g_stepMs.load()));
        }
        std::vector<std::string> wanted{"game", "mic", "visual", "chat", "markers"};
        if (p.contains("signals"))
            wanted = p.at("signals").get<std::vector<std::string>>();
        const auto has = [&](const char* s) { return std::find(wanted.begin(), wanted.end(), s) != wanted.end(); };
        const bool chat = g_chatImported.load() && has("chat"); // draft 2: the imported log counts on its own
        const int count = p.value("count", 10);
        const double minScore = p.value("minScore", 0.25);
        const SequenceId seq{p.at("sequence").get<std::uint64_t>()};

        // Draft 3: re-running find replaces only the markers whose highlight source is "highlights".
        std::vector<Marker> markers;
        for (const Marker& m : c.state().findSequence(seq)->markers) {
            if (!(m.highlight && m.highlight->source == "highlights"))
                markers.push_back(m);
        }
        Json moments = Json::array();
        int added = 0;
        for (const StubMoment& m : stubMoments()) {
            if (m.score < minScore || static_cast<int>(moments.size()) >= count)
                continue;
            Json sig = Json::object();
            if (has("game"))
                sig["game"] = m.score;
            if (has("mic"))
                sig["mic"] = m.score * 0.8;
            if (has("visual"))
                sig["visual"] = 0.34;
            if (chat)
                sig["chat"] = 0.7;
            if (has("markers") && m.rank == 1)
                sig["hint"] = 1.0;
            moments.push_back(Json{{"rank", m.rank}, {"start", m.start}, {"end", m.end}, {"peak", m.peak},
                                   {"thumbnail", m.peak}, {"duration", m.end - m.start},
                                   {"startTicks", seconds(m.start).value}, {"endTicks", seconds(m.end).value},
                                   {"peakTicks", seconds(m.peak).value}, {"score", m.score}, {"confidence", m.score},
                                   {"confidenceLabel", m.score >= 0.8 ? "high" : m.score >= 0.5 ? "medium" : "low"},
                                   {"title", m.title}, {"reasons", m.reasons}, {"signals", sig}});
            Marker marker;
            marker.start = seconds(m.start);
            marker.duration = seconds(m.end - m.start);
            marker.name = "Highlight " + std::to_string(m.rank) + ": " + m.title;
            marker.comment = "score " + std::to_string(m.score);
            marker.colorIndex = 3;
            MarkerHighlight h;
            h.score = m.score;
            h.confidence = m.score >= 0.8 ? HighlightConfidence::High
                           : m.score >= 0.5 ? HighlightConfidence::Medium
                                            : HighlightConfidence::Low;
            for (std::size_t i = 0; i < m.reasons.size() && i < MarkerHighlight::kMaxReasons; ++i)
                h.reasons.push_back(m.reasons[i]);
            h.peak = seconds(m.peak);
            marker.highlight = h;
            markers.push_back(std::move(marker));
            ++added;
        }
        command::CommandOutput out;
        out.state = c.take(edit::setSequenceMarkers(c.state(), seq, std::move(markers))).state;
        out.data = Json{{"timeBase", "timeline"},
                        {"sequence", seq.value},
                        {"moments", moments},
                        {"recordings", Json::array({Json{{"media", 1}, {"name", "Gameplay.mkv"},
                                                         {"audioRoles", Json::array({"game", "mic"})}, {"video", true},
                                                         {"markers", 0}, {"chat", chat}, {"fromCache", false}}})},
                        {"markersAdded", added}};
        if (chat)
            out.data["chat"] = Json{{"format", "twitch"}, {"messages", 18234}};
        return out;
    };
    REQUIRE(registry.add(std::move(find)));

    // Draft 2: the chat log is read by its own command (the one that reads a caller-named file), kept with the
    // recording; nothing in the project changes.
    command::CommandSpec importChat;
    importChat.name = "highlights.importChat";
    importChat.title = "Import Chat Log";
    importChat.description = "Stub";
    importChat.capability = command::Capability::MediaAnalysis;
    importChat.params = Json{{"type", "object"}};
    importChat = command::touchesFiles(std::move(importChat), {"file"});
    importChat.execute = [](command::ExecutionContext& c) -> command::CommandOutput {
        const Json p = c.params();
        {
            std::lock_guard lock(g_mutex);
            g_lastImport = p;
        }
        c.checkpoint("reading the chat log 50%");
        QFile file(QString::fromStdString(p.at("file").get<std::string>()));
        if (!file.open(QIODevice::ReadOnly) || !file.readAll().trimmed().startsWith('{'))
            command::fail(command::ErrorCode::InvalidParams, "not a chat log it knows");
        g_chatImported = true;
        const double offset = p.value("offset", 0.0);
        command::CommandOutput out;
        out.data = Json{{"media", 1},  {"name", "Gameplay.mkv"}, {"format", "twitch"}, {"messages", 18234},
                        {"first", 2.4}, {"last", 29.5},           {"offset", offset}};
        if (offset > 30)
            out.warnings.push_back("the chat does not overlap 'Gameplay.mkv': check \"offset\"");
        return out;
    };
    REQUIRE(registry.add(std::move(importChat)));

    command::CommandSpec reel;
    reel.name = "highlights.toReel";
    reel.title = "Make Highlight Reel";
    reel.description = "Stub";
    reel.capability = command::Capability::EditProject;
    reel.params = Json{{"type", "object"}};
    reel.execute = [](command::ExecutionContext& c) -> command::CommandOutput {
        const Json p = c.params();
        {
            std::lock_guard lock(g_mutex);
            g_lastReel = p;
        }
        const SequenceId source{p.at("sequence").get<std::uint64_t>()};
        const Sequence* seq = c.state().findSequence(source);
        if (!seq)
            command::fail(command::ErrorCode::NotFound, "no such sequence");
        edit::SequenceSettings settings;
        settings.name = p.value("name", seq->name + " - Highlights");
        auto created = edit::addSequence(c.state(), settings);
        if (!created)
            command::fail(command::ErrorCode::EditRejected, created.error().message);
        command::CommandOutput out;
        out.createdIds = {created->id.value};
        out.data = Json{{"source", source.value}, {"sequence", created->id.value}, {"name", settings.name},
                        {"duration", 12.0}, {"moments", Json::array()}, {"transitions", 2}};
        out.state = std::move(created->state);
        return out;
    };
    REQUIRE(registry.add(std::move(reel)));

    command::CommandSpec shortSpec;
    shortSpec.name = "highlights.toShort";
    shortSpec.title = "Make a Short";
    shortSpec.description = "Stub";
    shortSpec.capability = command::Capability::EditProject;
    shortSpec.params = Json{{"type", "object"}};
    shortSpec.execute = [](command::ExecutionContext& c) -> command::CommandOutput {
        const Json p = c.params();
        {
            std::lock_guard lock(g_mutex);
            g_lastShort = p;
        }
        if (p.contains("mode") || p.contains("fit"))
            command::fail(command::ErrorCode::InvalidParams, "mode and fit are gone (draft 3: layout.apply)");
        // 0.4: Make Shorts' Cancel test holds the second Short at its checkpoints until Cancel stops it (10 s at most).
        if (++g_shortCalls == 2 && g_shortHold.load()) {
            for (int i = 0; i < 5000; ++i) {
                c.checkpoint("framing the short 50%");
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        const bool facecam = p.contains("facecam") || p.contains("facecamRegion");
        std::string layout = p.value("layout", std::string("auto"));
        if (layout == "auto")
            layout = facecam ? "verticalSplit" : "verticalBlurFill";
        const Sequence* source = c.state().findSequence(SequenceId{p.at("sequence").get<std::uint64_t>()});
        const std::uint64_t gameplay = source && !source->videoTracks.empty() && !source->videoTracks.front()->clips.empty()
                                           ? source->videoTracks.front()->clips.front().id.value
                                           : 0;
        edit::SequenceSettings settings;
        settings.name = p.value("title", std::string("Highlight")) + " (Short)";
        settings.width = 1080;
        settings.height = 1920;
        auto created = edit::addSequence(c.state(), settings);
        if (!created)
            command::fail(command::ErrorCode::EditRejected, created.error().message);
        command::CommandOutput out;
        out.createdIds = {created->id.value};
        out.data = Json{{"source", p.at("sequence")}, {"sequence", created->id.value}, {"start", p.value("start", 0.0)},
                        {"end", p.value("end", 0.0)}, {"width", 1080}, {"height", 1920}, {"layout", layout},
                        {"gameplay", gameplay}, {"facecam", p.contains("facecam") ? p.at("facecam") : Json(nullptr)},
                        {"captions", p.value("captions", true)}, {"title", p.value("title", std::string())},
                        {"normalized", true}};
        out.state = std::move(created->state);
        return out;
    };
    REQUIRE(registry.add(std::move(shortSpec)));
    return registry;
}

const command::CommandRegistry& catalog() {
    static const command::CommandRegistry registry = stubCatalog();
    return registry;
}

/// The bridge over a plain controller (no window).
struct Rig {
    Timeline tl{gameplayPicture()};
    ProjectController controller;
    HighlightsBridge hl;
    std::vector<bool> transport; ///< preview's play (true) / pause (false)

    explicit Rig(bool synchronous = true, const std::string& picture = gameplayPicture(),
                 const std::map<char, std::int64_t>& mediaFrames = {})
        : tl(picture, mediaFrames) {
        g_findSteps = 4;
        g_stepMs = 0;
        g_chatImported = false;
        g_shortCalls = 0;
        g_shortHold = false;
        controller.resetProject(tl.state, tl.seq);
        hl.setRegistry(&catalog());
        hl.setSynchronous(synchronous);
        hl.setController(&controller);
        hl.setTransport([this](bool play) { transport.push_back(play); });
    }
    std::size_t undo() { return controller.session().undoCount(); }
    std::vector<int> ranks() const {
        std::vector<int> out;
        for (const QVariant& m : hl.moments())
            out.push_back(m.toMap().value(QStringLiteral("rank")).toInt());
        return out;
    }
    QVariantMap moment(int rank) const {
        for (const QVariant& m : hl.moments()) {
            if (m.toMap().value(QStringLiteral("rank")).toInt() == rank)
                return m.toMap();
        }
        return {};
    }
    std::size_t highlightMarkers() const {
        const ProjectSnapshot snap = controller.snapshot();
        const Sequence* s = snap->findSequence(tl.seq);
        return static_cast<std::size_t>(std::count_if(s->markers.begin(), s->markers.end(), [](const Marker& m) {
            return m.name.starts_with("Highlight ");
        }));
    }
};

QVariantMap opts(std::initializer_list<std::pair<const QString, QVariant>> values) { return QVariantMap(values); }

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

void collectNamed(QQuickItem* root, const QString& name, QList<QQuickItem*>& out) {
    if (!root)
        return;
    if (root->objectName() == name)
        out.append(root);
    for (QQuickItem* child : root->childItems())
        collectNamed(child, name, out);
}

std::unique_ptr<QQuickItem> makeItem(Shell& shell, const QByteArray& qml, QRectF at) {
    QQmlComponent component(&shell.engine);
    component.setData(qml, QUrl(QStringLiteral("qrc:/m5o-highlights/Host.qml")));
    std::unique_ptr<QObject> object(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    auto* item = qobject_cast<QQuickItem*>(object.get());
    REQUIRE(item);
    object.release();
    item->setParentItem(shell.window->contentItem());
    item->setPosition(at.topLeft());
    item->setSize(at.size());
    item->setZ(1000);
    return std::unique_ptr<QQuickItem>(item);
}

/// Lays out `root` and everything under it now, parents first (a window that is never shown only polishes when a frame
/// is rendered; the offscreen grab is kept for the screenshot): layouts size their children, lists make their cards.
void polishAll(QQuickItem* root) {
    if (!root)
        return;
    root->ensurePolished();
    for (QQuickItem* child : root->childItems())
        polishAll(child);
}

QRectF sceneRect(const QQuickItem* item) { return item->mapRectToScene(QRectF(0, 0, item->width(), item->height())); }

/// A frame of the gameplay for the cards: a colour per second, a bright band where the action is.
QImage testFrame(const MediaItem&, Ticks time) {
    QImage image(320, 180, QImage::Format_RGB888);
    const int hue = static_cast<int>(time.toSeconds() * 13.0) % 360;
    image.fill(QColor::fromHsv(hue, 150, 90));
    QPainter p(&image);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor::fromHsv((hue + 180) % 360, 200, 230));
    p.drawEllipse(QPointF(160 + 60 * std::sin(time.toSeconds()), 90), 46, 46);
    p.setBrush(QColor(20, 20, 24));
    p.drawRect(0, 150, 320, 30);
    return image;
}

} // namespace

TEST_CASE("Highlights: find runs off the GUI thread with progress, Cancel stops it, an edit meanwhile keeps the markers out",
          "[app][highlights]") {
    Rig r(false);
    CHECK(r.hl.available());
    g_findSteps = 20;
    g_stepMs = 10;

    // A run: the GUI thread keeps turning while the worker measures; the markers are one undo step.
    bool sawProgress = false;
    double lastProgress = -1;
    QObject::connect(&r.hl, &HighlightsBridge::jobChanged, &r.hl, [&] {
        if (r.hl.busy() && r.hl.progress() > 0) {
            sawProgress = true;
            lastProgress = r.hl.progress();
            CHECK(r.hl.activity() == QStringLiteral("Measuring 'Gameplay.mkv'"));
            CHECK(r.hl.job() == QStringLiteral("find"));
        }
    });
    int ticks = 0;
    QTimer ticker;
    ticker.setInterval(5);
    QObject::connect(&ticker, &QTimer::timeout, [&] { ++ticks; });
    ticker.start();
    const std::size_t before = r.undo();
    REQUIRE(r.hl.find({}).isEmpty());
    CHECK(r.hl.busy());
    CHECK(r.hl.moments().isEmpty()); // not inside the call
    REQUIRE(waitFor([&] { return !r.hl.busy(); }, 10000));
    INFO(r.hl.message().toStdString());
    CHECK(g_findThread.load() != QCoreApplication::instance()->thread());
    CHECK(ticks > 3);
    CHECK(sawProgress);
    CHECK(lastProgress < 1.0);
    CHECK(r.hl.searched());
    CHECK(r.ranks() == std::vector<int>{1, 2, 3, 4, 5}); // minScore 0.25 leaves out the sixth
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.session().undoLabel().value_or("") == "Find Highlights");
    CHECK(r.highlightMarkers() == 5);
    CHECK(r.hl.job().isEmpty());

    // Cancel: stopped at its next checkpoint, nothing committed, the earlier moments kept.
    g_findSteps = 500;
    sawProgress = false;
    REQUIRE(r.hl.find({}).isEmpty());
    REQUIRE(waitFor([&] { return sawProgress; }, 5000));
    r.hl.cancel();
    REQUIRE(waitFor([&] { return !r.hl.busy(); }, 10000));
    CHECK(r.hl.message().contains(QStringLiteral("Stopped")));
    CHECK(r.undo() == before + 1);
    CHECK(r.highlightMarkers() == 5);
    CHECK(r.ranks().size() == 5);

    // An edit while it runs: the moments come back, their markers are not committed over the edit.
    g_findSteps = 30;
    REQUIRE(r.hl.find(opts({{QStringLiteral("count"), 2}})).isEmpty());
    r.controller.setSelection({r.tl.clip("V1@0")});
    r.controller.deleteSelection();
    const std::size_t afterEdit = r.undo();
    REQUIRE(waitFor([&] { return !r.hl.busy(); }, 10000));
    CHECK(r.ranks() == std::vector<int>{1, 2, 3, 4, 5}); // the list is the markers: the earlier ones
    CHECK(r.undo() == afterEdit);
    CHECK(r.hl.message().contains(QStringLiteral("markers were not added")));

    // One job at a time.
    g_findSteps = 50;
    REQUIRE(r.hl.find({}).isEmpty());
    CHECK(r.hl.find({}) == QStringLiteral("Busy"));
    r.hl.cancel();
    REQUIRE(waitFor([&] { return !r.hl.busy(); }, 10000));
}

TEST_CASE("Highlights: options become find parameters; moments ranked with reasons, signals, frames; accept, reject, jump, "
          "preview",
          "[app][highlights]") {
    Rig r;
    int reveals = 0;
    QObject::connect(&r.hl, &HighlightsBridge::revealRequested, &r.hl, [&] { ++reveals; });

    // The options: the signals the command knows, counts and scores clamped to its ranges.
    QVariantMap p = r.hl.findParams(opts({{QStringLiteral("signals"), QStringList{"game", "mic", "chat", "markers", "nope"}},
                                          {QStringLiteral("count"), 500},
                                          {QStringLiteral("minScore"), 1.5}}));
    CHECK(p.value(QStringLiteral("sequence")).toULongLong() == r.tl.seq.value);
    CHECK(p.value(QStringLiteral("signals")).toStringList() == QStringList{"game", "mic", "chat", "markers"});
    CHECK(p.value(QStringLiteral("count")).toInt() == 200);
    CHECK(p.value(QStringLiteral("minScore")).toDouble() == Approx(1.0));
    CHECK_FALSE(p.contains(QStringLiteral("chatFile"))); // draft 2: highlights.importChat reads the log
    CHECK_FALSE(p.contains(QStringLiteral("chatOffset")));
    // No signal at all: refused before anything runs.
    CHECK_FALSE(r.hl.find(opts({{QStringLiteral("signals"), QStringList{}}})).isEmpty());
    CHECK_FALSE(r.hl.searched());

    // A find: what the command got, the moments ranked.
    REQUIRE(r.hl.find(opts({{QStringLiteral("signals"), QStringList{"game", "mic", "markers"}},
                            {QStringLiteral("count"), 4},
                            {QStringLiteral("minScore"), 0.3}}))
                .isEmpty());
    const Json sent = last(g_lastFind);
    INFO(sent.dump());
    CHECK(sent["signals"] == Json::array({"game", "mic", "markers"}));
    CHECK(sent["count"] == 4);
    CHECK(sent["minScore"].get<double>() == Approx(0.3));
    CHECK(sent["sequence"] == r.tl.seq.value);
    CHECK_FALSE(sent.contains("chatFile"));
    CHECK(r.ranks() == std::vector<int>{1, 2, 3, 4});
    CHECK(r.hl.selectedRank() == 1);
    CHECK(r.hl.moments().size() == 4);
    CHECK(r.hl.recordingCount() == 1);
    CHECK(r.hl.chatMessages() == 0);
    REQUIRE(r.hl.recordings().size() == 1);
    CHECK(r.hl.recordings().front().toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Gameplay.mkv"));

    const QVariantMap best = r.moment(1);
    CHECK(best.value(QStringLiteral("title")).toString() == QStringLiteral("Triple kill + You shouted"));
    CHECK(best.value(QStringLiteral("reasons")).toStringList().size() == 5);
    CHECK(best.value(QStringLiteral("reasons")).toStringList().front().startsWith(QStringLiteral("you shouted")));
    CHECK(best.value(QStringLiteral("score")).toDouble() == Approx(0.91));
    CHECK(best.value(QStringLiteral("confidenceLabel")).toString() == QStringLiteral("high"));
    CHECK(best.value(QStringLiteral("rangeText")).toString() == QStringLiteral("0:18.0 – 0:22.0"));
    CHECK(best.value(QStringLiteral("durationText")).toString() == QStringLiteral("4.0 s"));
    // The measured signals only, in the cards' order (no picture or chat here; the stream marker hit).
    QStringList keys;
    for (const QVariant& s : best.value(QStringLiteral("signals")).toList())
        keys << s.toMap().value(QStringLiteral("key")).toString();
    CHECK(keys == QStringList{"game", "mic", "hint"});
    CHECK(best.value(QStringLiteral("signals")).toList().front().toMap().value(QStringLiteral("label")).toString() ==
          QStringLiteral("Game audio"));
    // The frame at the thumbnail time (the gameplay's media and source time) and a waveform of the range.
    const QString image = best.value(QStringLiteral("image")).toString();
    CHECK(image == QStringLiteral("image://wabcut-frames/%1/%2").arg(mediaKey(r.tl.media('G'))).arg(seconds(20.5).value));
    CHECK(best.value(QStringLiteral("waveform")).toList().size() == 32);

    // Review: accepted ones stay in place; rejected ones go last; again undoes.
    r.hl.accept(2);
    r.hl.accept(4);
    CHECK(r.hl.acceptedCount() == 2);
    CHECK(r.moment(2).value(QStringLiteral("state")).toString() == QStringLiteral("accepted"));
    CHECK(r.ranks() == std::vector<int>{1, 2, 3, 4});
    r.hl.reject(3);
    CHECK(r.ranks() == std::vector<int>{1, 2, 4, 3});
    CHECK(r.moment(3).value(QStringLiteral("state")).toString() == QStringLiteral("rejected"));
    r.hl.reject(1);
    CHECK(r.ranks() == std::vector<int>{2, 4, 1, 3});
    r.hl.reject(1);
    r.hl.accept(4);
    CHECK(r.ranks() == std::vector<int>{1, 2, 4, 3});
    CHECK(r.hl.acceptedCount() == 1);

    // Jump: the playhead to the peak, revealed, the moment chosen; from another sequence it goes back to the moments'.
    CHECK(r.hl.jump(2).isEmpty());
    CHECK(onFrameOf(r.controller.playhead(), 5.5));
    CHECK(r.hl.selectedRank() == 2);
    CHECK(reveals == 1);
    r.controller.newSequence();
    REQUIRE(r.controller.currentSequenceId() != r.tl.seq);
    CHECK(r.hl.jump(4).isEmpty());
    CHECK(r.controller.currentSequenceId() == r.tl.seq);
    CHECK(onFrameOf(r.controller.playhead(), 11.0));
    CHECK_FALSE(r.hl.jump(42).isEmpty());

    // Preview: from the start, played; paused at the end; again stops it.
    CHECK(r.hl.preview(2).isEmpty());
    CHECK(onFrameOf(r.controller.playhead(), 4.0));
    CHECK(r.hl.previewRank() == 2);
    CHECK(r.transport == std::vector<bool>{true});
    r.controller.setPlayhead(seconds(6.0));
    CHECK(r.hl.previewRank() == 2);
    r.controller.setPlayhead(seconds(7.04));
    CHECK(r.hl.previewRank() == 0);
    CHECK(r.transport == std::vector<bool>{true, false});
    CHECK(r.hl.preview(1).isEmpty());
    CHECK(r.hl.preview(1).isEmpty());
    CHECK(r.hl.previewRank() == 0);
    CHECK(r.transport == std::vector<bool>{true, false, true, false});
    // Moved away by hand: no longer a preview (nothing paused).
    CHECK(r.hl.preview(3).isEmpty());
    r.controller.setPlayhead(seconds(1.0));
    CHECK(r.hl.previewRank() == 0);
    CHECK(r.transport == std::vector<bool>{true, false, true, false, true});

    // The list is the highlight markers (Marker::highlight): undo takes them away, redo brings them back (with the
    // find's signals); the review is kept by the markers' ranges.
    REQUIRE(r.controller.currentSequenceId() == r.tl.seq);
    REQUIRE(r.hl.acceptedCount() == 1); // rank 2
    int later = 0; // the steps after the find (the new sequence above)
    for (; later < 4 && r.controller.session().undoLabel().value_or("") != "Find Highlights"; ++later)
        r.controller.undo();
    REQUIRE(r.controller.session().undoLabel().value_or("") == "Find Highlights");
    REQUIRE(r.controller.currentSequenceId() == r.tl.seq);
    CHECK(r.ranks() == std::vector<int>{1, 2, 4, 3});
    r.controller.undo();
    CHECK(r.hl.moments().isEmpty());
    r.controller.redo();
    CHECK(r.ranks() == std::vector<int>{1, 2, 4, 3}); // 3 still rejected, last
    CHECK(r.hl.acceptedCount() == 1);
    CHECK(r.moment(2).value(QStringLiteral("state")).toString() == QStringLiteral("accepted"));
    CHECK(r.moment(1).value(QStringLiteral("signals")).toList().size() == 3);
    for (int i = 0; i < later; ++i)
        r.controller.redo();
    if (r.controller.currentSequenceId() != r.tl.seq)
        r.controller.setCurrentSequenceIndex(0);
    REQUIRE(r.controller.currentSequenceId() == r.tl.seq);
    // Another bridge (the panel on open): the same moments from the markers, without what only the find measured.
    {
        HighlightsBridge fresh;
        fresh.setRegistry(&catalog());
        fresh.setController(&r.controller);
        REQUIRE(fresh.moments().size() == 4);
        const QVariantMap first = fresh.moments().front().toMap();
        CHECK(first.value(QStringLiteral("rank")).toInt() == 1);
        CHECK(first.value(QStringLiteral("title")).toString() == QStringLiteral("Triple kill + You shouted"));
        CHECK(first.value(QStringLiteral("score")).toDouble() == Approx(0.91));
        CHECK(first.value(QStringLiteral("confidenceLabel")).toString() == QStringLiteral("high"));
        CHECK(first.value(QStringLiteral("peak")).toDouble() == Approx(20.5));
        CHECK(first.value(QStringLiteral("reasons")).toStringList().size() == 5);
        CHECK(first.value(QStringLiteral("signals")).toList().isEmpty());
        CHECK_FALSE(fresh.searched());
        CHECK(fresh.selectedRank() == 1);
    }
    // A sequence switch: a new sequence without highlights keeps the list (otherSequence); a find there lists its own;
    // back on the first, its moments again.
    const QString firstName = r.hl.sequenceName();
    r.controller.newSequence();
    CHECK(r.ranks() == std::vector<int>{1, 2, 4, 3});
    CHECK(r.hl.otherSequence());
    CHECK(r.hl.sequenceName() == firstName);
    REQUIRE(r.hl.find(opts({{QStringLiteral("count"), 2}})).isEmpty());
    CHECK(r.ranks() == std::vector<int>{1, 2});
    CHECK_FALSE(r.hl.otherSequence());
    CHECK(r.hl.acceptedCount() == 0);
    r.controller.setCurrentSequenceIndex(0);
    REQUIRE(r.controller.currentSequenceId() == r.tl.seq);
    CHECK(r.ranks() == std::vector<int>{1, 2, 4, 3});
    CHECK(r.hl.acceptedCount() == 1);
}

TEST_CASE("Highlights: the reel gets the accepted moments in time order with its options; the short the chosen moment",
          "[app][highlights]") {
    Rig r;
    QString opened;
    QObject::connect(&r.hl, &HighlightsBridge::sequenceOpened, &r.hl, [&](const QString& name) { opened = name; });
    // Nothing found yet: nothing to make.
    CHECK_FALSE(r.hl.makeReel({}).isEmpty());
    CHECK_FALSE(r.hl.makeShort(1, {}).isEmpty());

    REQUIRE(r.hl.find({}).isEmpty());
    REQUIRE(r.ranks() == std::vector<int>{1, 2, 3, 4, 5});
    // None accepted: every moment not rejected, in time order.
    r.hl.reject(5);
    std::vector<double> starts;
    for (const QVariant& m : r.hl.reelMoments())
        starts.push_back(m.toMap().value(QStringLiteral("start")).toDouble());
    CHECK(starts == std::vector<double>{4.0, 10.0, 18.0, 25.0});
    CHECK(r.hl.reelCount() == 4);

    // Accepted ones only, in time order, with the reel's options.
    r.hl.accept(1);
    r.hl.accept(2);
    r.hl.accept(4);
    CHECK(r.hl.reelCount() == 3);
    const QString music = mediaKey(r.tl.media('G'));
    const std::size_t before = r.undo();
    REQUIRE(r.hl.makeReel(opts({{QStringLiteral("order"), QStringLiteral("time")},
                                {QStringLiteral("maxDuration"), 60},
                                {QStringLiteral("transition"), QStringLiteral("dipToBlack")},
                                {QStringLiteral("music"), music},
                                {QStringLiteral("slowMotion"), true}}))
                .isEmpty());
    const Json reel = last(g_lastReel);
    INFO(reel.dump());
    CHECK(reel["sequence"] == r.tl.seq.value);
    REQUIRE(reel["moments"].size() == 3);
    CHECK(reel["moments"][0]["start"].get<double>() == Approx(4.0));
    CHECK(reel["moments"][1]["start"].get<double>() == Approx(10.0));
    CHECK(reel["moments"][2]["start"].get<double>() == Approx(18.0));
    CHECK(reel["moments"][2]["end"].get<double>() == Approx(22.0));
    CHECK(reel["moments"][2]["peak"].get<double>() == Approx(20.5));
    CHECK(reel["moments"][2]["title"] == "Triple kill + You shouted");
    CHECK(reel["order"] == "time");
    CHECK(reel["maxDuration"].get<double>() == Approx(60));
    CHECK(reel["transition"] == "dipToBlack");
    CHECK(reel["music"] == r.tl.media('G').value);
    CHECK(reel["slowMotion"] == true);
    // One undo step; the new sequence is open.
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.currentSequenceId() != r.tl.seq);
    CHECK(r.controller.sequenceName().endsWith(QStringLiteral(" - Highlights")));
    CHECK(opened == r.controller.sequenceName());
    CHECK(r.hl.message().startsWith(QStringLiteral("Made")));
    // Unknown options are left to the command's defaults.
    const QVariantMap defaults = r.hl.reelParams(opts({{QStringLiteral("transition"), QStringLiteral("spin")},
                                                       {QStringLiteral("music"), QString()}}));
    CHECK_FALSE(defaults.contains(QStringLiteral("transition")));
    CHECK_FALSE(defaults.contains(QStringLiteral("music")));
    CHECK(defaults.value(QStringLiteral("order")).toString() == QStringLiteral("time"));

    // A Short of the chosen moment (rank 0 = the chosen one), with its options; the moments' sequence is the source.
    r.hl.setSelectedRank(4);
    // The reel is open: the list stays on the moments' sequence.
    CHECK(r.hl.otherSequence());
    CHECK(r.ranks() == std::vector<int>{1, 2, 3, 4, 5});
    REQUIRE(r.hl.makeShort(0, opts({{QStringLiteral("layout"), QStringLiteral("verticalSplit")},
                                    {QStringLiteral("split"), 0.9},
                                    {QStringLiteral("facecamOnTop"), true},
                                    {QStringLiteral("facecamRegion"), QVariantMap{{QStringLiteral("x"), 0.7},
                                                                                  {QStringLiteral("y"), 0.7},
                                                                                  {QStringLiteral("width"), 0.5},
                                                                                  {QStringLiteral("height"), 0.25}}},
                                    {QStringLiteral("mode"), QStringLiteral("faces")},
                                    {QStringLiteral("captions"), false}}))
                .isEmpty());
    Json shortSent = last(g_lastShort);
    INFO(shortSent.dump());
    CHECK(shortSent["sequence"] == r.tl.seq.value);
    CHECK(shortSent["start"].get<double>() == Approx(10.0));
    CHECK(shortSent["end"].get<double>() == Approx(13.0));
    CHECK(shortSent["title"] == "Close call");
    CHECK(shortSent["layout"] == "verticalSplit");
    CHECK(shortSent["split"].get<double>() == Approx(0.8)); // layout.apply's range
    CHECK(shortSent["facecamOnTop"] == true);
    CHECK(shortSent["facecamRegion"]["x"].get<double>() == Approx(0.7));
    CHECK(shortSent["facecamRegion"]["width"].get<double>() == Approx(0.3)); // inside the frame
    CHECK(shortSent["captions"] == false);
    CHECK_FALSE(shortSent.contains("mode")); // draft 3: gone
    CHECK_FALSE(shortSent.contains("fit"));
    CHECK_FALSE(shortSent.contains("hookTitle"));
    CHECK(r.undo() == before + 2);
    CHECK(r.controller.sequenceName() == QStringLiteral("Close call (Short)"));
    CHECK(r.controller.currentSequence()->width == 1080);
    CHECK(r.controller.currentSequence()->height == 1920);
    // The result's layout is said.
    CHECK(r.hl.message() == QStringLiteral("Made 'Close call (Short)': 1080 × 1920, gameplay and facecam split"));
    // An explicit rank, the defaults: auto (no facecam here: the blurred fill).
    REQUIRE(r.hl.makeShort(2, {}).isEmpty());
    shortSent = last(g_lastShort);
    CHECK(shortSent["start"].get<double>() == Approx(4.0));
    CHECK(shortSent["title"] == "Clutch round");
    CHECK_FALSE(shortSent.contains("layout"));
    CHECK(r.hl.message().endsWith(QStringLiteral("gameplay over a blurred fill")));
    // Out of range: zoom and the facecam clip ("c12").
    const QVariantMap sp = r.hl.shortParams(1, opts({{QStringLiteral("zoom"), 9.0},
                                                     {QStringLiteral("facecam"), clipKey(r.tl.clip("V1@0"))},
                                                     {QStringLiteral("layout"), QStringLiteral("pip")}}));
    CHECK(sp.value(QStringLiteral("zoom")).toDouble() == Approx(4.0));
    CHECK(sp.value(QStringLiteral("facecam")).toULongLong() == r.tl.clip("V1@0").value);
    CHECK_FALSE(sp.contains(QStringLiteral("layout")));
    CHECK_FALSE(r.hl.makeShort(42, {}).isEmpty());

    // The project's audio-only media are the reel's music choices (the gameplay is not one).
    for (const QVariant& m : r.hl.musicChoices())
        CHECK(m.toMap().value(QStringLiteral("id")).toString() != music);
}

TEST_CASE("Highlights: a chat log is imported first (no undo step, its format and size shown), then counts in the find",
          "[app][highlights]") {
    Rig r;
    CHECK(r.hl.chatAvailable());
    CHECK(r.hl.chatSummary().isEmpty());
    QTemporaryDir dir;
    const QString chatPath = QDir(dir.path()).filePath(QStringLiteral("chat.json"));
    const QString badPath = QDir(dir.path()).filePath(QStringLiteral("notes.txt"));
    {
        QFile f(chatPath);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("{\"comments\": []}");
        QFile bad(badPath);
        REQUIRE(bad.open(QIODevice::WriteOnly));
        bad.write("not json");
    }

    // From a file dialog's URL: the absolute path, the current sequence, the offset; nothing in the project changes.
    const std::size_t before = r.undo();
    REQUIRE(r.hl.importChat(QUrl::fromLocalFile(chatPath).toString()).isEmpty());
    const Json imported = last(g_lastImport);
    INFO(imported.dump());
    CHECK(QFileInfo(QString::fromStdString(imported["file"].get<std::string>())) == QFileInfo(chatPath));
    CHECK(imported["sequence"] == r.tl.seq.value);
    CHECK(imported["offset"].get<double>() == Approx(0.0));
    CHECK(r.undo() == before);
    CHECK(r.hl.chatFileName() == QStringLiteral("chat.json"));
    CHECK(r.hl.chat().value(QStringLiteral("format")).toString() == QStringLiteral("twitch"));
    CHECK(r.hl.chat().value(QStringLiteral("messages")).toInt() == 18234);
    CHECK(r.hl.chatSummary() ==
          QStringLiteral("Twitch chat · Messages: %1 · Gameplay.mkv").arg(QLocale().toString(qlonglong(18234))));
    CHECK(r.hl.message().startsWith(QStringLiteral("Chat log read: Twitch chat")));

    // The find does not send the log; a changed offset overrides the import's.
    QVariantMap p = r.hl.findParams({});
    CHECK_FALSE(p.contains(QStringLiteral("chatOffset")));
    r.hl.setChatOffset(2.5);
    p = r.hl.findParams({});
    CHECK(p.value(QStringLiteral("chatOffset")).toDouble() == Approx(2.5));
    REQUIRE(r.hl.find({}).isEmpty());
    const Json sent = last(g_lastFind);
    INFO(sent.dump());
    CHECK_FALSE(sent.contains("chatFile"));
    CHECK(sent["chatOffset"].get<double>() == Approx(2.5));
    CHECK(r.hl.chatMessages() == 18234);
    QStringList keys;
    for (const QVariant& s : r.moment(1).value(QStringLiteral("signals")).toList())
        keys << s.toMap().value(QStringLiteral("key")).toString();
    CHECK(keys == QStringList{"game", "mic", "visual", "chat", "hint"});

    // An offset far off: the import's warning is shown; a file it cannot read: its error, the last log kept.
    r.hl.setChatOffset(45);
    REQUIRE(r.hl.importChat(chatPath).isEmpty());
    CHECK(r.hl.message().contains(QStringLiteral("check \"offset\"")));
    CHECK(r.hl.chat().value(QStringLiteral("offset")).toDouble() == Approx(45));
    CHECK_FALSE(r.hl.findParams({}).contains(QStringLiteral("chatOffset")));
    REQUIRE(r.hl.importChat(badPath).isEmpty());
    CHECK(r.hl.message().contains(QStringLiteral("not a chat log")));
    CHECK(r.hl.chatFileName() == QStringLiteral("chat.json"));
    CHECK_FALSE(r.hl.importChat(QString()).isEmpty());
}

TEST_CASE("Highlights: unavailable with the real catalog until G.2 lands", "[app][highlights]") {
    Timeline tl{gameplayPicture()};
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    HighlightsBridge hl;
    hl.setSynchronous(true);
    hl.setController(&controller);
    const command::CommandRegistry& real = ai::edit::editingCommands();
    CHECK(hl.available() == real.knows("highlights.find"));
    CHECK(hl.reelAvailable() == real.knows("highlights.toReel"));
    CHECK(hl.shortAvailable() == real.knows("highlights.toShort"));
    CHECK(hl.chatAvailable() == real.knows("highlights.importChat"));
    if (!real.knows("highlights.find")) {
        const std::size_t before = controller.session().undoCount();
        CHECK(hl.find({}) == QStringLiteral("Highlights arrive with the next engine update"));
        CHECK_FALSE(hl.busy());
        CHECK_FALSE(hl.searched());
        CHECK(controller.session().undoCount() == before);
    }
    // The stub catalog makes it available; back to the real one, unavailable again.
    hl.setRegistry(&catalog());
    CHECK(hl.available());
    hl.setRegistry(nullptr);
    CHECK(hl.available() == real.knows("highlights.find"));
}

TEST_CASE("Highlights: the panel's find, reel and Short parameters pass G.2's own schemas (dry runs on the real catalog)",
          "[app][highlights]") {
    const command::CommandRegistry& real = ai::edit::editingCommands();
    if (!real.knows("highlights.find"))
        return; // G.2 not in this build
    Timeline tl{gameplayPicture()};
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    HighlightsBridge hl;
    hl.setSynchronous(true);
    hl.setController(&controller);
    const auto toJson = [](const QVariantMap& map) {
        return command::Json::parse(QJsonDocument::fromVariant(map).toJson(QJsonDocument::Compact).toStdString());
    };
    const auto check = [&](const char* name, const QVariantMap& params) {
        command::CommandEngine engine(controller.session(), real, command::EnginePolicy{});
        command::CommandRequest request;
        request.command = name;
        request.params = toJson(params);
        request.dryRun = true;
        const command::CommandResult result = engine.execute(request);
        INFO(name << " " << request.params.dump() << " -> " << (result.ok() ? std::string("ok") : result.error->message));
        // The media is not real, so the work itself may fail; the parameters must not.
        CHECK((result.ok() || result.error->code != command::ErrorCode::InvalidParams));
    };
    check("highlights.find", hl.findParams(QVariantMap{{QStringLiteral("count"), 5}, {QStringLiteral("minScore"), 0.4},
                                                       {QStringLiteral("signals"), QStringList{QStringLiteral("game"), QStringLiteral("mic")}}}));
    check("highlights.toReel", hl.reelParams(QVariantMap{{QStringLiteral("order"), QStringLiteral("time")},
                                                         {QStringLiteral("transition"), QStringLiteral("cut")}}));
    check("highlights.toShort", hl.shortParams(1, QVariantMap{{QStringLiteral("layout"), QStringLiteral("verticalBlurFill")},
                                                              {QStringLiteral("zoom"), 1.2}}));
    check("highlights.toShort", hl.shortParams(1, QVariantMap{{QStringLiteral("layout"), QStringLiteral("verticalSplit")},
                                                              {QStringLiteral("split"), 0.6},
                                                              {QStringLiteral("facecamOnTop"), true}}));
    // 0.4: the lead-in, tail and caption style the Short options send (a moment's range given, as shortParams makes it).
    check("highlights.toShort", QVariantMap{{QStringLiteral("sequence"), static_cast<qulonglong>(tl.seq.value)},
                                            {QStringLiteral("start"), 18.0},
                                            {QStringLiteral("end"), 22.0},
                                            {QStringLiteral("title"), QStringLiteral("Triple kill")},
                                            {QStringLiteral("handleBefore"), 5.0},
                                            {QStringLiteral("handleAfter"), 2.0},
                                            {QStringLiteral("captions"), true},
                                            {QStringLiteral("captionPreset"), QStringLiteral("Shorts")}});
}

// ---- 0.4: Make Shorts (every accepted moment), Export all, and the Short options' lead-in, tail and minimum length ----

TEST_CASE("Highlights: Make Shorts makes one Short per accepted moment in one undo step and stays on the source",
          "[app][highlights]") {
    Rig r;
    REQUIRE(r.hl.find({}).isEmpty());
    r.hl.accept(3);
    r.hl.accept(1);
    CHECK(r.hl.shortsCount() == 2);
    CHECK(r.hl.madeShortsCount() == 0);
    const qsizetype sequences = r.controller.sequenceNames().size();
    const int current = r.controller.currentSequenceIndex();
    const std::size_t before = r.undo();
    QStringList activities;
    QStringList jobs;
    QObject::connect(&r.hl, &HighlightsBridge::jobChanged, &r.hl, [&] {
        activities << r.hl.activity();
        jobs << r.hl.job();
    });
    int madeSignals = 0;
    QObject::connect(&r.hl, &HighlightsBridge::madeShortsChanged, &r.hl, [&] { ++madeSignals; });

    REQUIRE(r.hl.makeShorts(opts({{QStringLiteral("layout"), QStringLiteral("auto")}})).isEmpty());
    INFO(r.hl.message().toStdString());
    CHECK(activities.contains(QStringLiteral("Short 1 of 2")));
    CHECK(activities.contains(QStringLiteral("Short 2 of 2")));
    CHECK(jobs.contains(QStringLiteral("shorts")));
    CHECK(r.hl.job().isEmpty());
    // Two vertical sequences, one undo step; the timeline stays on the source and the moments stay listed.
    CHECK(r.controller.sequenceNames().size() == sequences + 2);
    CHECK(r.controller.currentSequenceIndex() == current);
    CHECK(r.controller.currentSequenceId() == r.tl.seq);
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.session().undoLabel().value_or("") == "Make Highlight Shorts");
    CHECK(r.ranks() == std::vector<int>{1, 2, 3, 4, 5});
    CHECK(r.hl.madeShortsCount() == 2);
    CHECK(madeSignals > 0);
    // In time order (rank 1 at 18 s, rank 3 at 25 s); the message names them.
    CHECK(r.controller.sequenceNames().contains(QStringLiteral("Triple kill + You shouted (Short)")));
    CHECK(r.controller.sequenceNames().contains(QStringLiteral("Boss down (Short)")));
    CHECK(r.hl.message().contains(QStringLiteral("Triple kill + You shouted (Short), Boss down (Short)")));
    for (const SequencePtr& s : r.controller.snapshot()->sequences) {
        if (s->id != r.tl.seq) {
            CHECK(s->width == 1080);
            CHECK(s->height == 1920);
        }
    }
    // One undo takes both away.
    REQUIRE(r.controller.undo());
    CHECK(r.controller.sequenceNames().size() == sequences);
    CHECK(r.hl.madeShortsCount() == 0);

    // Nothing to make: why.
    r.hl.accept(1);
    r.hl.accept(3);
    for (int rank : {1, 2, 3, 4, 5})
        r.hl.reject(rank);
    CHECK(r.hl.shortsCount() == 0);
    CHECK_FALSE(r.hl.makeShorts({}).isEmpty());
}

TEST_CASE("Highlights: Cancel keeps the Shorts made so far", "[app][highlights]") {
    Rig r;
    REQUIRE(r.hl.find({}).isEmpty());
    r.hl.accept(1);
    r.hl.accept(2);
    r.hl.accept(3);
    r.hl.setSynchronous(false);
    g_shortHold = true;
    const qsizetype sequences = r.controller.sequenceNames().size();
    const std::size_t before = r.undo();
    REQUIRE(r.hl.makeShorts({}).isEmpty());
    CHECK(r.hl.busy());
    CHECK(r.hl.job() == QStringLiteral("shorts"));
    // The second Short waits at its checkpoints: Cancel drops it and keeps the first.
    REQUIRE(waitFor([&] { return g_shortCalls.load() >= 2; }, 10000));
    r.hl.cancel();
    REQUIRE(waitFor([&] { return !r.hl.busy(); }, 10000));
    g_shortHold = false;
    INFO(r.hl.message().toStdString());
    CHECK(r.controller.sequenceNames().size() == sequences + 1);
    CHECK(r.controller.sequenceNames().contains(QStringLiteral("Clutch round (Short)"))); // rank 2, the earliest
    CHECK(r.undo() == before + 1);
    CHECK(r.hl.madeShortsCount() == 1);
    CHECK(r.hl.message().contains(QStringLiteral("Cancelled after 1 of 3.")));
    CHECK(r.controller.currentSequenceId() == r.tl.seq);
}

TEST_CASE("Highlights: Export all queues each Short with the vertical preset", "[app][highlights]") {
    Rig r;
    REQUIRE(r.hl.find({}).isEmpty());
    r.hl.accept(1);
    r.hl.accept(3);
    REQUIRE(r.hl.makeShorts({}).isEmpty());
    REQUIRE(r.hl.madeShortsCount() == 2);
    struct Queued {
        QString sequence;
        QString preset;
        QVariantMap overrides;
    };
    std::vector<Queued> queued;
    r.hl.setQueueExport([&](const QString& sequence, const QString& preset, const QVariantMap& overrides) {
        queued.push_back({sequence, preset, overrides});
        return QStringLiteral("j%1").arg(queued.size());
    });
    CHECK(r.hl.exportShorts(false) == 2);
    REQUIRE(queued.size() == 2);
    for (const Queued& q : queued) {
        CHECK(q.preset == QStringLiteral("vertical-1080x1920"));
        CHECK(q.overrides.isEmpty());
        const auto id = parseSequenceKey(q.sequence);
        REQUIRE(id);
        const Sequence* made = r.controller.snapshot()->findSequence(*id);
        REQUIRE(made);
        CHECK(made->height == 1920);
    }
    CHECK(queued[0].sequence != queued[1].sequence);
    CHECK(r.hl.exportShorts(true) == 2);
    REQUIRE(queued.size() == 4);
    CHECK(queued[2].overrides == QVariantMap{{QStringLiteral("videoCodec"), QStringLiteral("hevc")}});
    // A refused export is not counted; undone Shorts are not queued.
    r.hl.setQueueExport([&](const QString&, const QString&, const QVariantMap&) { return QString(); });
    CHECK(r.hl.exportShorts(false) == 0);
    REQUIRE(r.controller.undo());
    r.hl.setQueueExport([&](const QString& sequence, const QString& preset, const QVariantMap& overrides) {
        queued.push_back({sequence, preset, overrides});
        return QStringLiteral("j");
    });
    CHECK(r.hl.exportShorts(false) == 0);
    CHECK(queued.size() == 4);
}

TEST_CASE("Highlights: lead-in and tail and a minimum length widen the Short", "[app][highlights]") {
    Rig r;
    REQUIRE(r.hl.find(opts({{QStringLiteral("minScore"), 0.1}})).isEmpty());
    REQUIRE(r.ranks().size() == 6);
    const auto handles = [&](int rank, const QVariantMap& options) {
        const QVariantMap p = r.hl.shortParams(rank, options);
        return std::pair{p.value(QStringLiteral("handleBefore"), 0.0).toDouble(), p.value(QStringLiteral("handleAfter"), 0.0).toDouble()};
    };
    // No options: no handles sent (the command's 0).
    const QVariantMap plain = r.hl.shortParams(1, {});
    CHECK_FALSE(plain.contains(QStringLiteral("handleBefore")));
    CHECK_FALSE(plain.contains(QStringLiteral("handleAfter")));
    // Rank 1 is 18-22 s of a 30 s sequence. Lead-in and tail pass through.
    auto [before, after] = handles(1, opts({{QStringLiteral("handleBefore"), 5.0}, {QStringLiteral("handleAfter"), 2.0}}));
    CHECK(before == Approx(5.0));
    CHECK(after == Approx(2.0));
    // 99 is clamped to 30, then to the 18 s before the moment; the rest goes after it, up to the sequence's end.
    std::tie(before, after) = handles(1, opts({{QStringLiteral("handleBefore"), 99.0}}));
    CHECK(before == Approx(18.0));
    CHECK(after == Approx(8.0));
    // "At least" 15 s on a 4 s moment: 11 s more, two thirds before and one third after.
    std::tie(before, after) = handles(1, opts({{QStringLiteral("minLength"), 15}}));
    CHECK(before + after == Approx(11.0));
    CHECK(before == Approx(22.0 / 3.0).margin(0.001));
    CHECK(after == Approx(11.0 / 3.0).margin(0.001));
    // "At least" 30 s: 26 s more; the end of the sequence caps the tail, the lead-in takes the rest (to the start).
    std::tie(before, after) = handles(1, opts({{QStringLiteral("minLength"), 30}}));
    CHECK(before + after == Approx(26.0));
    CHECK(after == Approx(8.0));
    // A moment at 1 s with a 5 s lead-in: 1 s before, the other 4 s after.
    std::tie(before, after) = handles(6, opts({{QStringLiteral("handleBefore"), 5.0}}));
    CHECK(before == Approx(1.0));
    CHECK(after == Approx(4.0));
    // The caption style goes with captions only.
    CHECK(r.hl.shortParams(1, opts({{QStringLiteral("captionPreset"), QStringLiteral("Clean")}}))
              .value(QStringLiteral("captionPreset")).toString() == QStringLiteral("Clean"));
    CHECK_FALSE(r.hl.shortParams(1, opts({{QStringLiteral("captions"), false}, {QStringLiteral("captionPreset"), QStringLiteral("Clean")}}))
                    .contains(QStringLiteral("captionPreset")));
    // A batch uses the same options: each Short's request has its handles.
    r.hl.accept(1);
    REQUIRE(r.hl.makeShorts(opts({{QStringLiteral("handleBefore"), 3.0}, {QStringLiteral("handleAfter"), 1.0}})).isEmpty());
    const Json sent = last(g_lastShort);
    INFO(sent.dump());
    CHECK(sent["handleBefore"].get<double>() == Approx(3.0));
    CHECK(sent["handleAfter"].get<double>() == Approx(1.0));
}

TEST_CASE("Highlights: At least 60 s reaches 60 s when one side hits its 30 s cap", "[app][highlights]") {
    // Fix round 1 (D): the time the 30 s cap cuts from one side goes to the other (up to its own 30 s), as the
    // sequence's start and end already do. A 4 s moment at 60 s of a 120 s recording.
    const std::string body = "G" + std::string(2999, 'g');
    Rig r(true, "V1 tgt |" + body + "|\nA1 tgt |" + body + "|\n", {{'G', 3000}});
    Marker marker;
    marker.start = seconds(60.0);
    marker.duration = seconds(4.0);
    marker.name = "Highlight 1: Late clutch";
    marker.highlight = MarkerHighlight{.score = 0.9, .reasons = {"you shouted"}, .peak = seconds(62.0)};
    REQUIRE(r.controller.session().apply("Add Marker", [&](const ProjectState& s) { return edit::addMarker(s, r.tl.seq, marker); }));
    REQUIRE(r.ranks() == std::vector<int>{1});
    const auto handles = [&](const QVariantMap& options) {
        const QVariantMap p = r.hl.shortParams(1, options);
        return std::pair{p.value(QStringLiteral("handleBefore"), 0.0).toDouble(), p.value(QStringLiteral("handleAfter"), 0.0).toDouble()};
    };
    // The defaults (3 s / 1 s) and At least 60 s: 56 s more; two thirds before is over 30, so the tail takes the rest.
    auto [before, after] = handles(opts({{QStringLiteral("handleBefore"), 3.0}, {QStringLiteral("handleAfter"), 1.0},
                                         {QStringLiteral("minLength"), 60}}));
    CHECK(before == Approx(30.0));
    CHECK(after == Approx(26.0).margin(0.001));
    CHECK(4.0 + before + after == Approx(60.0).margin(0.001));
    // The other way round: a 30 s tail already, so the lead-in takes what the tail cannot.
    std::tie(before, after) = handles(opts({{QStringLiteral("handleAfter"), 30.0}, {QStringLiteral("minLength"), 60}}));
    CHECK(after == Approx(30.0));
    CHECK(before == Approx(26.0).margin(0.001));
    // Both sides full: 64 s is the most a 4 s moment gets (the command caps a Short at 3 minutes anyway).
    std::tie(before, after) = handles(opts({{QStringLiteral("handleBefore"), 30.0}, {QStringLiteral("handleAfter"), 30.0},
                                            {QStringLiteral("minLength"), 90}}));
    CHECK(before == Approx(30.0));
    CHECK(after == Approx(30.0));
}

TEST_CASE("Highlights: a ripple edit before reviewed moments keeps their accept and reject", "[app][highlights]") {
    // Fix round 1 (D): reviews follow their markers when a ripple edit moves them (Ripple Sequence Markers is on by
    // default in 0.4), so Make Shorts still makes the accepted moments.
    Rig r;
    REQUIRE(r.hl.find({}).isEmpty());
    REQUIRE(r.ranks() == std::vector<int>{1, 2, 3, 4, 5});
    r.hl.accept(1);  // 18-22 s
    r.hl.accept(3);  // 25-28 s
    r.hl.reject(4);  // 10-13 s
    r.hl.setSelectedRank(3);
    REQUIRE(r.hl.shortsCount() == 2);
    const auto state = [&](int rank) { return r.moment(rank).value(QStringLiteral("state")).toString(); };

    // Extract 8-10 s: every moment after it moves 2 s earlier.
    const auto extracted = r.controller.session().apply("Extract", [&](const ProjectState& s) -> edit::EditOutcome {
        auto marked = edit::markIn(s, r.tl.seq, seconds(8.0));
        if (!marked)
            return marked;
        marked = edit::markOut(marked->state, r.tl.seq, seconds(10.0) - f(1));
        if (!marked)
            return marked;
        return edit::extract(marked->state, r.tl.seq);
    });
    REQUIRE(extracted);
    REQUIRE(r.moment(1).value(QStringLiteral("start")).toDouble() == Approx(16.0));
    CHECK(r.moment(3).value(QStringLiteral("start")).toDouble() == Approx(23.0));
    CHECK(state(1) == QStringLiteral("accepted"));
    CHECK(state(3) == QStringLiteral("accepted"));
    CHECK(state(4) == QStringLiteral("rejected"));
    CHECK(state(2).isEmpty());
    CHECK(state(5).isEmpty());
    CHECK(r.hl.shortsCount() == 2);
    CHECK(r.hl.selectedRank() == 3);

    // Undo puts the markers back and the reviews go with them; redo moves them again.
    REQUIRE(r.controller.undo());
    REQUIRE(r.moment(1).value(QStringLiteral("start")).toDouble() == Approx(18.0));
    CHECK(state(1) == QStringLiteral("accepted"));
    CHECK(state(3) == QStringLiteral("accepted"));
    CHECK(state(4) == QStringLiteral("rejected"));
    REQUIRE(r.controller.redo());
    CHECK(state(1) == QStringLiteral("accepted"));
    CHECK(state(4) == QStringLiteral("rejected"));

    // Make Shorts makes the two accepted moments, not every moment that is not rejected.
    const qsizetype sequences = r.controller.sequenceNames().size();
    REQUIRE(r.hl.makeShorts({}).isEmpty());
    CHECK(r.controller.sequenceNames().size() == sequences + 2);
    CHECK(r.controller.sequenceNames().contains(QStringLiteral("Triple kill + You shouted (Short)")));
    CHECK(r.controller.sequenceNames().contains(QStringLiteral("Boss down (Short)")));
}

TEST_CASE("Highlights panel in the window: cards, the keyboard path, options, reel; a11y; screenshot",
          "[app][highlights][qml][screenshot]") {
    // 0.4 too: Make Shorts (N) and Export all, the Short options' Length group and caption style, the Short's length in
    // its tooltip (screenshots highlight-short-options.png and highlights-shorts-batch.png).
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* hl = shell.engine.singletonInstance<HighlightsBridge*>("Wabcut", "Highlights");
    REQUIRE(controller);
    REQUIRE(hl);
    g_findSteps = 4;
    g_stepMs = 0;
    g_chatImported = false;
    hl->setSynchronous(true);
    std::vector<bool> transport;
    hl->setTransport([&transport](bool play) { transport.push_back(play); });
    setPreviewFramesForTests(testFrame);
    Timeline tl{gameplayPicture()};
    controller->resetProject(tl.state, tl.seq);
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickWindow* w = shell.window.get();

    // The panel in the browser slot, over the Library.
    QQuickItem* library = findNamed(w->contentItem(), QStringLiteral("libraryPanel"));
    REQUIRE(library);
    const auto panel = makeItem(shell, "import Wabcut\nHighlightsPanel {}\n", sceneRect(library));
    QQuickItem* findButton = findNamed(panel.get(), QStringLiteral("highlightsFindButton"));
    QQuickItem* empty = findNamed(panel.get(), QStringLiteral("highlightsEmpty"));
    QQuickItem* list = findNamed(panel.get(), QStringLiteral("highlightsList"));
    REQUIRE(findButton);
    REQUIRE(empty);
    REQUIRE(list);

    // The real catalog: unavailable until G.2 lands (Find off, the empty state says so).
    const bool realHasIt = ai::edit::editingCommands().knows("highlights.find");
    hl->setRegistry(nullptr);
    if (!realHasIt) {
        CHECK_FALSE(findButton->isEnabled());
        CHECK(empty->isVisible());
        CHECK(empty->property("text").toString().contains(QStringLiteral("next engine update")));
    }

    // The stub catalog: Find on, the first-run empty state.
    hl->setRegistry(&catalog());
    CHECK(findButton->isEnabled());
    CHECK(empty->property("text").toString().startsWith(QStringLiteral("Find the moments")));
    CHECK(tabReaches(panel.get(), QStringLiteral("highlightsFindButton")));

    // Find options: turn the picture off; the find sends the others (chat needs a log).
    QQuickItem* optionsButton = findNamed(panel.get(), QStringLiteral("highlightsOptionsButton"));
    REQUIRE(optionsButton);
    auto* findOptions = panel->findChild<QObject*>(QStringLiteral("highlightFindOptions"));
    REQUIRE(findOptions);
    REQUIRE(QMetaObject::invokeMethod(optionsButton, "clicked"));
    REQUIRE(waitFor([&] { return findOptions->property("opened").toBool(); }, 3000));
    auto* optionsContent = findOptions->property("contentItem").value<QQuickItem*>();
    REQUIRE(optionsContent);
    {
        const QStringList issues = auditAccessibility(optionsContent);
        INFO(join(issues));
        CHECK(issues.isEmpty());
    }
    QQuickItem* chatToggle = findNamed(optionsContent, QStringLiteral("highlightsSignalChat"));
    REQUIRE(chatToggle);
    CHECK(chatToggle->isEnabled());
    QQuickItem* chatSummary = findNamed(optionsContent, QStringLiteral("highlightsChatSummary"));
    REQUIRE(chatSummary);
    CHECK(chatSummary->property("text").toString().startsWith(QStringLiteral("No chat log")));
    CHECK(findNamed(optionsContent, QStringLiteral("highlightsChatFileButton"))->isEnabled());
    // The file dialog's choice goes to importChat (the dialog itself is native): the popover says what was read.
    QTemporaryDir dir;
    const QString chatPath = QDir(dir.path()).filePath(QStringLiteral("stream-chat.json"));
    {
        QFile f(chatPath);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("{\"comments\": []}");
    }
    REQUIRE(hl->importChat(QUrl::fromLocalFile(chatPath).toString()).isEmpty());
    CHECK(chatSummary->property("text").toString() ==
          QStringLiteral("Twitch chat · Messages: %1 · Gameplay.mkv").arg(QLocale().toString(qlonglong(18234))));
    CHECK(findNamed(optionsContent, QStringLiteral("highlightsChatFileButton"))->property("text").toString() ==
          QStringLiteral("stream-chat.json"));
    QQuickItem* visualToggle = findNamed(optionsContent, QStringLiteral("highlightsSignalVisual"));
    REQUIRE(visualToggle);
    REQUIRE(QMetaObject::invokeMethod(visualToggle, "toggle"));
    CHECK_FALSE(visualToggle->property("checked").toBool());
    QMetaObject::invokeMethod(findOptions, "close");
    REQUIRE(waitFor([&] { return !findOptions->property("visible").toBool(); }, 3000));

    REQUIRE(QMetaObject::invokeMethod(findButton, "clicked"));
    const Json sent = last(g_lastFind);
    INFO(sent.dump());
    CHECK(sent["signals"] == Json::array({"game", "mic", "chat", "markers"}));
    CHECK(sent["count"] == 10);
    CHECK(hl->moments().size() == 5);
    CHECK_FALSE(empty->isVisible());
    polishAll(panel.get());
    polishAll(panel.get()); // the cards the list made
    REQUIRE(findNamed(panel.get(), QStringLiteral("highlightCard_1")) != nullptr);
    CHECK(findNamed(panel.get(), QStringLiteral("highlightsReelButton"))->isVisible());

    // The keyboard: Tab reaches the list; Down moves, A accepts, R rejects, Enter jumps, Space previews.
    CHECK(tabReaches(panel.get(), QStringLiteral("highlightsList")));
    CHECK(tabReaches(panel.get(), QStringLiteral("highlightsReelButton")));
    CHECK(pressWithKey(w, list, Qt::Key_Down));
    CHECK(hl->selectedRank() == 2);
    CHECK(pressWithKey(w, list, Qt::Key_A, QStringLiteral("a")));
    CHECK(hl->acceptedCount() == 1);
    CHECK(list->hasActiveFocus());
    CHECK(pressWithKey(w, list, Qt::Key_Down));
    CHECK(hl->selectedRank() == 3);
    CHECK(pressWithKey(w, list, Qt::Key_R, QStringLiteral("r")));
    CHECK(hl->moments().last().toMap().value(QStringLiteral("rank")).toInt() == 3); // rejected: last
    CHECK(hl->selectedRank() == 3);                                                   // the keyboard stays on it
    CHECK(list->property("currentIndex").toInt() == 4);
    CHECK(pressWithKey(w, list, Qt::Key_Home));
    CHECK(hl->selectedRank() == 1);
    CHECK(pressWithKey(w, list, Qt::Key_Return));
    CHECK(onFrameOf(controller->playhead(), 20.5));
    CHECK(pressWithKey(w, list, Qt::Key_Space, QStringLiteral(" ")));
    CHECK(hl->previewRank() == 1);
    CHECK(transport == std::vector<bool>{true});
    CHECK(pressWithKey(w, list, Qt::Key_Space, QStringLiteral(" ")));
    CHECK(hl->previewRank() == 0);
    // The card's own buttons do the same.
    hl->setSelectedRank(4); // the list scrolls to it
    polishAll(panel.get()); // the cards are made again after each review
    polishAll(panel.get());
    QQuickItem* accept4 = findNamed(panel.get(), QStringLiteral("highlightAccept_4"));
    REQUIRE(accept4);
    REQUIRE(QMetaObject::invokeMethod(accept4, "clicked"));
    CHECK(hl->acceptedCount() == 2);
    // 0.4: Make Shorts counts the accepted moments; Export all waits for a batch.
    QQuickItem* shortsButton = findNamed(panel.get(), QStringLiteral("highlightsShortsButton"));
    REQUIRE(shortsButton);
    CHECK(shortsButton->property("text").toString() == QStringLiteral("Make Shorts (2)"));
    CHECK(shortsButton->isEnabled());
    QQuickItem* exportRow = findNamed(panel.get(), QStringLiteral("highlightsExportRow"));
    REQUIRE(exportRow);
    CHECK_FALSE(exportRow->isVisible());
    hl->setSelectedRank(1);
    polishAll(panel.get());
    polishAll(panel.get());
    QQuickItem* more = findNamed(panel.get(), QStringLiteral("highlightMoreReasons_1"));
    REQUIRE(more);
    CHECK(more->isVisible());
    REQUIRE(QMetaObject::invokeMethod(more, "clicked"));

    // Every control named.
    polishAll(panel.get());
    polishAll(panel.get());
    {
        const QStringList issues = auditAccessibility(panel.get());
        INFO(join(issues));
        CHECK(issues.isEmpty());
    }

    // The frames have arrived (decoded off the GUI thread by image://wabcut-frames).
    REQUIRE(waitFor(
        [&] {
            QList<QQuickItem*> frames;
            collectNamed(panel.get(), QStringLiteral("highlightFrame"), frames);
            if (frames.isEmpty())
                return false;
            for (QQuickItem* f : frames) {
                if (f->property("status").toInt() != 1) // Image.Ready
                    return false;
            }
            return true;
        },
        5000));
    // Make highlight reel from the footer: the accepted moments; the new sequence opens.
    QQuickItem* reelButton = findNamed(panel.get(), QStringLiteral("highlightsReelButton"));
    REQUIRE(reelButton);
    CHECK(reelButton->isEnabled());
    REQUIRE(QMetaObject::invokeMethod(reelButton, "clicked"));
    const Json reel = last(g_lastReel);
    INFO(reel.dump());
    REQUIRE(reel["moments"].size() == 2);
    CHECK(reel["moments"][0]["start"].get<double>() == Approx(4.0));  // rank 2
    CHECK(reel["moments"][1]["start"].get<double>() == Approx(10.0)); // rank 4
    CHECK(reel["transition"] == "crossDissolve");
    CHECK(reel["slowMotion"] == false);
    CHECK(controller->currentSequenceId() != tl.seq);
    // The reel is open; the list stays on the moments, said so.
    polishAll(panel.get());
    CHECK(findNamed(panel.get(), QStringLiteral("highlightsOtherSequence"))->isVisible());

    // Short options: the layout, and the layout picker's facecam region once it has one.
    auto* layouts = shell.engine.singletonInstance<LayoutBridge*>("Wabcut", "Layouts");
    REQUIRE(layouts);
    QQuickItem* shortOptionsButton = findNamed(panel.get(), QStringLiteral("highlightsShortOptionsButton"));
    REQUIRE(shortOptionsButton);
    auto* shortOptions = panel->findChild<QObject*>(QStringLiteral("highlightShortOptions"));
    REQUIRE(shortOptions);
    REQUIRE(QMetaObject::invokeMethod(shortOptionsButton, "clicked"));
    REQUIRE(waitFor([&] { return shortOptions->property("opened").toBool(); }, 3000));
    auto* shortContent = shortOptions->property("contentItem").value<QQuickItem*>();
    REQUIRE(shortContent);
    QQuickItem* useRegion = findNamed(shortContent, QStringLiteral("highlightsShortUseRegion"));
    REQUIRE(useRegion);
    CHECK_FALSE(useRegion->isVisible()); // the layout picker has no region yet
    layouts->setFacecamRegion(QVariantMap{{QStringLiteral("x"), 0.72}, {QStringLiteral("y"), 0.68},
                                          {QStringLiteral("width"), 0.24}, {QStringLiteral("height"), 0.28}});
    CHECK(useRegion->isVisible());
    REQUIRE(QMetaObject::invokeMethod(useRegion, "toggle"));
    QQuickItem* layoutTabs = findNamed(shortContent, QStringLiteral("highlightsShortLayout"));
    REQUIRE(layoutTabs);
    REQUIRE(layoutTabs->setProperty("currentIndex", 1)); // Split
    CHECK(findNamed(shortContent, QStringLiteral("highlightsShortSplitOptions"))->isVisible());
    CHECK_FALSE(findNamed(shortContent, QStringLiteral("highlightsShortZoomOptions"))->isVisible());
    {
        const QStringList issues = auditAccessibility(shortContent);
        INFO(join(issues));
        CHECK(issues.isEmpty());
    }
    // 0.4: Length - lead-in, tail and "At least" - and the caption style, from Preferences.
    QQuickItem* leadIn = findNamed(shortContent, QStringLiteral("highlightsShortLeadIn"));
    QQuickItem* tail = findNamed(shortContent, QStringLiteral("highlightsShortTail"));
    QQuickItem* minLength = findNamed(shortContent, QStringLiteral("highlightsShortMinLength"));
    QQuickItem* lengthHint = findNamed(shortContent, QStringLiteral("highlightsShortLengthHint"));
    QQuickItem* styleRow = findNamed(shortContent, QStringLiteral("highlightsShortCaptionStyleRow"));
    QQuickItem* styleButton = findNamed(shortContent, QStringLiteral("highlightsShortCaptionStyle"));
    QQuickItem* captionsToggle = findNamed(shortContent, QStringLiteral("highlightsShortCaptions"));
    REQUIRE(leadIn);
    REQUIRE(tail);
    REQUIRE(minLength);
    REQUIRE(lengthHint);
    REQUIRE(styleRow);
    REQUIRE(styleButton);
    REQUIRE(captionsToggle);
    CHECK(leadIn->property("value").toDouble() == Approx(3.0));
    CHECK(tail->property("value").toDouble() == Approx(1.0));
    CHECK(minLength->property("currentIndex").toInt() == 0);
    CHECK(lengthHint->isVisible());
    CHECK(styleRow->isVisible());
    CHECK(styleButton->property("text").toString() == QStringLiteral("Shorts"));
    // "At least" 30 s is remembered; a typed lead-in too.
    REQUIRE(QMetaObject::invokeMethod(minLength, "activated", Q_ARG(int, 2)));
    CHECK(shell.preferences->property("highlightsShortMinLength").toInt() == 30);
    CHECK(minLength->property("currentIndex").toInt() == 2);
    REQUIRE(QMetaObject::invokeMethod(leadIn, "valueModified", Q_ARG(double, 4.5)));
    CHECK(shell.preferences->property("highlightsShortLeadIn").toDouble() == Approx(4.5));
    CHECK(leadIn->property("value").toDouble() == Approx(4.5));
    // The style menu: the caption presets, Shorts first; the choice is remembered.
    QObject* styleMenu = shortOptions->findChild<QObject*>(QStringLiteral("highlightsShortCaptionStyleMenu"));
    REQUIRE(styleMenu);
    REQUIRE(QMetaObject::invokeMethod(styleButton, "clicked"));
    REQUIRE(waitFor([&] { return styleMenu->property("opened").toBool(); }, 3000));
    QStringList presets;
    for (const QVariant& v : styleMenu->property("options").toList())
        presets << v.toMap().value(QStringLiteral("name")).toString();
    INFO(presets.join(QStringLiteral(", ")).toStdString());
    REQUIRE_FALSE(presets.isEmpty());
    CHECK(presets.front() == QStringLiteral("Shorts"));
    CHECK(presets.contains(QStringLiteral("Clean")));
    REQUIRE(QMetaObject::invokeMethod(styleMenu, "chosen", Q_ARG(QString, QStringLiteral("Clean"))));
    REQUIRE(QMetaObject::invokeMethod(styleMenu, "close"));
    REQUIRE(waitFor([&] { return !styleMenu->property("visible").toBool(); }, 3000));
    CHECK(shell.preferences->property("highlightsShortCaptionPreset").toString() == QStringLiteral("Clean"));
    CHECK(styleButton->property("text").toString() == QStringLiteral("Clean"));
    // Captions off: no style to pick.
    REQUIRE(QMetaObject::invokeMethod(captionsToggle, "toggle"));
    CHECK_FALSE(styleRow->isVisible());
    REQUIRE(QMetaObject::invokeMethod(captionsToggle, "toggle"));
    CHECK(styleRow->isVisible());
    // The popover open: Layout, Length, Facecam, Captions and their style.
    shell.preferences->setProperty("highlightsShortMinLength", 0);
    shell.preferences->setProperty("highlightsShortLeadIn", 3.0);
    polishAll(panel.get());
    spin(50);
    {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(QDir(outputDir()).filePath(QStringLiteral("highlight-short-options.png"))));
    }
    QMetaObject::invokeMethod(shortOptions, "close");
    REQUIRE(waitFor([&] { return !shortOptions->property("visible").toBool(); }, 3000));

    // Make a Short of the chosen moment: the split with the region; the message says the layout.
    hl->setSelectedRank(2);
    QQuickItem* shortButton = findNamed(panel.get(), QStringLiteral("highlightsShortButton"));
    REQUIRE(shortButton);
    // 0.4: its tooltip says how long the Short is; a longer lead-in makes it longer.
    const QString tooltip = shortButton->property("tooltip").toString();
    CHECK(tooltip.startsWith(QStringLiteral("A vertical 1080x1920 Short of #2, about ")));
    shell.preferences->setProperty("highlightsShortLeadIn", 13.0);
    CHECK(shortButton->property("tooltip").toString() != tooltip);
    shell.preferences->setProperty("highlightsShortLeadIn", 3.0);
    CHECK(shortButton->property("tooltip").toString() == tooltip);
    REQUIRE(QMetaObject::invokeMethod(shortButton, "clicked"));
    const Json made = last(g_lastShort);
    INFO(made.dump());
    CHECK(made["title"] == "Clutch round");
    CHECK(made["layout"] == "verticalSplit");
    CHECK(made["split"].get<double>() == Approx(0.6));
    CHECK(made["facecamOnTop"] == false);
    CHECK(made["facecamRegion"]["x"].get<double>() == Approx(0.72));
    CHECK_FALSE(made.contains("zoom"));
    CHECK_FALSE(made.contains("mode"));
    // 0.4: the lead-in and tail of the Short options, the caption style chosen.
    CHECK(made["handleBefore"].get<double>() == Approx(3.0));
    CHECK(made["handleAfter"].get<double>() == Approx(1.0));
    CHECK(made["captionPreset"] == "Clean");
    shell.preferences->setProperty("highlightsShortCaptionPreset", QStringLiteral("Shorts"));
    CHECK(controller->currentSequence()->height == 1920);
    QQuickItem* message = findNamed(panel.get(), QStringLiteral("highlightsMessage"));
    REQUIRE(message);
    CHECK(message->property("text").toString().contains(QStringLiteral("gameplay and facecam split")));

    // The screenshot: the list on its moments beside the Short just made, the message with its layout.
    hl->setSelectedRank(1);
    polishAll(panel.get());
    polishAll(panel.get());
    REQUIRE(waitFor(
        [&] {
            QList<QQuickItem*> frames;
            collectNamed(panel.get(), QStringLiteral("highlightFrame"), frames);
            for (QQuickItem* f : frames) {
                if (f->property("status").toInt() != 1) // Image.Ready
                    return false;
            }
            return !frames.isEmpty();
        },
        5000));
    // The summary: words that read right at 1, counts in the locale's format.
    QQuickItem* summary = findNamed(panel.get(), QStringLiteral("highlightsSummary"));
    REQUIRE(summary);
    CHECK(summary->property("text").toString() ==
          QStringLiteral("Moments: 5 · Recordings: 1 · Chat messages: %1 · Accepted: 2").arg(QLocale().toString(qlonglong(18234))));
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    REQUIRE(image.save(QDir(outputDir()).filePath(QStringLiteral("highlights-panel.png"))));
    layouts->clearRegion();

    // 0.4: Make Shorts - a Short of each accepted moment, one undo step, the timeline stays on what it shows; then
    // Export all queues them (HEVC when its toggle is on).
    CHECK(shortsButton->property("text").toString() == QStringLiteral("Make Shorts (2)"));
    CHECK(shortsButton->isEnabled());
    const SequenceId shownBefore = controller->currentSequenceId();
    const std::size_t beforeBatch = controller->session().undoCount();
    REQUIRE(QMetaObject::invokeMethod(shortsButton, "clicked"));
    INFO(hl->message().toStdString());
    CHECK(controller->session().undoCount() == beforeBatch + 1);
    CHECK(controller->session().undoLabel().value_or("") == "Make Highlight Shorts");
    CHECK(controller->currentSequenceId() == shownBefore);
    CHECK(hl->madeShortsCount() == 2);
    polishAll(panel.get());
    polishAll(panel.get());
    CHECK(exportRow->isVisible());
    QQuickItem* exportButton = findNamed(panel.get(), QStringLiteral("highlightsExportShortsButton"));
    QQuickItem* hevc = findNamed(panel.get(), QStringLiteral("highlightsExportHevc"));
    REQUIRE(exportButton);
    REQUIRE(hevc);
    CHECK(exportButton->property("text").toString() == QStringLiteral("Export all (2)"));
    QStringList queued;
    hl->setQueueExport([&queued](const QString& sequence, const QString& preset, const QVariantMap& overrides) {
        queued << preset + QLatin1Char(' ') + overrides.value(QStringLiteral("videoCodec")).toString() + QLatin1Char(' ') + sequence;
        return QStringLiteral("job%1").arg(queued.size());
    });
    REQUIRE(QMetaObject::invokeMethod(hevc, "toggle"));
    REQUIRE(QMetaObject::invokeMethod(exportButton, "clicked"));
    INFO(queued.join(QStringLiteral(" | ")).toStdString());
    REQUIRE(queued.size() == 2);
    CHECK(queued[0].startsWith(QStringLiteral("vertical-1080x1920 hevc ")));
    CHECK(queued[1].startsWith(QStringLiteral("vertical-1080x1920 hevc ")));
    {
        const QStringList issues = auditAccessibility(panel.get());
        INFO(join(issues));
        CHECK(issues.isEmpty());
    }
    spin(50);
    {
        const QImage batch = shell.grab();
        REQUIRE_FALSE(batch.isNull());
        REQUIRE(batch.save(QDir(outputDir()).filePath(QStringLiteral("highlights-shorts-batch.png"))));
    }
    // Undo takes the batch away; Export all hides.
    REQUIRE(controller->undo());
    CHECK(hl->madeShortsCount() == 0);
    CHECK_FALSE(exportRow->isVisible());
    hl->setQueueExport([](const QString&, const QString&, const QVariantMap&) { return QString(); });

    hl->setTransport(nullptr);
    hl->setRegistry(nullptr);
    setPreviewFramesForTests(nullptr);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
