// 0.6 (P1 round, stream 2C): export.
//   * #20 the export queue across restarts (ExportQueueKeeper.cpp): waiting exports come back held, an export stopped
//         by a crash comes back interrupted with Retry (from the project as it was queued), a finished one done; one
//         Wabcut keeps the folder; Cancel Exports and Quit / Clear keep nothing;
//   * #22 several sequences queued at once (ExportBatches.cpp): each file named after its sequence, the preset of each
//         shape, all or nothing;
//   * #26 each ranged marker or selected clip as its own file;
//   * #25 the media usage report written by the Export controller (UTF-8 with a byte order mark);
//   * in the window: the Export Sequences, Export Ranges and Media Usage Report dialogs, the restored-queue toast, the
//     Library's Export rows and the quit question's Keep Queue and Quit (screenshots, dark and light).

#include "A11yAudit.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "ExportController.h"
#include "ProjectController.h"

#include <wabcut/command/Engine.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/export/VideoFrameSource.h>

#include <catch2/catch_test_macros.hpp>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

namespace ex = wabcut::exporter;
namespace fs = std::filesystem;

fs::path freshDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "app" / "export-2c" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

QString q(const fs::path& p) {
    return QString::fromStdU16String(p.u16string());
}

bool sameFile(const QString& a, const QString& b) {
    return !a.isEmpty() && QDir::cleanPath(QDir::fromNativeSeparators(a)).compare(
                               QDir::cleanPath(QDir::fromNativeSeparators(b)), Qt::CaseInsensitive) == 0;
}

/// A sequence of `seconds` of nothing (exports render silence and the synthetic picture).
SequencePtr emptySequence(ProjectState& state, const char* name, double seconds, int width = 640, int height = 360) {
    Sequence seq;
    seq.id = state.ids.allocate<SequenceId>();
    seq.name = name;
    seq.width = width;
    seq.height = height;
    seq.frameRate = kRate30;
    seq.duration = Ticks::fromSeconds(seconds);
    return std::make_shared<const Sequence>(std::move(seq));
}

ProjectState emptyProject(double seconds, const char* name = "Clip") {
    ProjectState state;
    state.sequences.push_back(emptySequence(state, name, seconds));
    return state;
}

bool finished(const ExportController& c, const QString& jobId, int timeoutMs = 60'000) {
    return waitFor([&] { return c.job(jobId).value(QStringLiteral("finished")).toBool(); }, timeoutMs);
}

/// Adds a sequence marker through marker.add (one undo step, as the app does).
/// `start` and `length` in frames at 30 fps (marker.add takes frame-exact ticks).
void addMarker(ProjectController& project, int start, int length, const char* name, int color) {
    command::CommandEngine engine(project.session());
    command::CommandRequest add;
    add.command = "marker.add";
    add.params = command::Json{{"time", framesToTicks(start, kRate30).value}, {"name", name}, {"colorIndex", color},
                               {"sequence", project.currentSequenceId().value}};
    if (length > 0)
        add.params["duration"] = framesToTicks(length, kRate30).value;
    const command::CommandResult r = engine.execute(add);
    INFO(command::toJson(r).dump());
    REQUIRE(r.ok());
}

QJsonArray keptJobs(const fs::path& folder) {
    QFile file(q(folder / "queue.json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("jobs")).toArray();
}

int snapshotFiles(const fs::path& folder) {
    return static_cast<int>(QDir(q(folder)).entryList({QStringLiteral("*.wabcut")}, QDir::Files).size());
}

QVariant role(const ExportController& c, const QString& jobId, int roleId) {
    ExportJobModel* jobs = c.jobs();
    for (int i = 0; i < jobs->rowCount(); ++i) {
        if (jobs->data(jobs->index(i), ExportJobModel::JobIdRole).toString() == jobId)
            return jobs->data(jobs->index(i), roleId);
    }
    return {};
}

/// The job id of the row whose output file is named `name`.
QString jobNamed(const ExportController& c, const QString& name) {
    ExportJobModel* jobs = c.jobs();
    for (int i = 0; i < jobs->rowCount(); ++i) {
        if (jobs->data(jobs->index(i), ExportJobModel::TitleRole).toString() == name)
            return jobs->data(jobs->index(i), ExportJobModel::JobIdRole).toString();
    }
    return {};
}

QQuickItem* findWith(QQuickItem* root, const std::function<bool(QQuickItem*)>& match) {
    if (!root)
        return nullptr;
    if (match(root))
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findWith(child, match))
            return found;
    }
    return nullptr;
}

QVariant evaluate(const Shell& shell, const QString& code) {
    QQmlExpression expression(qmlContext(shell.window.get()), shell.window.get(), code);
    const QVariant result = expression.evaluate();
    INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    return result;
}

