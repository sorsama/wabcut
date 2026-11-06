// CloudBridge, review links (CloudBridge.h): the review copy, publishing, revoking and the comments that come back.

#include "CloudBridge.h"

#include "CloudBridgeInternal.h"
#include "ProjectController.h"

#include <wabcut/cloud/HttpStores.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportPresets.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ReviewComments.h>

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QLocale>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace wabcut::app {

namespace fs = std::filesystem;
using namespace cloud_detail;

namespace {

struct Resolution {
    int height;
    double mbps;
};

Resolution resolutionFor(int height) {
    if (height >= 1080)
        return {1080, 6.0};
    if (height >= 720)
        return {720, 3.0};
    return {540, 1.5};
}

struct Plan {
    bool ok = false;
    QString error;
    SequencePtr sequence;
    TimeRange range;
    int width = 0;
    int height = 0;
    double mbps = 0.0;
    double seconds = 0.0;
    std::uint64_t bytes = 0;
    QString rangeName;
};

Plan planFor(ProjectController& controller, const QVariantMap& options) {
    Plan plan;
    plan.sequence = controller.currentSequence();
    if (!plan.sequence) {
        plan.error = CloudBridge::tr("Open a sequence first.");
        return plan;
    }
    const Resolution resolution = resolutionFor(options.value(QStringLiteral("resolution"), 720).toInt());
    plan.rangeName = options.value(QStringLiteral("range"), QStringLiteral("whole")).toString();
    if (plan.rangeName == QLatin1String("inOut")) {
        const auto in = controller.markIn();
        const auto out = controller.markOut();
        if (!in || !out || !(*out > *in)) {
            plan.error = CloudBridge::tr("Set an In and an Out point first.");
            return plan;
        }
        plan.range = TimeRange{*in, *out - *in};
    } else if (plan.rangeName == QLatin1String("workArea")) {
        if (!plan.sequence->workArea) {
            plan.error = CloudBridge::tr("The sequence has no work area.");
            return plan;
        }
        plan.range = *plan.sequence->workArea;
    } else {
        plan.rangeName = QStringLiteral("whole");
        plan.range = TimeRange{Ticks{}, plan.sequence->duration};
    }
    if (plan.range.duration.value <= 0) {
        plan.error = CloudBridge::tr("The sequence is empty.");
        return plan;
    }
    const int sourceWidth = std::max(1, plan.sequence->width);
    const int sourceHeight = std::max(1, plan.sequence->height);
    plan.height = std::min(resolution.height, sourceHeight);
    plan.width = std::max(16, static_cast<int>(std::lround(static_cast<double>(sourceWidth) * plan.height / sourceHeight / 2.0)) * 2);
    plan.height = std::max(16, plan.height / 2 * 2);
    plan.mbps = resolution.mbps;
    plan.seconds = plan.range.duration.toSeconds();
    plan.bytes = static_cast<std::uint64_t>((plan.mbps * 1e6 + 128000.0) / 8.0 * plan.seconds * 1.03);
    plan.ok = true;
    return plan;
}

QString durationText(double seconds) {
    const int total = static_cast<int>(std::lround(seconds));
    return total >= 3600 ? QStringLiteral("%1:%2:%3").arg(total / 3600).arg(total / 60 % 60, 2, 10, QLatin1Char('0')).arg(total % 60, 2, 10, QLatin1Char('0'))
                         : QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

QVariantMap CloudBridge::estimateReviewCopy(const QVariantMap& options) {
    QVariantMap out{{QStringLiteral("ok"), false}};
    if (!m_controller) {
        out.insert(QStringLiteral("error"), tr("Open a project first."));
        return out;
    }
    const Plan plan = planFor(*m_controller, options);
    if (!plan.ok) {
        out.insert(QStringLiteral("error"), plan.error);
        return out;
    }
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("error"), QString());
    out.insert(QStringLiteral("sequenceName"), qs(plan.sequence->name));
    out.insert(QStringLiteral("seconds"), plan.seconds);
    out.insert(QStringLiteral("durationText"), durationText(plan.seconds));
    out.insert(QStringLiteral("resolution"), QStringLiteral("%1 × %2").arg(plan.width).arg(plan.height));
    out.insert(QStringLiteral("bytes"), static_cast<qulonglong>(plan.bytes));
    out.insert(QStringLiteral("sizeText"), tr("about %1").arg(QLocale().formattedDataSize(static_cast<qint64>(plan.bytes))));
    out.insert(QStringLiteral("range"), plan.rangeName);
    return out;
}

void CloudBridge::discardReviewCopy() {
    if (m_worker.running())
        return;
    std::error_code ec;
    if (!d->prepared.folder.empty())
        fs::remove_all(d->prepared.folder, ec);
    d->prepared = {};
    m_reviewCopy = QVariantMap{{QStringLiteral("state"), QStringLiteral("none")}};
    emit reviewCopyChanged();
}

void CloudBridge::prepareReviewCopy(const QVariantMap& options) {
    if (!m_controller) {
        setMessage(tr("Open a project first."), true);
        return;
    }
    const Plan plan = planFor(*m_controller, options);
    if (!plan.ok) {
        setMessage(plan.error, true);
        return;
    }
    if (m_worker.running()) {
        setMessage(tr("Something is still running. Wait for it to finish or cancel it."), true);
        return;
    }
    discardReviewCopy();
    const fs::path folder = cacheFolder() / "review" / cloud::newToken(m_random, 8);
    const fs::path output = folder / "review.mp4";
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        setMessage(tr("The review copy cannot be written: the cache folder is not usable."), true);
        return;
    }
    const ProjectSnapshot snapshot = m_controller->snapshot();
    const fs::path file = projectFile();
    const std::string contentHash = cloud::sha256Hex(io::renderKeyJson(*snapshot, plan.sequence->id, plan.range));
    const QString resolutionText = QStringLiteral("%1 × %2").arg(plan.width).arg(plan.height);
    m_reviewCopy = QVariantMap{{QStringLiteral("state"), QStringLiteral("rendering")}, {QStringLiteral("resolution"), resolutionText},
                               {QStringLiteral("sequenceName"), qs(plan.sequence->name)}, {QStringLiteral("estimateText"),
                                tr("about %1").arg(QLocale().formattedDataSize(static_cast<qint64>(plan.bytes)))}};
    emit reviewCopyChanged();
    const QString phase = tr("Rendering the review copy");
    const bool started = start(phase, [=, this](std::stop_token stop) -> CloudWorker::Finish {
        std::expected<std::int64_t, Error> rendered;
        if (m_testRender) {
            rendered = m_testRender(output);
        } else {
            std::optional<exporter::Preset> preset = exporter::findPreset("match-source");
            if (!preset)
                return [this] { setMessage(tr("The export engine has no usable preset."), true); discardReviewCopy(); };
            exporter::ExportSettings settings = preset->settings;
            settings.video.width = plan.width;
            settings.video.height = plan.height;
            settings.video.rateControl = exporter::RateControl::Bitrate;
            settings.video.bitrate = static_cast<decltype(settings.video.bitrate)>(plan.mbps * 1e6);
            settings.video.maxBitrate = static_cast<decltype(settings.video.maxBitrate)>(plan.mbps * 1.5e6);
            settings.audio.bitrate = static_cast<decltype(settings.audio.bitrate)>(128000);
            settings.range = exporter::RangeMode::Custom;
            settings.customRange = plan.range;
            exporter::ExportRequest request;
            request.snapshot = snapshot;
            request.sequence = plan.sequence->id;
            request.settings = std::move(settings);
            request.output = output;
            request.projectFile = file;
            request.videoSource = m_videoSource;
            const cloud::Progress sink = progressSink(phase);
            request.onProgress = [sink](const exporter::ExportProgress& p) { (void)sink(static_cast<std::uint64_t>(p.framesDone), static_cast<std::uint64_t>(p.framesTotal)); };
            exporter::ExportJob job(std::move(request));
            auto result = job.run(stop);
            if (result)
                rendered = result->bytes;
            else
                rendered = std::unexpected(result.error());
        }
        return [=, this, rendered = std::move(rendered)] {
            std::error_code removeEc;
            if (!rendered) {
                fs::remove_all(folder, removeEc);
                m_reviewCopy = QVariantMap{{QStringLiteral("state"), QStringLiteral("none")}};
                emit reviewCopyChanged();
                setMessage(rendered.error().code == ErrorCode::Cancelled ? tr("Cancelled. Nothing was kept.")
                                                                          : tr("The review copy could not be made: %1").arg(qs(rendered.error().message)),
                           rendered.error().code != ErrorCode::Cancelled);
                return;
            }
            const std::uint64_t bytes = fs::is_regular_file(output, removeEc) ? fs::file_size(output, removeEc) : 0;
            Impl::Prepared& p = d->prepared;
            p = {};
            p.ready = bytes > 0;
            p.folder = folder;
            p.video = output;
            p.bytes = bytes;
            p.sequence = plan.sequence->id.value;
            p.sequenceName = plan.sequence->name;
            p.startTicks = plan.range.start.value;
            p.durationTicks = plan.range.duration.value;
            p.rate = plan.sequence->frameRate;
            p.dropFrame = plan.sequence->dropFrame;
            p.frames = ticksToFrames(plan.range.duration, plan.sequence->frameRate);
            p.contentHash = contentHash;
            p.resolution = resolutionText;
            p.seconds = plan.seconds;
            if (!p.ready) {
                fs::remove_all(folder, removeEc);
                m_reviewCopy = QVariantMap{{QStringLiteral("state"), QStringLiteral("none")}};
                emit reviewCopyChanged();
                setMessage(tr("The review copy came out empty."), true);
                return;
            }
            m_reviewCopy = QVariantMap{{QStringLiteral("state"), QStringLiteral("ready")},
                                       {QStringLiteral("path"), qs(pathToUtf8(output))},
                                       {QStringLiteral("bytes"), static_cast<qulonglong>(bytes)},
                                       {QStringLiteral("sizeText"), QLocale().formattedDataSize(static_cast<qint64>(bytes))},
                                       {QStringLiteral("seconds"), plan.seconds},
                                       {QStringLiteral("durationText"), durationText(plan.seconds)},
                                       {QStringLiteral("resolution"), resolutionText},
                                       {QStringLiteral("sequenceName"), qs(plan.sequence->name)}};
            emit reviewCopyChanged();
            setMessage(tr("The review copy is ready (%1). Nothing has been sent yet.").arg(QLocale().formattedDataSize(static_cast<qint64>(bytes))));
        };
    });
    if (!started) {
        std::error_code removeEc;
        fs::remove_all(folder, removeEc);
        m_reviewCopy = QVariantMap{{QStringLiteral("state"), QStringLiteral("none")}};
        emit reviewCopyChanged();
    }
}

void CloudBridge::rebuildLinkRows() {
    QVariantList rows;
    const std::int64_t now = m_clock();
    ProjectSnapshot snapshot = m_controller ? m_controller->snapshot() : nullptr;
    for (const cloud::ReviewLinkRecord& r : d->links) {
        const Location* l = findLocation(qs(r.location));
        const bool expired = r.expiresAt > 0 && now > r.expiresAt;
        QString expires = tr("Never expires");
        if (r.expiresAt > 0) {
            const QString day = QLocale().toString(QDateTime::fromSecsSinceEpoch(r.expiresAt).date(), QLocale::ShortFormat);
            expires = expired ? tr("Expired %1").arg(day) : tr("Expires %1").arg(day);
        }
        bool changed = false;
        QString sequenceName;
        if (snapshot) {
            if (const Sequence* seq = snapshot->findSequence(SequenceId{r.sequence})) {
                sequenceName = qs(seq->name);
                const TimeRange range{Ticks{r.startTicks}, Ticks{r.durationTicks}};
                changed = !r.contentHash.empty() && cloud::sha256Hex(io::renderKeyJson(*snapshot, seq->id, range)) != r.contentHash;
            }
        }
        rows.append(QVariantMap{{QStringLiteral("id"), qs(r.linkId)},
                                {QStringLiteral("title"), qs(r.title)},
                                {QStringLiteral("createdText"), whenText(r.created)},
                                {QStringLiteral("expiresText"), expires},
                                {QStringLiteral("expired"), expired},
                                {QStringLiteral("locationName"), l ? l->name : tr("(removed)")},
                                {QStringLiteral("kind"), qs(r.kind)},
                                {QStringLiteral("sizeText"), QLocale().formattedDataSize(static_cast<qint64>(r.bytes))},
                                {QStringLiteral("url"), qs(r.url)},
                                {QStringLiteral("canCopyLink"), !r.url.empty()},
                                {QStringLiteral("sequenceName"), sequenceName},
                                {QStringLiteral("changed"), changed}});
    }
    m_linkRows = rows;
    emit linksChanged();
}

cloud::ReviewLinkRecord* CloudBridge::findLink(const QString& id) {
    for (cloud::ReviewLinkRecord& r : d->links) {
        if (qs(r.linkId) == id)
            return &r;
    }
    return nullptr;
}

void CloudBridge::publishReviewLink(const QVariantMap& options) {
    Impl::Prepared& prep = d->prepared;
    if (!prep.ready) {
        setMessage(tr("Make the review copy first."), true);
        return;
    }
    if (const QString blocked = blockedReason(); !blocked.isEmpty()) {
        setMessage(blocked, true);
        return;
    }
    const QString locationId = options.value(QStringLiteral("locationId")).toString();
    const Location* l = findLocation(locationId);
    if (!l || l->purpose != QLatin1String("reviews")) {
        setMessage(tr("Choose a location for review links."), true);
        return;
    }
    if (!projectSaved()) {
        setMessage(tr("Save the project first, so this computer can remember the link."), true);
        return;
    }
    auto opened = openStore(*l);
    if (!opened) {
        setMessage(opened.error(), true);
        return;
    }
    std::shared_ptr<cloud::RemoteStore> store = std::move(*opened);
    cloud::PublishRequest request;
    request.video = prep.video;
    request.stagingFolder = prep.folder;
    request.page.title = s(options.value(QStringLiteral("title")).toString().trimmed().left(120));
    if (request.page.title.empty())
        request.page.title = prep.sequenceName;
    request.page.instructions = s(options.value(QStringLiteral("instructions")).toString().trimmed().left(400));
    request.page.frameRate = prep.rate;
    request.page.dropFrame = prep.dropFrame;
    request.page.durationFrames = prep.frames;
    request.location = s(locationId);
    request.title = request.page.title;
    const int days = std::clamp(options.value(QStringLiteral("expiryDays"), 7).toInt(), 0, 365);
    request.expiresAt = days > 0 ? m_clock() + static_cast<std::int64_t>(days) * 86400 : 0;
    request.removeWhenExpired = options.value(QStringLiteral("removeWhenExpired"), true).toBool();
    request.sequence = prep.sequence;
    request.startTicks = prep.startTicks;
    request.durationTicks = prep.durationTicks;
    request.contentHash = prep.contentHash;
    if (auto* s3 = dynamic_cast<cloud::S3Store*>(store.get()))
        request.presign = [s3](std::string_view path, int seconds) { return s3->presignGet(path, seconds); };
    const fs::path file = projectFile();
    const QString phase = tr("Uploading the review copy");
    start(phase, [=, this, request = std::move(request)](std::stop_token stop) mutable -> CloudWorker::Finish {
        auto published = cloud::publishReviewLink(*store, request, m_random, m_clock, progressSink(phase), stop);
        return [=, this, published = std::move(published)]() mutable {
            if (!published) {
                setMessage(published.error().kind == cloud::StoreFailure::Cancelled ? tr("Cancelled. Nothing was kept on the storage.")
                                                                                       : tr("The review link could not be published: %1").arg(qs(published.error().message)),
                           published.error().kind != cloud::StoreFailure::Cancelled);
                return;
            }
            d->links.push_back(published->record);
            (void)cloud::writeReviewLinks(file, d->links);
            std::error_code ec;
            if (!d->prepared.folder.empty())
                fs::remove_all(d->prepared.folder, ec);
            d->prepared = {};
            m_reviewCopy = QVariantMap{{QStringLiteral("state"), QStringLiteral("none")}};
            emit reviewCopyChanged();
            rebuildLinkRows();
            const bool signedLink = !published->record.url.empty();
            setMessage(signedLink ? tr("Published. Copy the link and send it; it stops working on its own and can be revoked here.")
                                  : tr("Published to the folder. Share that folder with your reviewers using your sync service; it can be revoked here."));
        };
    });
}

void CloudBridge::revokeLink(const QString& id) {
    cloud::ReviewLinkRecord* record = findLink(id);
    if (!record)
        return;
    const Location* l = findLocation(qs(record->location));
    if (!l) {
        // the location is gone: nothing can be deleted from here; the record is dropped so it does not stay forever
        d->links.erase(std::remove_if(d->links.begin(), d->links.end(), [&](const cloud::ReviewLinkRecord& r) { return qs(r.linkId) == id; }), d->links.end());
        (void)cloud::writeReviewLinks(projectFile(), d->links);
        rebuildLinkRows();
        setMessage(tr("The location of this link was removed; the record is gone. Delete the folder on the storage yourself."), true);
        return;
    }
    auto opened = openStore(*l);
    if (!opened) {
        setMessage(opened.error(), true);
        return;
    }
    std::shared_ptr<cloud::RemoteStore> store = std::move(*opened);
    const cloud::ReviewLinkRecord copy = *record;
    start(tr("Revoking the review link"), [=, this](std::stop_token stop) -> CloudWorker::Finish {
        auto removed = cloud::revokeReviewLink(*store, copy, stop);
        return [=, this] {
            if (!removed) {
                setMessage(tr("The link could not be revoked: %1").arg(qs(removed.error().message)), true);
                return;
            }
            d->links.erase(std::remove_if(d->links.begin(), d->links.end(), [&](const cloud::ReviewLinkRecord& r) { return r.linkId == copy.linkId; }), d->links.end());
            (void)cloud::writeReviewLinks(projectFile(), d->links);
            rebuildLinkRows();
            setMessage(tr("The review link is revoked: its page, its video and the comments left there were deleted from the storage."));
        };
    });
}

void CloudBridge::removeExpiredLinks() {
    std::vector<cloud::ReviewLinkRecord> expired;
    const std::int64_t now = m_clock();
    for (const cloud::ReviewLinkRecord& r : d->links) {
        if (r.expiresAt > 0 && now > r.expiresAt)
            expired.push_back(r);
    }
    if (expired.empty()) {
        setMessage(tr("No review link has expired."));
        return;
    }
    struct Target {
        cloud::ReviewLinkRecord record;
        std::shared_ptr<cloud::RemoteStore> store;
    };
    std::vector<Target> targets;
    for (const cloud::ReviewLinkRecord& r : expired) {
        const Location* l = findLocation(qs(r.location));
        std::shared_ptr<cloud::RemoteStore> store;
        if (l) {
            if (auto opened = openStore(*l))
                store = std::move(*opened);
        }
        targets.push_back({r, store});
    }
    start(tr("Removing expired review links"), [=, this](std::stop_token stop) -> CloudWorker::Finish {
        std::vector<std::string> done;
        QStringList failures;
        for (const Target& t : targets) {
            if (!t.store) {
                done.push_back(t.record.linkId); // nothing reachable to delete from; forget the record
                continue;
            }
            if (auto removed = cloud::revokeReviewLink(*t.store, t.record, stop))
                done.push_back(t.record.linkId);
            else
                failures << qs(removed.error().message);
        }
        return [=, this] {
            d->links.erase(std::remove_if(d->links.begin(), d->links.end(),
                                          [&](const cloud::ReviewLinkRecord& r) { return std::find(done.begin(), done.end(), r.linkId) != done.end(); }),
                           d->links.end());
            (void)cloud::writeReviewLinks(projectFile(), d->links);
            rebuildLinkRows();
            if (failures.isEmpty())
                setMessage(tr("Removed %n expired review link(s) from the storage.", nullptr, static_cast<int>(done.size())));
            else
                setMessage(tr("Some expired links could not be removed: %1").arg(failures.join(QLatin1Char(' '))), true);
        };
    });
}

bool CloudBridge::copyLink(const QString& id) {
    const cloud::ReviewLinkRecord* record = findLink(id);
    if (!record || record->url.empty())
        return false;
    if (auto* clipboard = QGuiApplication::clipboard())
        clipboard->setText(qs(record->url));
    return true;
}

QString CloudBridge::linkFolder(const QString& id) const {
    for (const cloud::ReviewLinkRecord& r : d->links) {
        if (qs(r.linkId) != id)
            continue;
        const Location* l = findLocation(qs(r.location));
        if (l && l->kind == QLatin1String("folder"))
            return QDir::toNativeSeparators(QDir(l->folder).filePath(QStringLiteral("reviews/") + qs(r.token)));
    }
    return {};
}

// ---- the comments that come back ----

void CloudBridge::collectComments(const QString& id, bool markers) {
    cloud::ReviewLinkRecord* record = findLink(id);
    if (!record)
        return;
    const Location* l = findLocation(qs(record->location));
    if (!l) {
        setMessage(tr("The location of this link was removed."), true);
        return;
    }
    auto opened = openStore(*l);
    if (!opened) {
        setMessage(opened.error(), true);
        return;
    }
    std::shared_ptr<cloud::RemoteStore> store = std::move(*opened);
    const cloud::ReviewLinkRecord copy = *record;
    start(tr("Reading the comments"), [=, this](std::stop_token stop) -> CloudWorker::Finish {
        auto collected = cloud::collectComments(*store, copy, stop);
        return [=, this, collected = std::move(collected)]() mutable {
            if (!collected) {
                setMessage(tr("The comments could not be read: %1").arg(qs(collected.error().message)), true);
                return;
            }
            std::vector<std::pair<QString, std::string>> files;
            for (cloud::CommentFile& f : collected->files)
                files.emplace_back(qs(f.name), std::move(f.text));
            importComments(files, copy, markers);
            if (collected->skipped > 0) {
                QVariantMap summary = m_lastImport;
                summary.insert(QStringLiteral("skipped"), static_cast<int>(collected->skipped));
                m_lastImport = summary;
                emit importChanged();
            }
        };
    });
}

void CloudBridge::importCommentsFile(const QString& id, const QUrl& fileUrl, bool markers) {
    const cloud::ReviewLinkRecord* record = findLink(id);
    if (!record)
        return;
    const QString path = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileUrl.toString();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setMessage(tr("The file cannot be read."), true);
        return;
    }
    if (file.size() > static_cast<qint64>(kMaxReviewCommentsBytes)) {
        setMessage(tr("That file is too large to be a comments file."), true);
        return;
    }
    const QByteArray bytes = file.readAll();
    importComments({{QFileInfo(path).fileName(), std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()))}}, *record, markers);
}