/// An item of a dialog (popups live in the window's overlay).
QQuickItem* itemNamed(const Shell& shell, QObject* dialog, const QString& name) {
    if (QQuickItem* found = findWith(shell.window->contentItem(), [&](QQuickItem* i) { return i->objectName() == name; }))
        return found;
    const auto items = dialog->findChildren<QQuickItem*>(name);
    return items.isEmpty() ? nullptr : items.front();
}

} // namespace

// ---- #20: the queue across restarts ------------------------------------------------------------------------------------

TEST_CASE("Export queue across restarts: waiting exports come back held and a crash leaves them interrupted with Retry",
          "[app][export][p1]") {
    resetSettings();
    const fs::path dir = freshDir("queue");
    const fs::path folder = dir / "Export Queue";
    ProjectController project;
    project.resetProject(emptyProject(1.0));

    // Session 1: two exports wait for Start Queue; Wabcut closes.
    {
        ExportController a;
        a.setProject(&project);
        a.setVideoSourceFactory(ex::syntheticVideoSourceFactory());
        a.setQueueFolder(folder);
        CHECK_FALSE(a.keepsQueue());
        const QVariantMap first = a.restoreQueue();
        CHECK(first.value(QStringLiteral("count")).toInt() == 0);
        CHECK(a.keepsQueue());
        CHECK(a.restoreQueue().value(QStringLiteral("count")).toInt() == 0); // once
        const QStringList one = a.sendToQueue({}, {QStringLiteral("audio-wav")}, {{QStringLiteral("output"), q(dir / "one.wav")}}, true);
        const QStringList two = a.sendToQueue({}, {QStringLiteral("audio-wav")}, {{QStringLiteral("output"), q(dir / "two.wav")}}, true);
        REQUIRE(one.size() == 1);
        REQUIRE(two.size() == 1);
        REQUIRE(waitFor([&] { return keptJobs(folder).size() == 2; }));
        CHECK(keptJobs(folder).at(0).toObject().value(QStringLiteral("state")).toString() == QStringLiteral("held"));
        CHECK(snapshotFiles(folder) == 1); // one snapshot, shared by both jobs
        // An edit after queueing does not change what they export (they keep the project as it was queued).
        project.resetProject(emptyProject(3.0));
    }
    // Closing cancelled nothing for good: both still wait in the folder.
    QJsonArray kept = keptJobs(folder);
    REQUIRE(kept.size() == 2);

    // A crash mid-export: "two" was running (no file of its own yet); "three" had finished writing just before.
    {
        QJsonObject running = kept.at(1).toObject();
        running[QStringLiteral("state")] = QStringLiteral("running");
        running[QStringLiteral("startedAt")] = QDateTime::currentDateTimeUtc().addSecs(-30).toString(Qt::ISODateWithMs);
        kept[1] = running;
        QJsonObject done = running;
        done[QStringLiteral("output")] = q(dir / "three.wav");
        done[QStringLiteral("title")] = QStringLiteral("three.wav");
        kept.append(done);
        { std::ofstream(dir / "three.wav", std::ios::binary) << "RIFF"; }
        QJsonObject broken = kept.at(0).toObject();
        broken[QStringLiteral("project")] = QStringLiteral("q-00000000000000000000000000000000.wabcut");
        broken[QStringLiteral("title")] = QStringLiteral("lost.wav");
        kept.append(broken);
        QFile file(q(folder / "queue.json"));
        REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("jobs"), kept}}).toJson());
    }

    // Session 2, on another project: they come back.
    {
        ProjectController other;
        other.resetProject(emptyProject(2.0, "Other"));
        ExportController b;
        b.setProject(&other);
        b.setVideoSourceFactory(ex::syntheticVideoSourceFactory());
        b.setQueueFolder(folder);
        const QVariantMap r = b.restoreQueue();
        CHECK(r.value(QStringLiteral("count")).toInt() == 3);
        CHECK(r.value(QStringLiteral("held")).toInt() == 1);
        CHECK(r.value(QStringLiteral("interrupted")).toInt() == 1);
        CHECK(r.value(QStringLiteral("done")).toInt() == 1);
        CHECK(r.value(QStringLiteral("skipped")).toStringList() == QStringList{QStringLiteral("lost.wav")});
        const QString held = jobNamed(b, QStringLiteral("one.wav"));
        const QString interrupted = jobNamed(b, QStringLiteral("two.wav"));
        const QString done = jobNamed(b, QStringLiteral("three.wav"));
        REQUIRE_FALSE(held.isEmpty());
        REQUIRE_FALSE(interrupted.isEmpty());
        REQUIRE_FALSE(done.isEmpty());
        CHECK(b.jobs()->roleNames().value(ExportJobModel::RestoredRole) == QByteArray("restored"));
        CHECK(role(b, held, ExportJobModel::RestoredRole).toBool());
        CHECK(role(b, done, ExportJobModel::StateRole).toString() == QStringLiteral("done"));
        CHECK(role(b, interrupted, ExportJobModel::StateRole).toString() == QStringLiteral("interrupted"));
        CHECK(role(b, interrupted, ExportJobModel::FinishedRole).toBool());
        CHECK(role(b, interrupted, ExportJobModel::RetryableRole).toBool());
        // Nothing starts by itself after a restart.
        spin(400);
        CHECK(role(b, held, ExportJobModel::StateRole).toString() == QStringLiteral("held"));
        CHECK_FALSE(fs::exists(dir / "one.wav"));
        // The folder now names the restored jobs (interrupted ones too, so they survive another restart).
        REQUIRE(waitFor([&] { return keptJobs(folder).size() == 2; }));

        // A second Wabcut does not take the queue: it keeps its own in memory.
        {
            ExportController c;
            c.setQueueFolder(folder);
            CHECK(c.restoreQueue().value(QStringLiteral("locked")).toBool());
            CHECK_FALSE(c.keepsQueue());
        }

        // Retry: the interrupted export runs again from the project it was queued with (1 s, not the 2 s open now).
        const QString again = b.retry(interrupted);
        REQUIRE_FALSE(again.isEmpty());
        CHECK(b.job(interrupted).isEmpty()); // replaced by its retry
        REQUIRE(finished(b, again));
        CHECK(b.job(again).value(QStringLiteral("state")).toString() == QStringLiteral("done"));
        CHECK(b.job(again).value(QStringLiteral("detail")).toString().startsWith(QStringLiteral("0:01")));
        CHECK(fs::exists(dir / "two.wav"));
        // Start Queue runs the one that waited.
        b.startQueue();
        REQUIRE(finished(b, held));
        CHECK(b.job(held).value(QStringLiteral("state")).toString() == QStringLiteral("done"));
        CHECK(fs::exists(dir / "one.wav"));
        // Finished jobs leave the folder, and their snapshot with them.
        REQUIRE(waitFor([&] { return keptJobs(folder).isEmpty() && snapshotFiles(folder) == 0; }));
    }

    // Cancel Exports and Quit: nothing comes back, even when Wabcut closes before the cancellation is reported.
    {
        ExportController d;
        d.setProject(&project);
        d.setQueueFolder(folder);
        d.restoreQueue();
        REQUIRE(d.sendToQueue({}, {QStringLiteral("audio-wav")}, {{QStringLiteral("output"), q(dir / "four.wav")}}, true).size() == 1);
        REQUIRE(waitFor([&] { return keptJobs(folder).size() == 1; }));
        d.cancelAll();
        CHECK(keptJobs(folder).isEmpty());
    }
    // Clear (the start-up toast's): the restored exports are cancelled and the folder is empty.
    {
        ExportController e;
        e.setProject(&project);
        e.setQueueFolder(folder);
        CHECK(e.restoreQueue().value(QStringLiteral("count")).toInt() == 0);
        REQUIRE(e.sendToQueue({}, {QStringLiteral("audio-wav")}, {{QStringLiteral("output"), q(dir / "five.wav")}}, true).size() == 1);
        REQUIRE(waitFor([&] { return keptJobs(folder).size() == 1; }));
    }
    {
        ExportController f;
        f.setProject(&project);
        f.setQueueFolder(folder);
        CHECK(f.restoreQueue().value(QStringLiteral("held")).toInt() == 1);
        f.clearRestored();
        CHECK(keptJobs(folder).isEmpty());
        REQUIRE(waitFor([&] { return f.jobs()->activeCount() == 0; }));
        CHECK_FALSE(fs::exists(dir / "five.wav"));
    }
    resetSettings();
}