void CloudBridge::importComments(const std::vector<std::pair<QString, std::string>>& files, const cloud::ReviewLinkRecord& record, bool markers) {
    if (!m_controller)
        return;
    QStringList conflicts;
    QStringList reviewers;
    std::vector<ReviewCommentsFile> parsed;
    int skipped = 0;
    for (const auto& [name, text] : files) {
        auto file = parseReviewComments(text);
        if (!file) {
            conflicts << QStringLiteral("%1: %2").arg(name, qs(file.error().message));
            ++skipped;
            continue;
        }
        if (file->link != record.linkId) {
            conflicts << tr("%1: this file belongs to another review link.").arg(name);
            ++skipped;
            continue;
        }
        for (const std::string& s : file->skipped)
            conflicts << QStringLiteral("%1: %2").arg(name, qs(s));
        if (!reviewers.contains(qs(file->reviewer)))
            reviewers << qs(file->reviewer);
        parsed.push_back(std::move(*file));
    }
    QStringList warnings;
    if (const ProjectSnapshot snapshot = m_controller->snapshot()) {
        if (const Sequence* seq = snapshot->findSequence(SequenceId{record.sequence})) {
            const TimeRange range{Ticks{record.startTicks}, Ticks{record.durationTicks}};
            if (!record.contentHash.empty() && cloud::sha256Hex(io::renderKeyJson(*snapshot, seq->id, range)) != record.contentHash)
                warnings << tr("The cut has changed since this review copy was made, so comments may sit a little off.");
        } else {
            warnings << tr("The sequence this link was made from is no longer in the project.");
        }
    }
    ReviewLinkTarget target;
    target.link = record.linkId;
    target.sequence = SequenceId{record.sequence};
    target.start = Ticks{record.startTicks};
    target.duration = Ticks{record.durationTicks};
    const std::string now = cloud::formatIsoUtc(m_clock());
    std::size_t added = 0;
    std::size_t unchanged = 0;
    std::size_t madeMarkers = 0;
    if (!parsed.empty()) {
        auto applied = m_controller->session().apply("Import Review Comments", [&](const ProjectState& state) -> std::expected<ProjectState, Error> {
            ProjectState current = state;
            added = unchanged = madeMarkers = 0;
            for (const ReviewCommentsFile& file : parsed) {
                auto r = edit::importReviewComments(current, file, target, markers, now);
                if (!r) {
                    conflicts << qs(r.error().message);
                    continue;
                }
                current = std::move(r->state);
                added += r->added;
                unchanged += r->unchanged;
                madeMarkers += r->markers;
                for (const std::string& c : r->conflicts)
                    conflicts << qs(c);
            }
            return current;
        });
        if (!applied) {
            setMessage(tr("The comments could not be added: %1").arg(qs(applied.error().message)), true);
            return;
        }
    }
    m_lastImport = QVariantMap{{QStringLiteral("files"), static_cast<int>(files.size())},
                               {QStringLiteral("added"), static_cast<int>(added)},
                               {QStringLiteral("unchanged"), static_cast<int>(unchanged)},
                               {QStringLiteral("markers"), static_cast<int>(madeMarkers)},
                               {QStringLiteral("conflicts"), conflicts},
                               {QStringLiteral("warnings"), warnings},
                               {QStringLiteral("skipped"), skipped},
                               {QStringLiteral("reviewers"), reviewers}};
    emit importChanged();
    if (files.empty())
        setMessage(tr("No comments have been left for this link yet."));
    else if (added == 0 && unchanged == 0 && !conflicts.isEmpty())
        setMessage(tr("No comments could be added: %1").arg(conflicts.front()), true);
    else
        setMessage(tr("Added %n comment(s) as review notes.", nullptr, static_cast<int>(added)) +
                   (unchanged > 0 ? QLatin1Char(' ') + tr("%n already there.", nullptr, static_cast<int>(unchanged)) : QString()) +
                   (madeMarkers > 0 ? QLatin1Char(' ') + tr("%n marker(s) made.", nullptr, static_cast<int>(madeMarkers)) : QString()));
}

} // namespace wabcut::app