TEST_CASE("Export queue across restarts: an overwrite cut short by a crash comes back interrupted and Retry replaces its file",
          "[app][export][p1]") {
    // Fix round 1 (ARGON-11): a file at the export's path after a crash is not proof it finished when it is the file
    // the export was to replace, or when the export's temporary file is still there.
    resetSettings();
    const fs::path dir = freshDir("queue-overwrite");
    const fs::path folder = dir / "Export Queue";
    ProjectController project;
    project.resetProject(emptyProject(1.0));
    {
        ExportController a;
        a.setProject(&project);
        a.setVideoSourceFactory(ex::syntheticVideoSourceFactory());
        a.setQueueFolder(folder);
        a.restoreQueue();
        REQUIRE(a.sendToQueue({}, {QStringLiteral("audio-wav")}, {{QStringLiteral("output"), q(dir / "film.wav")}}, true).size() == 1);
        REQUIRE(a.sendToQueue({}, {QStringLiteral("audio-wav")}, {{QStringLiteral("output"), q(dir / "clip.wav")}}, true).size() == 1);
        REQUIRE(waitFor([&] { return keptJobs(folder).size() == 2; }));
    }
    QJsonArray kept = keptJobs(folder);
    REQUIRE(kept.size() == 2);
    {
        const QString startedAt = QDateTime::currentDateTimeUtc().addSecs(-30).toString(Qt::ISODateWithMs);
        // "film.wav": written after the job started, but the job's temporary file is still there: it never finished.
        { std::ofstream(dir / "film.wav", std::ios::binary) << "someone else's"; }
        { std::ofstream(dir / "film.wabcut-partial-0123456789abcdef.wav", std::ios::binary) << "RIFF"; }
        // "clip.wav": the file the job was to replace, untouched (the same last-modified time as when it started).
        { std::ofstream(dir / "clip.wav", std::ios::binary) << "old"; }
        const QString replaced = QFileInfo(q(dir / "clip.wav")).lastModified().toUTC().toString(Qt::ISODateWithMs);
        for (int i = 0; i < kept.size(); ++i) {
            QJsonObject e = kept.at(i).toObject();
            e[QStringLiteral("state")] = QStringLiteral("running");
            e[QStringLiteral("startedAt")] = startedAt;
            if (e.value(QStringLiteral("title")).toString() == QStringLiteral("clip.wav"))
                e[QStringLiteral("replacedModified")] = replaced;
            kept[i] = e;
        }
        QFile file(q(folder / "queue.json"));
        REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("jobs"), kept}}).toJson());
    }
    {
        ExportController b;
        b.setProject(&project);
        b.setVideoSourceFactory(ex::syntheticVideoSourceFactory());
        b.setQueueFolder(folder);
        const QVariantMap r = b.restoreQueue();
        CHECK(r.value(QStringLiteral("interrupted")).toInt() == 2);
        CHECK(r.value(QStringLiteral("done")).toInt() == 0);
        const QString film = jobNamed(b, QStringLiteral("film.wav"));
        const QString clip = jobNamed(b, QStringLiteral("clip.wav"));
        REQUIRE_FALSE(film.isEmpty());
        REQUIRE_FALSE(clip.isEmpty());
        CHECK(role(b, film, ExportJobModel::StateRole).toString() == QStringLiteral("interrupted"));
        CHECK(role(b, clip, ExportJobModel::StateRole).toString() == QStringLiteral("interrupted"));
        // Retry replaces the file the export was to replace (the path the user chose), not a numbered one.
        const QString again = b.retry(clip);
        REQUIRE_FALSE(again.isEmpty());
        CHECK(sameFile(b.job(again).value(QStringLiteral("outputPath")).toString(), q(dir / "clip.wav")));
        REQUIRE(finished(b, again));
        CHECK(b.job(again).value(QStringLiteral("state")).toString() == QStringLiteral("done"));
        CHECK(fs::file_size(dir / "clip.wav") > 3);
        CHECK_FALSE(fs::exists(dir / "clip (2).wav"));
        // A file written at the path after the export started is someone else's: that Retry gets a numbered name.
        const QString other = b.retry(film);
        REQUIRE_FALSE(other.isEmpty());
        CHECK(sameFile(b.job(other).value(QStringLiteral("outputPath")).toString(), q(dir / "film (2).wav")));
        REQUIRE(finished(b, other));
        CHECK(fs::file_size(dir / "film.wav") == 14);
        REQUIRE(waitFor([&] { return keptJobs(folder).isEmpty(); }));
    }
    resetSettings();
}

// ---- #22: several sequences at once ------------------------------------------------------------------------------------

TEST_CASE("Export Sequences: several sequences queue at once named after each and all or nothing", "[app][export][p1]") {
    resetSettings();
    const fs::path dir = freshDir("sequences");
    ProjectState state;
    const SequencePtr longEdit = emptySequence(state, "Long edit", 1.0);
    const SequencePtr shortCut = emptySequence(state, "Short", 1.0, 360, 640);
    const SequencePtr sameName = emptySequence(state, "Long edit", 1.0);
    state.sequences = {longEdit, shortCut, sameName};
    ProjectController project;
    project.resetProject(state);
    ExportController c;
    c.setProject(&project);
    c.setVideoSourceFactory(ex::syntheticVideoSourceFactory());
    const QStringList keys{sequenceKey(longEdit->id), sequenceKey(shortCut->id), sequenceKey(sameName->id)};

    // Each sequence's shape: YouTube for landscape, the vertical preset for 9:16; each file named after its sequence.
    const QVariantMap matchShape{{QStringLiteral("matchShape"), true}, {QStringLiteral("folder"), q(dir)}};
    const QVariantList entries = c.sequenceExports(keys, matchShape);
    REQUIRE(entries.size() == 3);
    for (const QVariant& e : entries) {
        INFO(e.toMap().value(QStringLiteral("error")).toString().toStdString());
        CHECK(e.toMap().value(QStringLiteral("ok")).toBool());
    }
    CHECK_FALSE(entries[0].toMap().value(QStringLiteral("portrait")).toBool());
    CHECK(entries[1].toMap().value(QStringLiteral("portrait")).toBool());
    CHECK(entries[0].toMap().value(QStringLiteral("presetName")).toString() !=
          entries[1].toMap().value(QStringLiteral("presetName")).toString());
    CHECK(QFileInfo(entries[0].toMap().value(QStringLiteral("output")).toString()).completeBaseName() == QStringLiteral("Long edit"));
    CHECK(QFileInfo(entries[1].toMap().value(QStringLiteral("output")).toString()).completeBaseName() == QStringLiteral("Short"));
    CHECK(QFileInfo(entries[2].toMap().value(QStringLiteral("output")).toString()).completeBaseName() == QStringLiteral("Long edit (2)"));

    // Queued held: three rows waiting for Start Queue; the folder becomes the next default.
    QVariantMap options = matchShape;
    options[QStringLiteral("hold")] = true;
    const QVariantMap queued = c.queueSequences(keys, options);
    INFO(queued.value(QStringLiteral("error")).toString().toStdString());
    REQUIRE(queued.value(QStringLiteral("ok")).toBool());
    CHECK(queued.value(QStringLiteral("count")).toInt() == 3);
    const QStringList jobs = queued.value(QStringLiteral("jobs")).toStringList();
    REQUIRE(jobs.size() == 3);
    for (const QString& job : jobs) {
        CHECK(c.job(job).value(QStringLiteral("state")).toString() == QStringLiteral("held"));
        CHECK(c.job(job).value(QStringLiteral("origin")).toString() == QStringLiteral("queue"));
    }
    CHECK(QDir::cleanPath(c.lastFolder()) == QDir::cleanPath(q(dir)));
    // Queued names are taken now: the same sequences again get numbered names.
    const QVariantList again = c.sequenceExports({sequenceKey(longEdit->id)}, matchShape);
    CHECK(QFileInfo(again[0].toMap().value(QStringLiteral("output")).toString()).completeBaseName() == QStringLiteral("Long edit (3)"));

    // One preset for all.
    const QVariantList wav = c.sequenceExports(keys, {{QStringLiteral("preset"), QStringLiteral("audio-wav")}, {QStringLiteral("folder"), q(dir)}});
    CHECK(wav[1].toMap().value(QStringLiteral("output")).toString().endsWith(QStringLiteral("Short (2).wav")) == false);
    CHECK(QFileInfo(wav[1].toMap().value(QStringLiteral("output")).toString()).fileName() == QStringLiteral("Short.wav"));

    // All or nothing: a sequence that is not there (or a folder that is not) queues none.
    const int rows = c.jobs()->rowCount();
    const QVariantMap refused = c.queueSequences({sequenceKey(longEdit->id), QStringLiteral("s999999")},
                                                 {{QStringLiteral("preset"), QStringLiteral("audio-wav")}});
    CHECK_FALSE(refused.value(QStringLiteral("ok")).toBool());
    CHECK_FALSE(refused.value(QStringLiteral("error")).toString().isEmpty());
    CHECK(c.jobs()->rowCount() == rows);
    const QVariantMap noFolder = c.queueSequences({sequenceKey(longEdit->id)},
                                                  {{QStringLiteral("preset"), QStringLiteral("audio-wav")},
                                                   {QStringLiteral("folder"), q(dir / "missing")}});
    CHECK_FALSE(noFolder.value(QStringLiteral("ok")).toBool());
    CHECK(c.jobs()->rowCount() == rows);
    CHECK_FALSE(c.queueSequences({}, {}).value(QStringLiteral("ok")).toBool());
    c.cancelAll();
    REQUIRE(waitFor([&] { return c.jobs()->activeCount() == 0; }));
    resetSettings();
}

// ---- #26: each range as its own file -----------------------------------------------------------------------------------

TEST_CASE("Export ranges: each ranged marker or selected clip becomes its own file", "[app][export][p1]") {
    resetSettings();
    const fs::path dir = freshDir("ranges");
    ProjectController project;
    project.resetProject(emptyProject(2.0));
    addMarker(project, 0, 12, "Ace", 1);
    addMarker(project, 15, 0, "point", 1);
    addMarker(project, 18, 12, "Clutch: 1/2", 2);
    addMarker(project, 36, 6, "", 1);
    ExportController c;
    c.setProject(&project);
    c.setVideoSourceFactory(ex::syntheticVideoSourceFactory());

    // The markers with a duration, in time order; a colour filter.
    const QVariantList all = c.markerRanges({}, -1);
    REQUIRE(all.size() == 3);
    CHECK(all[0].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Ace"));
    CHECK(all[2].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Marker 4"));
    CHECK(c.markerRanges({}, 1).size() == 2);
    CHECK(c.markerRanges({}, 2).size() == 1);

    // What it writes: "<sequence> - <name>", characters files refuse replaced, a name taken on disk numbered.
    { std::ofstream(dir / "Clip - Ace.wav", std::ios::binary) << "x"; }
    const QVariantMap folder{{QStringLiteral("folder"), q(dir)}};
    const QVariantMap plan = c.describeRanges({}, QStringLiteral("audio-wav"), all, folder);
    INFO(plan.value(QStringLiteral("error")).toString().toStdString());
    REQUIRE(plan.value(QStringLiteral("ok")).toBool());
    CHECK(plan.value(QStringLiteral("count")).toInt() == 3);
    CHECK(plan.value(QStringLiteral("outputs")).toStringList() ==
          QStringList({QStringLiteral("Clip - Ace (2).wav"), QStringLiteral("Clip - Clutch_ 1_2.wav"),
                       QStringLiteral("Clip - Marker 4.wav")}));
    CHECK(plan.value(QStringLiteral("bytes")).toDouble() > 0.0);
    CHECK_FALSE(plan.value(QStringLiteral("sizeText")).toString().isEmpty());

    const QStringList jobs = c.queueRanges({}, QStringLiteral("audio-wav"), all, folder);
    REQUIRE(jobs.size() == 3);
    for (const QString& job : jobs)
        REQUIRE(finished(c, job));
    CHECK(c.job(jobs[0]).value(QStringLiteral("state")).toString() == QStringLiteral("done"));
    CHECK(fs::exists(dir / "Clip - Ace (2).wav"));
    CHECK(fs::exists(dir / "Clip - Clutch_ 1_2.wav"));
    CHECK(fs::exists(dir / "Clip - Marker 4.wav"));
    CHECK(c.job(jobs[2]).value(QStringLiteral("detail")).toString().startsWith(QStringLiteral("0:00")));

    // An empty range refuses the whole batch: nothing is queued and the reason says which.
    const int rows = c.jobs()->rowCount();
    QVariantList withEmpty = all;
    withEmpty.append(QVariantMap{{QStringLiteral("in"), QVariant::fromValue<qlonglong>(Ticks::fromSeconds(1.0).value)},
                                 {QStringLiteral("out"), QVariant::fromValue<qlonglong>(Ticks::fromSeconds(1.0).value)},
                                 {QStringLiteral("name"), QStringLiteral("Nothing")}});
    CHECK(c.queueRanges({}, QStringLiteral("audio-wav"), withEmpty, folder).isEmpty());
    CHECK(c.lastError().contains(QStringLiteral("Nothing")));
    CHECK(c.jobs()->rowCount() == rows);
    CHECK_FALSE(c.describeRanges({}, QStringLiteral("audio-wav"), withEmpty, folder).value(QStringLiteral("ok")).toBool());

    // The selected clips: a linked video and audio clip is one range, named after the clip.
    Timeline tl(R"(
        V1 |Aaaa..Bbbb|
        A1 |Aaaa......|
    )");
    tl.linkMatching();
    project.resetProject(tl.state, tl.seq);
    project.selectAll();
    REQUIRE(waitFor([&] { return c.selectedClipRanges().size() == 2; }));
    const QVariantList clips = c.selectedClipRanges();
    REQUIRE(clips.size() == 2);
    CHECK(clips[0].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("A"));
    CHECK(clips[0].toMap().value(QStringLiteral("in")).toLongLong() == 0);
    CHECK(clips[0].toMap().value(QStringLiteral("out")).toLongLong() == f(4).value);
    CHECK(clips[1].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("B"));
    CHECK(clips[1].toMap().value(QStringLiteral("in")).toLongLong() == f(6).value);
    resetSettings();
}

// ---- #25: the media usage report --------------------------------------------------------------------------------------

TEST_CASE("Media usage report: the Export controller writes the CSV with a byte order mark", "[app][export][p1]") {
    resetSettings();
    const fs::path dir = freshDir("usage");
    ExportController c;
    CHECK_FALSE(c.mediaUsage({}).value(QStringLiteral("ok")).toBool()); // no project
    Timeline tl(R"(
        V1 |Aaaa..Bbbb|
        A1 |Aaaa......|
    )");
    tl.linkMatching();
    ProjectController project;
    project.resetProject(tl.state, tl.seq);
    c.setProject(&project);

    const QVariantMap report = c.mediaUsage({});
    REQUIRE(report.value(QStringLiteral("ok")).toBool());
    CHECK(report.value(QStringLiteral("uses")).toInt() == 2);
    CHECK(report.value(QStringLiteral("media")).toInt() == 2);
    CHECK(report.value(QStringLiteral("text")).toString().startsWith(QStringLiteral("Media Name,File Path,")));
    // In to Out without marks: the reason in plain words.
    const QVariantMap noMarks = c.mediaUsage({}, {{QStringLiteral("range"), QStringLiteral("inOut")}});
    CHECK_FALSE(noMarks.value(QStringLiteral("ok")).toBool());
    CHECK(noMarks.value(QStringLiteral("error")).toString() == QStringLiteral("Set an In and an Out point first."));

    const QVariantMap written = c.writeMediaUsage(QUrl::fromLocalFile(q(dir / "usage.csv")).toString(), {});
    INFO(written.value(QStringLiteral("error")).toString().toStdString());
    REQUIRE(written.value(QStringLiteral("ok")).toBool());
    QFile csv(q(dir / "usage.csv"));
    REQUIRE(csv.open(QIODevice::ReadOnly));
    const QByteArray bytes = csv.readAll();
    CHECK(bytes.startsWith("\xEF\xBB\xBF" "Media Name,"));
    const QVariantMap json = c.writeMediaUsage(q(dir / "usage.json"), {}, {{QStringLiteral("format"), QStringLiteral("json")}});
    REQUIRE(json.value(QStringLiteral("ok")).toBool());
    QFile jsonFile(q(dir / "usage.json"));
    REQUIRE(jsonFile.open(QIODevice::ReadOnly));
    CHECK(QJsonDocument::fromJson(jsonFile.readAll()).object().value(QStringLiteral("uses")).toArray().size() == 2);
    CHECK_FALSE(c.writeMediaUsage(QStringLiteral("relative.csv"), {}).value(QStringLiteral("ok")).toBool());
    resetSettings();
}

// ---- In the window ----------------------------------------------------------------------------------------------------

TEST_CASE("Export batch dialogs: Export Sequences Export Ranges and Media Usage Report; screenshots",
          "[app][export][qml][screenshot][p1]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            clearMessages();
            const fs::path dir = freshDir(dark ? "dialogs-dark" : "dialogs-light");
            A11yScope a11y;
            Shell shell;
            REQUIRE(shell.window);
            INFO(join(shell.errors));
            shell.resize(kShotWidth, kShotHeight);
            shell.theme->setProperty("reduceMotion", true);
            shell.theme->setProperty("dark", dark);
            auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
            auto* exporter = shell.engine.singletonInstance<ExportController*>("Wabcut", "ExportController");
            REQUIRE(controller);
            REQUIRE(exporter);
            exporter->setVideoSourceFactory(ex::syntheticVideoSourceFactory());
            exporter->setLastFolder(q(dir));
            ProjectState state;
            const SequencePtr longEdit = emptySequence(state, "Long edit", 4.0);
            const SequencePtr shortCut = emptySequence(state, "Short", 4.0, 360, 640);
            state.sequences = {longEdit, shortCut};
            controller->resetProject(state);
            addMarker(*controller, 15, 30, "Ace", 1);
            addMarker(*controller, 60, 30, "Clutch", 3);
            if (evaluate(shell, QStringLiteral("home.opened")).toBool())
                evaluate(shell, QStringLiteral("home.close()"));
            spin(200);
            // The start-up check ran in the test (no folder of its own: nothing kept, no toast).
            REQUIRE(waitFor([&] { return exporter->restoreQueue().value(QStringLiteral("count")).toInt() == 0; }));
            CHECK_FALSE(exporter->keepsQueue());

            // Export Sequences (Send to Export Queue… on two sequences): each shape's preset, waiting for Start Queue.
            QObject* sequencesDialog = shell.object("sequencesExportDialog");
            REQUIRE(sequencesDialog);
            emit exporter->sequencesExportRequested({sequenceKey(longEdit->id), sequenceKey(shortCut->id)}, true);
            REQUIRE(waitFor([&] { return sequencesDialog->property("visible").toBool(); }));
            CHECK(sequencesDialog->property("hold").toBool());
            CHECK(sequencesDialog->property("entries").toList().size() == 2);
            CHECK(sequencesDialog->property("allReady").toBool());
            QQuickItem* ok = itemNamed(shell, sequencesDialog, QStringLiteral("sequencesExportOk"));
            REQUIRE(ok);
            CHECK(ok->property("text").toString() == QStringLiteral("Queue 2 exports"));
            spin(300);
            QImage image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            CHECK(image.save(QDir(outputDir()).filePath(dark ? QStringLiteral("export-2c-sequences.png")
                                                             : QStringLiteral("export-2c-sequences-light.png"))));
            REQUIRE(QMetaObject::invokeMethod(ok, "clicked"));
            REQUIRE(waitFor([&] { return !sequencesDialog->property("visible").toBool(); }));
            CHECK(exporter->jobs()->activeCount() == 2);
            exporter->cancelAll();
            REQUIRE(waitFor([&] { return exporter->jobs()->activeCount() == 0; }, 60'000));
            exporter->clearFinished();

            // Export each marker range: two files, the size line, the colour filter.
            QObject* rangesDialog = shell.object("exportRangesDialog");
            REQUIRE(rangesDialog);
            emit exporter->rangesExportRequested(QStringLiteral("markers"));
            REQUIRE(waitFor([&] { return rangesDialog->property("visible").toBool(); }));
            CHECK(rangesDialog->property("ranges").toList().size() == 2);
            QQuickItem* summary = itemNamed(shell, rangesDialog, QStringLiteral("exportRangesSummary"));
            REQUIRE(summary);
            CHECK(summary->property("text").toString().startsWith(QStringLiteral("2 files · ≈ ")));
            rangesDialog->setProperty("colorIndex", 3);
            REQUIRE(waitFor([&] { return rangesDialog->property("ranges").toList().size() == 1; }));
            rangesDialog->setProperty("colorIndex", -1);
            spin(300);
            image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            CHECK(image.save(QDir(outputDir()).filePath(dark ? QStringLiteral("export-2c-ranges.png")
                                                             : QStringLiteral("export-2c-ranges-light.png"))));
            QMetaObject::invokeMethod(rangesDialog, "close");
            REQUIRE(waitFor([&] { return !rangesDialog->property("visible").toBool(); }));

            // The media usage report: nothing is used in this project (empty sequences): no uses, Save still works.
            QObject* usageDialog = shell.object("mediaUsageReportDialog");
            REQUIRE(usageDialog);
            REQUIRE(QMetaObject::invokeMethod(usageDialog, "openDialog"));
            REQUIRE(waitFor([&] { return usageDialog->property("visible").toBool(); }));
            QQuickItem* usageSummary = itemNamed(shell, usageDialog, QStringLiteral("mediaUsageSummary"));
            REQUIRE(usageSummary);
            CHECK(usageSummary->property("text").toString() == QStringLiteral("0 uses · 0 sources"));
            spin(300);
            image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            CHECK(image.save(QDir(outputDir()).filePath(dark ? QStringLiteral("export-2c-media-usage.png")
                                                             : QStringLiteral("export-2c-media-usage-light.png"))));
            REQUIRE(QMetaObject::invokeMethod(usageDialog, "save", Q_ARG(QUrl, QUrl::fromLocalFile(q(dir / "usage.csv")))));
            REQUIRE(waitFor([&] { return !usageDialog->property("visible").toBool(); }));
            CHECK(fs::exists(dir / "usage.csv"));

            // The restored-queue toast: Start Queue and Clear.
            evaluate(shell, QStringLiteral("announceRestoredQueue({ held: 3, interrupted: 1, count: 4, skipped: [] })"));
            QQuickItem* toast = shell.item("toast");
            REQUIRE(toast);
            CHECK(toast->property("text").toString() ==
                  QStringLiteral("3 exports from last time are waiting · 1 stopped when Wabcut closed (Retry in the queue)"));
            CHECK(toast->property("actionText").toString() == QStringLiteral("Start Queue"));
            CHECK(toast->property("secondaryActionText").toString() == QStringLiteral("Clear"));
            spin(300);
            image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            CHECK(image.save(QDir(outputDir()).filePath(dark ? QStringLiteral("export-2c-restored-toast.png")
                                                             : QStringLiteral("export-2c-restored-toast-light.png"))));

            // The quit question with a kept queue: its text and Keep Queue and Quit (not shown: keepsQueue is off).
            QObject* quit = shell.object("backgroundWorkCloseDialog");
            REQUIRE(quit);
            CHECK_FALSE(quit->property("keepsQueue").toBool());
            // The Library's rows for sequences exist (their menu is built when it opens).
            CHECK(evaluate(shell, QStringLiteral("typeof openSequencesExport")).toString() == QStringLiteral("function"));
            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
        }
    }
    resetSettings();
}
