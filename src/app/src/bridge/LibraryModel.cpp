#include "LibraryModel.h"

#include "BridgeIds.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Timecode.h>

#include <QDateTime>
#include <QDir>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

namespace {

constexpr std::array<LibraryColumnInfo, kLibraryColumnCount> kColumns{{
    {LibraryColumn::Name, "name", 220, true, true},
    {LibraryColumn::Label, "label", 96, true, false},
    {LibraryColumn::MediaType, "type", 96, true, false},
    {LibraryColumn::Duration, "duration", 104, true, false},
    {LibraryColumn::FrameRate, "fps", 84, true, false},
    {LibraryColumn::Resolution, "resolution", 108, true, false},
    {LibraryColumn::Codec, "codec", 112, true, false},
    {LibraryColumn::AudioChannels, "audio", 112, true, false},
    {LibraryColumn::MediaIn, "in", 104, false, false},
    {LibraryColumn::MediaOut, "out", 104, false, false},
    {LibraryColumn::Proxy, "proxy", 104, true, false},
    {LibraryColumn::Analysis, "analysis", 156, true, false},
    {LibraryColumn::Generated, "generated", 120, true, false},
    {LibraryColumn::Date, "date", 136, true, false},
    {LibraryColumn::Path, "path", 260, false, false},
    {LibraryColumn::Comment, "comment", 180, false, true},
    {LibraryColumn::Description, "description", 200, false, true},
    {LibraryColumn::Scene, "scene", 96, false, true},
    {LibraryColumn::Shot, "shot", 84, false, true},
    {LibraryColumn::Take, "take", 72, false, true},
    {LibraryColumn::LogNote, "logNote", 200, false, true},
    {LibraryColumn::Good, "good", 64, false, true},
    {LibraryColumn::Client, "client", 120, false, true},
    {LibraryColumn::TapeName, "tapeName", 112, false, true},
    {LibraryColumn::Status, "status", 104, false, true},
    {LibraryColumn::CameraAngle, "cameraAngle", 104, false, true},
    {LibraryColumn::CameraLabel, "cameraLabel", 104, false, true},
    {LibraryColumn::CameraModel, "cameraModel", 120, false, true},
    {LibraryColumn::ShotDate, "shotDate", 120, false, true},
    {LibraryColumn::ShotLocation, "shotLocation", 140, false, true},
}};

QString text(const std::string& s) { return QString::fromStdString(s); }

/// The display rate of an item's timecode: its video's rate, else 30 fps (Premiere shows audio in video timecode).
Rate displayRate(const MediaItem& media) {
    if (media.info.hasVideo() && media.info.video.front().frameRate.isValid() && !media.info.isStillImage)
        return media.info.video.front().frameRate;
    return kRate30;
}

QString timecodeText(Ticks t, Rate rate) { return text(ticksToTimecode(t, rate, false)); }

} // namespace

QString libraryLabelName(int index) {
    static const char* const kNames[] = {
        QT_TRANSLATE_NOOP("wabcut::app::LibraryModel", "Violet"),   QT_TRANSLATE_NOOP("wabcut::app::LibraryModel", "Iris"),
        QT_TRANSLATE_NOOP("wabcut::app::LibraryModel", "Caribbean"), QT_TRANSLATE_NOOP("wabcut::app::LibraryModel", "Lavender"),
        QT_TRANSLATE_NOOP("wabcut::app::LibraryModel", "Cerulean"), QT_TRANSLATE_NOOP("wabcut::app::LibraryModel", "Forest"),
        QT_TRANSLATE_NOOP("wabcut::app::LibraryModel", "Rose"),     QT_TRANSLATE_NOOP("wabcut::app::LibraryModel", "Mango"),
        QT_TRANSLATE_NOOP("wabcut::app::LibraryModel", "Purple"),   QT_TRANSLATE_NOOP("wabcut::app::LibraryModel", "Blue"),
        QT_TRANSLATE_NOOP("wabcut::app::LibraryModel", "Teal"),     QT_TRANSLATE_NOOP("wabcut::app::LibraryModel", "Magenta"),
        QT_TRANSLATE_NOOP("wabcut::app::LibraryModel", "Tan"),      QT_TRANSLATE_NOOP("wabcut::app::LibraryModel", "Green"),
        QT_TRANSLATE_NOOP("wabcut::app::LibraryModel", "Brown"),    QT_TRANSLATE_NOOP("wabcut::app::LibraryModel", "Yellow"),
    };
    if (index < 0 || index >= 16)
        return {};
    return LibraryModel::tr(kNames[index]);
}

namespace {

QString dateText(std::int64_t seconds) {
    if (seconds <= 0)
        return {};
    return QDateTime::fromSecsSinceEpoch(seconds).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString dayText(std::int64_t seconds) {
    if (seconds <= 0)
        return {};
    return QDateTime::fromSecsSinceEpoch(seconds).toString(QStringLiteral("yyyy-MM-dd"));
}

/// The usable length of an item: a subclip's range, else its container duration.
Ticks itemDuration(const MediaItem& media) {
    if (media.subclip)
        return media.subclip->range.duration;
    return media.info.duration;
}

QString proxyText(const LibraryMediaStatus& s) {
    if (s.proxyState == QStringLiteral("ready"))
        return LibraryModel::tr("Ready");
    if (s.proxyState == QStringLiteral("generating"))
        return LibraryModel::tr("Making %1 %").arg(std::lround(s.proxyProgress * 100.0));
    if (s.proxyState == QStringLiteral("queued"))
        return LibraryModel::tr("Queued");
    if (s.proxyState == QStringLiteral("paused"))
        return LibraryModel::tr("Paused");
    if (s.proxyState == QStringLiteral("stale"))
        return LibraryModel::tr("Out of date");
    if (s.proxyState == QStringLiteral("failed"))
        return LibraryModel::tr("Failed");
    return {};
}

QString analysisText(const LibraryMediaStatus& s) {
    QStringList parts;
    if (s.transcribed)
        parts << LibraryModel::tr("Transcribed");
    if (s.indexed)
        parts << LibraryModel::tr("Indexed");
    if (s.shots)
        parts << LibraryModel::tr("Shots");
    return parts.join(QStringLiteral(" · "));
}

} // namespace

const std::array<LibraryColumnInfo, kLibraryColumnCount>& libraryColumns() { return kColumns; }

std::optional<LibraryColumn> libraryColumnFromId(const QString& id) {
    for (const LibraryColumnInfo& info : kColumns) {
        if (id == QLatin1String(info.id))
            return info.column;
    }
    return std::nullopt;
}

QString libraryColumnTitle(LibraryColumn column) {
    switch (column) {
    case LibraryColumn::Name:
        return LibraryModel::tr("Name");
    case LibraryColumn::Label:
        return LibraryModel::tr("Label");
    case LibraryColumn::MediaType:
        return LibraryModel::tr("Type");
    case LibraryColumn::Duration:
        return LibraryModel::tr("Duration");
    case LibraryColumn::FrameRate:
        return LibraryModel::tr("Frame rate");
    case LibraryColumn::Resolution:
        return LibraryModel::tr("Resolution");
    case LibraryColumn::Codec:
        return LibraryModel::tr("Codec");
    case LibraryColumn::AudioChannels:
        return LibraryModel::tr("Audio");
    case LibraryColumn::MediaIn:
        return LibraryModel::tr("Media In");
    case LibraryColumn::MediaOut:
        return LibraryModel::tr("Media Out");
    case LibraryColumn::Proxy:
        return LibraryModel::tr("Proxy");
    case LibraryColumn::Analysis:
        return LibraryModel::tr("Analysis");
    case LibraryColumn::Generated:
        return LibraryModel::tr("Generated");
    case LibraryColumn::Date:
        return LibraryModel::tr("Date modified");
    case LibraryColumn::Path:
        return LibraryModel::tr("File path");
    case LibraryColumn::Comment:
        return LibraryModel::tr("Comment");
    case LibraryColumn::Description:
        return LibraryModel::tr("Description");
    case LibraryColumn::Scene:
        return LibraryModel::tr("Scene");
    case LibraryColumn::Shot:
        return LibraryModel::tr("Shot");
    case LibraryColumn::Take:
        return LibraryModel::tr("Take");
    case LibraryColumn::LogNote:
        return LibraryModel::tr("Log Note");
    case LibraryColumn::Good:
        return LibraryModel::tr("Good");
    case LibraryColumn::Client:
        return LibraryModel::tr("Client");
    case LibraryColumn::TapeName:
        return LibraryModel::tr("Tape Name");
    case LibraryColumn::Status:
        return LibraryModel::tr("Status");
    case LibraryColumn::CameraAngle:
        return LibraryModel::tr("Camera Angle");
    case LibraryColumn::CameraLabel:
        return LibraryModel::tr("Camera Label");
    case LibraryColumn::CameraModel:
        return LibraryModel::tr("Camera Model");
    case LibraryColumn::ShotDate:
        return LibraryModel::tr("Shot Date");
    case LibraryColumn::ShotLocation:
        return LibraryModel::tr("Shot Location");
    }
    return {};
}

bool LibraryFilter::active() const {
    return !text.trimmed().isEmpty() || !type.isEmpty() || label >= 0 || transcribed || offline || proxy || generated ||
           unused || duplicates || !from.isEmpty() || !to.isEmpty() || good;
}

LibraryFilter LibraryFilter::fromSearch(const BinSearch& s) {
    LibraryFilter f;
    f.text = QString::fromStdString(s.text);
    f.field = QString::fromStdString(s.field);
    f.type = QString::fromStdString(s.type);
    f.label = s.label;
    f.good = s.good;
    f.offline = s.offline;
    f.generated = s.generated;
    f.transcribed = s.transcribed;
    f.proxy = s.proxy;
    f.unused = s.unused;
    f.duplicates = s.duplicates;
    f.from = QString::fromStdString(s.from);
    f.to = QString::fromStdString(s.to);
    return f;
}

BinSearch LibraryFilter::toSearch() const {
    BinSearch s;
    s.text = text.trimmed().toStdString();
    s.field = field.toStdString();
    s.type = type == QStringLiteral("bin") ? std::string() : type.toStdString(); // a search bin finds no bins by type
    s.label = label;
    s.good = good;
    s.offline = offline;
    s.generated = generated;
    s.transcribed = transcribed;
    s.proxy = proxy;
    s.unused = unused;
    s.duplicates = duplicates;
    s.from = from.toStdString();
    s.to = to.toStdString();
    return s;
}

QString libraryFrameRateText(const MediaItem& media) {
    if (!media.info.hasVideo() || media.info.isStillImage)
        return {};
    const Rate r = media.info.video.front().frameRate;
    if (!r.isValid())
        return {};
    return LibraryModel::tr("%1 fps").arg(QString::number(r.toDouble(), 'g', 5));
}

QString libraryResolutionText(const MediaItem& media) {
    if (!media.info.hasVideo())
        return {};
    const VideoStreamInfo& v = media.info.video.front();
    return QStringLiteral("%1 × %2").arg(v.width).arg(v.height);
}

QString libraryAudioText(const MediaItem& media) {
    if (!media.info.hasAudio())
        return {};
    const AudioStreamInfo& a = media.info.audio.front();
    QString layout = a.channels == 1   ? LibraryModel::tr("Mono")
                     : a.channels == 2 ? LibraryModel::tr("Stereo")
                     : !a.channelLayout.empty() ? text(a.channelLayout)
                                                : LibraryModel::tr("%n channel(s)", nullptr, a.channels);
    QString out = LibraryModel::tr("%1 kHz %2").arg(QString::number(a.sampleRate.toDouble() / 1000.0, 'g', 4), layout);
    if (media.info.audio.size() > 1)
        out += LibraryModel::tr(", %n stream(s)", nullptr, static_cast<int>(media.info.audio.size()));
    return out;
}

LibraryModel::LibraryModel(QObject* parent) : QAbstractTableModel(parent) {
    for (const LibraryColumnInfo& info : kColumns) {
        if (info.defaultVisible)
            m_columns.push_back(info.column);
    }
}

int LibraryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

int LibraryModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_columns.size());
}

QHash<int, QByteArray> LibraryModel::roleNames() const {
    return {
        {Qt::DisplayRole, "display"},
        {KeyRole, "key"},
        {KindRole, "kind"},
        {NameRole, "name"},
        {ColumnIdRole, "columnId"},
        {EditableRole, "editable"},
        {LabelRole, "label"},
        {ThumbnailRole, "thumbnail"},
        {DurationRole, "durationText"},
        {MetaRole, "meta"},
        {OfflineRole, "offline"},
        {ProxyStateRole, "proxyState"},
        {ProxyProgressRole, "proxyProgress"},
        {GeneratedRole, "generated"},
        {SubclipRole, "subclip"},
        {UnusedRole, "unused"},
        {DuplicateRole, "duplicate"},
        {SelectedRole, "selected"},
        {FilmstripRole, "filmstrip"},
        {BinPathRole, "binPath"},
        {CommentRole, "comment"},
        {CountRole, "itemCount"},
        {DerivedRole, "derivedFrom"},
        {SearchBinRole, "searchBin"},
        {GoodRole, "good"},
        {MarksRole, "marks"},
    };
}

LibraryColumn LibraryModel::columnAt(int section) const {
    if (section < 0 || static_cast<std::size_t>(section) >= m_columns.size())
        return LibraryColumn::Name;
    return m_columns[static_cast<std::size_t>(section)];
}

QString LibraryModel::keyFor(const Row& row) {
    switch (row.kind) {
    case Row::Kind::Bin:
        return binKey(BinId{row.id});
    case Row::Kind::Sequence:
        return sequenceKey(SequenceId{row.id});
    case Row::Kind::Media:
        break;
    }
    return mediaKey(MediaId{row.id});
}

QString LibraryModel::kindOf(const Row& row) {
    switch (row.kind) {
    case Row::Kind::Bin:
        return QStringLiteral("bin");
    case Row::Kind::Sequence:
        return QStringLiteral("sequence");
    case Row::Kind::Media:
        break;
    }
    const MediaItem& m = *row.media;
    if (m.isSubclip())
        return QStringLiteral("subclip");
    if (m.isGenerator())
        return QStringLiteral("title");
    if (m.info.isStillImage)
        return QStringLiteral("still");
    if (m.info.hasVideo())
        return QStringLiteral("video");
    return QStringLiteral("audio");
}

int LibraryModel::rowOf(const QString& key) const { return m_index.value(key, -1); }

QString LibraryModel::keyOf(int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= m_rows.size())
        return {};
    return keyFor(m_rows[static_cast<std::size_t>(row)]);
}

QString LibraryModel::binPath(BinId bin) const {
    QStringList parts;
    std::size_t guard = 0;
    while (bin.isValid() && m_snapshot && guard++ <= m_snapshot->bins.size()) {
        const Bin* b = m_snapshot->findBin(bin);
        if (!b)
            break;
        parts.prepend(text(b->name));
        bin = b->parent;
    }
    return parts.join(QStringLiteral(" › "));
}

QString LibraryModel::cellText(int rowIndex, LibraryColumn column) const {
    if (rowIndex < 0 || static_cast<std::size_t>(rowIndex) >= m_rows.size())
        return {};
    const Row& row = m_rows[static_cast<std::size_t>(rowIndex)];
    const LibraryMediaStatus* status = nullptr;
    if (row.kind == Row::Kind::Media && m_context.status) {
        if (const auto it = m_context.status->find(row.id); it != m_context.status->end())
            status = &it->second;
    }
    switch (column) {
    case LibraryColumn::Name:
        return row.kind == Row::Kind::Bin        ? text(row.bin->name)
               : row.kind == Row::Kind::Sequence ? text(row.sequence->name)
                                                 : text(row.media->name);
    case LibraryColumn::Label:
        return row.kind == Row::Kind::Media      ? libraryLabelName(row.media->colorIndex)
               : row.kind == Row::Kind::Sequence ? libraryLabelName(row.sequence->colorIndex) // 0.2
                                                 : QString();
    case LibraryColumn::MediaType: {
        const QString kind = kindOf(row);
        if (kind == QStringLiteral("bin"))
            return tr("Bin");
        if (kind == QStringLiteral("sequence"))
            return tr("Sequence");
        if (kind == QStringLiteral("subclip"))
            return tr("Subclip");
        if (kind == QStringLiteral("title"))
            return tr("Title");
        if (kind == QStringLiteral("still"))
            return tr("Still image");
        if (kind == QStringLiteral("video"))
            return row.media->info.hasAudio() ? tr("Video and audio") : tr("Video");
        return tr("Audio");
    }
    case LibraryColumn::Duration:
        if (row.kind == Row::Kind::Sequence)
            return timecodeText(row.sequence->duration, row.sequence->frameRate);
        if (row.kind == Row::Kind::Media && !row.media->info.isStillImage && !row.media->isGenerator())
            return timecodeText(itemDuration(*row.media), displayRate(*row.media));
        return {};
    case LibraryColumn::FrameRate:
        if (row.kind == Row::Kind::Sequence)
            return tr("%1 fps").arg(QString::number(row.sequence->frameRate.toDouble(), 'g', 5));
        return row.kind == Row::Kind::Media ? libraryFrameRateText(*row.media) : QString();
    case LibraryColumn::Resolution:
        if (row.kind == Row::Kind::Sequence)
            return QStringLiteral("%1 × %2").arg(row.sequence->width).arg(row.sequence->height);
        return row.kind == Row::Kind::Media ? libraryResolutionText(*row.media) : QString();
    case LibraryColumn::Codec: {
        if (row.kind != Row::Kind::Media)
            return {};
        QStringList codecs;
        if (row.media->info.hasVideo() && !row.media->info.video.front().codec.empty())
            codecs << text(row.media->info.video.front().codec);
        if (row.media->info.hasAudio() && !row.media->info.audio.front().codec.empty())
            codecs << text(row.media->info.audio.front().codec);
        return codecs.join(QStringLiteral(" / "));
    }
    case LibraryColumn::AudioChannels:
        return row.kind == Row::Kind::Media ? libraryAudioText(*row.media) : QString();
    case LibraryColumn::MediaIn:
        if (row.kind == Row::Kind::Media && row.media->markIn)
            return timecodeText(*row.media->markIn, displayRate(*row.media));
        if (row.kind == Row::Kind::Sequence && row.sequence->markIn)
            return timecodeText(*row.sequence->markIn, row.sequence->frameRate);
        return {};
    case LibraryColumn::MediaOut:
        // Premiere shows the last frame marked; the stored Out is exclusive.
        if (row.kind == Row::Kind::Media && row.media->markOut) {
            const Rate rate = displayRate(*row.media);
            const auto frames = ticksToFrames(*row.media->markOut, rate, Rounding::Ceil).value_or(0);
            return text(framesToTimecode(std::max<std::int64_t>(0, frames - 1), rate, false));
        }
        if (row.kind == Row::Kind::Sequence && row.sequence->markOut) {
            const auto frames = ticksToFrames(*row.sequence->markOut, row.sequence->frameRate, Rounding::Ceil).value_or(0);
            return text(framesToTimecode(std::max<std::int64_t>(0, frames - 1), row.sequence->frameRate, false));
        }
        return {};
    case LibraryColumn::Proxy:
        return status ? proxyText(*status) : QString();
    case LibraryColumn::Analysis:
        return status ? analysisText(*status) : QString();
    case LibraryColumn::Generated:
        if (row.kind == Row::Kind::Media && row.media->generated)
            return tr("AI · %1").arg(text(row.media->generated->provider));
        if (row.kind == Row::Kind::Media && row.media->derived) {
            const MediaItem* source = m_snapshot ? m_snapshot->findMedia(row.media->derived->source) : nullptr;
            return tr("Separated from %1").arg(source ? text(source->name) : text(row.media->derived->part));
        }
        return {};
    case LibraryColumn::Date:
        return row.kind == Row::Kind::Media ? dateText(row.media->info.modifiedTime) : QString();
    case LibraryColumn::Path:
        if (row.kind != Row::Kind::Media)
            return {};
        return row.media->isGenerator() ? tr("Made in Wabcut") : QDir::toNativeSeparators(text(row.media->locator.path));
    case LibraryColumn::Comment:
        return row.kind == Row::Kind::Media ? text(row.media->comment) : QString();
    case LibraryColumn::Description:
        return row.kind == Row::Kind::Media ? text(row.media->description) : QString();
    case LibraryColumn::Scene:
        return row.kind == Row::Kind::Media ? text(row.media->metadata.scene) : QString();
    case LibraryColumn::Shot:
        return row.kind == Row::Kind::Media ? text(row.media->metadata.shot) : QString();
    case LibraryColumn::Take:
        return row.kind == Row::Kind::Media ? text(row.media->metadata.take) : QString();
    case LibraryColumn::LogNote:
        return row.kind == Row::Kind::Media ? text(row.media->metadata.logNote) : QString();
    case LibraryColumn::Good:
        // The list draws a check box (GoodRole); the text is for sorting, export and screen readers.
        return row.kind == Row::Kind::Media && row.media->metadata.good ? tr("Good") : QString();
    case LibraryColumn::Client:
        return row.kind == Row::Kind::Media ? text(row.media->metadata.client) : QString();
    case LibraryColumn::TapeName:
        return row.kind == Row::Kind::Media && row.media->timecode ? text(row.media->timecode->reelName) : QString();
    case LibraryColumn::Status:
        return row.kind == Row::Kind::Media ? text(row.media->metadata.status) : QString();
    case LibraryColumn::CameraAngle:
        return row.kind == Row::Kind::Media ? text(row.media->metadata.cameraAngle) : QString();
    case LibraryColumn::CameraLabel:
        return row.kind == Row::Kind::Media ? text(row.media->metadata.cameraLabel) : QString();
    case LibraryColumn::CameraModel:
        return row.kind == Row::Kind::Media ? text(row.media->metadata.cameraModel) : QString();
    case LibraryColumn::ShotDate:
        return row.kind == Row::Kind::Media ? text(row.media->metadata.shotDate) : QString();
    case LibraryColumn::ShotLocation:
        return row.kind == Row::Kind::Media ? text(row.media->metadata.shotLocation) : QString();
    }
    return {};
}

QVariant LibraryModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || static_cast<std::size_t>(index.row()) >= m_rows.size())
        return {};
    const Row& row = m_rows[static_cast<std::size_t>(index.row())];
    const LibraryColumn column = columnAt(index.column());
    switch (role) {
    case Qt::DisplayRole:
        return cellText(index.row(), column);
    case KeyRole:
        return keyFor(row);
    case KindRole:
        return kindOf(row);
    case NameRole:
        return cellText(index.row(), LibraryColumn::Name);
    case ColumnIdRole:
        return QString::fromLatin1(kColumns[static_cast<std::size_t>(column)].id);
    case EditableRole:
        return kColumns[static_cast<std::size_t>(column)].editable &&
               (row.kind == Row::Kind::Media || (row.kind == Row::Kind::Bin && column == LibraryColumn::Name));
    case LabelRole:
        return row.kind == Row::Kind::Media      ? row.media->colorIndex
               : row.kind == Row::Kind::Sequence ? row.sequence->colorIndex // 0.2: sequences have labels too
                                                 : -1;
    case ThumbnailRole:
        if (row.kind == Row::Kind::Sequence)
            return m_context.sequenceThumbnail && m_snapshot ? m_context.sequenceThumbnail(*m_snapshot, *row.sequence) : QString();
        return row.kind == Row::Kind::Media && m_context.thumbnail ? m_context.thumbnail(*row.media) : QString();
    case DurationRole:
        return cellText(index.row(), LibraryColumn::Duration);
    case MetaRole: {
        if (row.kind == Row::Kind::Bin)
            return tr("%n item(s)", nullptr, data(index, CountRole).toInt());
        if (row.kind == Row::Kind::Sequence)
            return QStringLiteral("%1 · %2").arg(cellText(index.row(), LibraryColumn::Resolution),
                                                 cellText(index.row(), LibraryColumn::FrameRate));
        const MediaItem& m = *row.media;
        if (m.info.hasVideo()) {
            const QString fps = libraryFrameRateText(m);
            return fps.isEmpty() ? libraryResolutionText(m) : libraryResolutionText(m) + QStringLiteral(" · ") + fps;
        }
        return libraryAudioText(m);
    }
    case OfflineRole:
        return row.kind == Row::Kind::Media && !row.media->isGenerator() && row.media->locator.offline;
    case ProxyStateRole:
    case ProxyProgressRole: {
        if (row.kind != Row::Kind::Media || !m_context.status)
            return role == ProxyStateRole ? QVariant(QString()) : QVariant(0.0);
        const auto it = m_context.status->find(row.id);
        if (it == m_context.status->end())
            return role == ProxyStateRole ? QVariant(QString()) : QVariant(0.0);
        return role == ProxyStateRole ? QVariant(it->second.proxyState) : QVariant(it->second.proxyProgress);
    }
    case GeneratedRole:
        return row.kind == Row::Kind::Media && row.media->generated.has_value();
    case SubclipRole: {
        if (row.kind != Row::Kind::Media || !row.media->subclip)
            return QVariantMap{};
        const SubclipSpec& s = *row.media->subclip;
        const Rate rate = displayRate(*row.media);
        const auto lastFrame = ticksToFrames(s.range.end(), rate, Rounding::Ceil).value_or(1) - 1;
        QString master;
        if (m_snapshot) {
            if (const MediaItem* parent = m_snapshot->findMedia(s.master))
                master = text(parent->name);
        }
        return QVariantMap{
            {QStringLiteral("inText"), timecodeText(s.range.start, rate)},
            {QStringLiteral("outText"), text(framesToTimecode(std::max<std::int64_t>(0, lastFrame), rate, false))},
            {QStringLiteral("restricted"), s.restricted},
            {QStringLiteral("master"), master},
        };
    }
    case UnusedRole:
        return row.kind == Row::Kind::Media && m_context.used && !m_context.used->contains(row.id);
    case DuplicateRole:
        return row.kind == Row::Kind::Media && m_context.duplicates && m_context.duplicates->contains(row.id);
    case SelectedRole:
        return m_selection.contains(keyFor(row));
    case FilmstripRole:
        return row.kind == Row::Kind::Media && row.media->info.hasVideo() && !row.media->info.isStillImage &&
               !row.media->isGenerator() && !row.media->locator.offline;
    case BinPathRole: {
        if (!m_filtering)
            return QString();
        const BinId bin = row.kind == Row::Kind::Bin        ? row.bin->parent
                          : row.kind == Row::Kind::Sequence ? row.sequence->bin
                                                            : row.media->bin;
        return binPath(bin);
    }
    case CommentRole:
        return row.kind == Row::Kind::Media ? text(row.media->comment) : QString();
    case DerivedRole: {
        if (row.kind != Row::Kind::Media || !row.media->derived)
            return QString();
        const MediaItem* source = m_snapshot ? m_snapshot->findMedia(row.media->derived->source) : nullptr;
        return source ? text(source->name) : text(row.media->derived->part);
    }
    case SearchBinRole:
        return row.kind == Row::Kind::Bin && row.bin->isSearch();
    case GoodRole:
        return row.kind == Row::Kind::Media && row.media->metadata.good;
    case MarksRole: {
        // 0.2 UX (stream T): the In/Out band on the card.
        if (row.kind != Row::Kind::Media || row.media->isGenerator())
            return QVariantMap{};
        const MediaItem& m = *row.media;
        const edit::SourceMarks marks = edit::storedSourceMarks(m);
        const auto extent = edit::sourceMarkExtent(m);
        if ((!marks.in && !marks.out) || !extent || extent->value <= 0)
            return QVariantMap{};
        const Ticks start = m.subclip ? m.subclip->range.start : Ticks{};
        const Ticks length = m.subclip ? m.subclip->range.duration : *extent;
        if (length.value <= 0)
            return QVariantMap{};
        const Ticks in = std::clamp(marks.in.value_or(start), start, start + length);
        const Ticks out = std::clamp(marks.out.value_or(start + length), in, start + length);
        // Marks around the whole card's range mark nothing (Make Subclip stores the subclip's range as its marks).
        if (in == start && out == start + length)
            return QVariantMap{};
        const auto fraction = [&](Ticks t) {
            return std::clamp(static_cast<double>((t - start).value) / static_cast<double>(length.value), 0.0, 1.0);
        };
        const Rate rate = displayRate(m);
        const auto lastFrame = ticksToFrames(out, rate, Rounding::Ceil).value_or(1) - 1;
        return QVariantMap{
            {QStringLiteral("in"), fraction(in)},
            {QStringLiteral("out"), fraction(out)},
            {QStringLiteral("inText"), timecodeText(in, rate)},
            {QStringLiteral("outText"), text(framesToTimecode(std::max<std::int64_t>(0, lastFrame), rate, false))},
            {QStringLiteral("durationText"), timecodeText(out - in, rate)},
        };
    }
    case CountRole: {
        if (row.kind != Row::Kind::Bin || !m_snapshot)
            return 0;
        if (row.bin->isSearch())
            return searchCount(*row.bin->search);
        const BinId bin{row.id};
        int count = 0;
        for (const Bin& b : m_snapshot->bins)
            count += b.parent == bin ? 1 : 0;
        for (const MediaPtr& m : m_snapshot->media)
            count += m->bin == bin ? 1 : 0;
        for (const SequencePtr& s : m_snapshot->sequences)
            count += s->bin == bin ? 1 : 0;
        return count;
    }
    default:
        break;
    }
    return {};
}

QVariant LibraryModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    return libraryColumnTitle(columnAt(section));
}

QVariant LibraryModel::sortKey(const Row& row, LibraryColumn column) const {
    const MediaItem* m = row.kind == Row::Kind::Media ? row.media : nullptr;
    switch (column) {
    case LibraryColumn::Label:
        return m ? m->colorIndex : row.kind == Row::Kind::Sequence ? row.sequence->colorIndex : -1;
    case LibraryColumn::Duration:
        if (row.kind == Row::Kind::Sequence)
            return static_cast<qlonglong>(row.sequence->duration.value);
        return m ? static_cast<qlonglong>(itemDuration(*m).value) : -1LL;
    case LibraryColumn::FrameRate:
        if (row.kind == Row::Kind::Sequence)
            return row.sequence->frameRate.toDouble();
        return m && m->info.hasVideo() && !m->info.isStillImage ? m->info.video.front().frameRate.toDouble() : -1.0;
    case LibraryColumn::Resolution:
        if (row.kind == Row::Kind::Sequence)
            return static_cast<qlonglong>(row.sequence->width) * row.sequence->height;
        return m && m->info.hasVideo() ? static_cast<qlonglong>(m->info.video.front().width) * m->info.video.front().height
                                       : -1LL;
    case LibraryColumn::AudioChannels:
        return m && m->info.hasAudio() ? m->info.audio.front().channels : -1;
    case LibraryColumn::MediaIn:
        return m && m->markIn ? static_cast<qlonglong>(m->markIn->value) : -1LL;
    case LibraryColumn::MediaOut:
        return m && m->markOut ? static_cast<qlonglong>(m->markOut->value) : -1LL;
    case LibraryColumn::Date:
        return m ? static_cast<qlonglong>(m->info.modifiedTime) : -1LL;
    case LibraryColumn::Generated:
        return m && m->generated ? 1 : 0;
    case LibraryColumn::Good:
        return m && m->metadata.good ? 1 : 0;
    default:
        break;
    }
    return QVariant();
}

void LibraryModel::sortRows(std::vector<Row>& rows) const {
    const LibraryColumn column = m_sortColumn;
    struct Keyed {
        Row row;
        QVariant key;
        QString text;
    };
    std::vector<Keyed> keyed;
    keyed.reserve(rows.size());
    const bool textual = !sortKey(Row{Row::Kind::Bin, 0, nullptr, nullptr, nullptr}, column).isValid() ||
                         column == LibraryColumn::Name;
    // Format each row's text once (cellText needs the row in m_rows, so format straight from the row here).
    for (const Row& r : rows) {
        Keyed k{r, sortKey(r, column), {}};
        if (textual || !k.key.isValid()) {
            switch (column) {
            case LibraryColumn::Name:
                k.text = r.kind == Row::Kind::Bin        ? text(r.bin->name)
                         : r.kind == Row::Kind::Sequence ? text(r.sequence->name)
                                                         : text(r.media->name);
                break;
            default: {
                // The cell text of a column without a numeric key (type, codec, proxy, ...).
                LibraryModel* self = const_cast<LibraryModel*>(this);
                std::vector<Row> one{r};
                std::swap(self->m_rows, one);
                k.text = cellText(0, column);
                std::swap(self->m_rows, one);
                break;
            }
            }
        }
        keyed.push_back(std::move(k));
    }
    const bool ascending = m_ascending;
    std::stable_sort(keyed.begin(), keyed.end(), [&](const Keyed& a, const Keyed& b) {
        // Groups first, whatever the order: bins, sequences, media (Row::Kind's order).
        if (a.row.kind != b.row.kind)
            return a.row.kind < b.row.kind;
        int c = 0;
        if (a.key.isValid() && b.key.isValid() && !textual) {
            const QPartialOrdering o = QVariant::compare(a.key, b.key);
            c = o == QPartialOrdering::Less ? -1 : o == QPartialOrdering::Greater ? 1 : 0;
        } else {
            c = QString::compare(a.text, b.text, Qt::CaseInsensitive);
        }
        if (c == 0)
            return false;
        return ascending ? c < 0 : c > 0;
    });
    for (std::size_t i = 0; i < rows.size(); ++i)
        rows[i] = keyed[i].row;
}

bool LibraryModel::inScope(const Row& row, const LibraryFilter& f) {
    if (!f.audioOnly || row.kind == Row::Kind::Bin)
        return true;
    return row.kind == Row::Kind::Media && !row.media->info.hasVideo() && row.media->info.hasAudio();
}

bool LibraryModel::matches(const Row& row, const LibraryFilter& f) const {
    const QString kind = kindOf(row);
    if (!f.type.isEmpty() && f.type != kind)
        return false;
    const bool mediaOnly = f.label >= 0 || f.transcribed || f.offline || f.proxy || f.generated || f.unused ||
                           f.duplicates || !f.from.isEmpty() || !f.to.isEmpty() || f.good;
    if (mediaOnly && row.kind != Row::Kind::Media)
        return false;
    if (row.kind == Row::Kind::Media) {
        const MediaItem& m = *row.media;
        const LibraryMediaStatus* status = nullptr;
        if (m_context.status) {
            if (const auto it = m_context.status->find(row.id); it != m_context.status->end())
                status = &it->second;
        }
        if (f.label >= 0 && m.colorIndex != f.label)
            return false;
        if (f.transcribed && !(status && status->transcribed))
            return false;
        if (f.offline && (m.isGenerator() || !m.locator.offline))
            return false;
        if (f.proxy && !(status && !status->proxyState.isEmpty()))
            return false;
        if (f.generated && !m.generated)
            return false;
        if (f.good && !m.metadata.good)
            return false;
        if (f.unused && m_context.used && m_context.used->contains(row.id))
            return false;
        if (f.duplicates && !(m_context.duplicates && m_context.duplicates->contains(row.id)))
            return false;
        if (!f.from.isEmpty() || !f.to.isEmpty()) {
            const QString day = dayText(m.info.modifiedTime);
            if (day.isEmpty() || (!f.from.isEmpty() && day < f.from) || (!f.to.isEmpty() && day > f.to))
                return false;
        }
    }
    const QString needle = f.text.trimmed();
    if (needle.isEmpty())
        return true;
    const auto has = [&](const std::string& s) { return text(s).contains(needle, Qt::CaseInsensitive); };
    const QString& field = f.field;
    const bool all = field.isEmpty();
    switch (row.kind) {
    case Row::Kind::Bin:
        return (all || field == QStringLiteral("name")) && has(row.bin->name);
    case Row::Kind::Sequence:
        return (all || field == QStringLiteral("name")) && has(row.sequence->name);
    case Row::Kind::Media:
        break;
    }
    const MediaItem& m = *row.media;
    const ClipMetadata& md = m.metadata;
    const std::string tape = m.timecode ? m.timecode->reelName : std::string();
    if (!all) {
        // Search Bins (0.2, P.3): one property.
        if (field == QStringLiteral("name"))
            return has(m.name);
        if (field == QStringLiteral("comment"))
            return has(m.comment);
        if (field == QStringLiteral("description"))
            return has(m.description);
        if (field == QStringLiteral("scene"))
            return has(md.scene);
        if (field == QStringLiteral("shot"))
            return has(md.shot);
        if (field == QStringLiteral("take"))
            return has(md.take);
        if (field == QStringLiteral("logNote"))
            return has(md.logNote);
        if (field == QStringLiteral("client"))
            return has(md.client);
        if (field == QStringLiteral("tapeName"))
            return has(tape);
        if (field == QStringLiteral("status"))
            return has(md.status);
        if (field == QStringLiteral("cameraAngle"))
            return has(md.cameraAngle);
        if (field == QStringLiteral("cameraLabel"))
            return has(md.cameraLabel);
        if (field == QStringLiteral("cameraModel"))
            return has(md.cameraModel);
        if (field == QStringLiteral("shotDate"))
            return has(md.shotDate);
        if (field == QStringLiteral("shotLocation"))
            return has(md.shotLocation);
        if (field == QStringLiteral("path"))
            return has(m.locator.path);
        return false;
    }
    return has(m.name) || has(m.comment) || has(m.description) || has(m.locator.path) || has(md.scene) ||
           has(md.shot) || has(md.take) || has(md.logNote) || has(md.client) || has(tape) || has(md.status) ||
           has(md.cameraAngle) || has(md.cameraLabel) || has(md.cameraModel) || has(md.shotDate) || has(md.shotLocation);
}

int LibraryModel::searchCount(const BinSearch& search) const {
    if (!m_snapshot)
        return 0;
    const LibraryFilter f = LibraryFilter::fromSearch(search);
    int count = 0;
    for (const Bin& b : m_snapshot->bins)
        count += !b.isSearch() && matches({Row::Kind::Bin, b.id.value, &b, nullptr, nullptr}, f) ? 1 : 0;
    for (const SequencePtr& s : m_snapshot->sequences)
        count += matches({Row::Kind::Sequence, s->id.value, nullptr, s.get(), nullptr}, f) ? 1 : 0;
    for (const MediaPtr& m : m_snapshot->media)
        count += matches({Row::Kind::Media, m->id.value, nullptr, nullptr, m.get()}, f) ? 1 : 0;
    return count;
}

void LibraryModel::refresh(ProjectSnapshot snapshot, BinId bin, const LibraryFilter& filter, const Context& context) {
    std::vector<Row> rows;
    // 0.2 (P.3): a search bin's query lists the whole project (and an active Find narrows it).
    const Bin* searchBin = snapshot && bin.isValid() ? snapshot->findBin(bin) : nullptr;
    const std::optional<LibraryFilter> query =
        searchBin && searchBin->isSearch() ? std::optional<LibraryFilter>(LibraryFilter::fromSearch(*searchBin->search))
                                           : std::nullopt;
    const bool filtering = filter.active() || query.has_value();
    // The new context goes in first: matches() and the sort read it.
    m_context = context;
    if (snapshot) {
        rows.reserve(snapshot->media.size() + snapshot->bins.size() + snapshot->sequences.size());
        const auto take = [&](const Row& row, BinId parent) {
            if (query) {
                // A search bin never lists search bins (nor itself).
                if (row.kind == Row::Kind::Bin && row.bin->isSearch())
                    return;
                if (inScope(row, filter) && matches(row, *query) && (!filter.active() || matches(row, filter)))
                    rows.push_back(row);
                return;
            }
            if (inScope(row, filter) && (filtering ? matches(row, filter) : parent == bin))
                rows.push_back(row);
        };
        for (const Bin& b : snapshot->bins)
            take({Row::Kind::Bin, b.id.value, &b, nullptr, nullptr}, b.parent);
        for (const SequencePtr& s : snapshot->sequences)
            take({Row::Kind::Sequence, s->id.value, nullptr, s.get(), nullptr}, s->bin);
        for (const MediaPtr& m : snapshot->media)
            take({Row::Kind::Media, m->id.value, nullptr, nullptr, m.get()}, m->bin);
    }
    // Sorting reads the new snapshot through the rows' pointers (binPath reads m_snapshot: not used while sorting).
    sortRows(rows);

    const bool sameRows = rows.size() == m_rows.size() &&
                          std::equal(rows.begin(), rows.end(), m_rows.begin(), [](const Row& a, const Row& b) {
                              return a.kind == b.kind && a.id == b.id;
                          });
    if (sameRows && filtering == m_filtering) {
        m_snapshot = std::move(snapshot);
        m_rows = std::move(rows);
        if (!m_rows.empty())
            emit dataChanged(index(0, 0), index(static_cast<int>(m_rows.size()) - 1, std::max(0, columnCount() - 1)));
        return;
    }
    beginResetModel();
    m_snapshot = std::move(snapshot);
    m_rows = std::move(rows);
    m_filtering = filtering;
    m_index.clear();
    m_index.reserve(static_cast<qsizetype>(m_rows.size()));
    for (std::size_t i = 0; i < m_rows.size(); ++i)
        m_index.insert(keyFor(m_rows[i]), static_cast<int>(i));
    endResetModel();
    emit countChanged();
}

void LibraryModel::setColumns(std::vector<LibraryColumn> columns) {
    if (columns.empty())
        columns.push_back(LibraryColumn::Name);
    if (columns == m_columns)
        return;
    beginResetModel();
    m_columns = std::move(columns);
    endResetModel();
}

void LibraryModel::setSort(LibraryColumn column, bool ascending) {
    if (column == m_sortColumn && ascending == m_ascending)
        return;
    m_sortColumn = column;
    m_ascending = ascending;
    if (m_rows.empty())
        return;
    beginResetModel();
    sortRows(m_rows);
    m_index.clear();
    for (std::size_t i = 0; i < m_rows.size(); ++i)
        m_index.insert(keyFor(m_rows[i]), static_cast<int>(i));
    endResetModel();
}

void LibraryModel::setSelection(const QSet<QString>& keys) {
    if (keys == m_selection)
        return;
    QSet<QString> changed = keys;
    changed.unite(m_selection);
    m_selection = keys;
    for (const QString& key : std::as_const(changed)) {
        const int row = rowOf(key);
        if (row >= 0)
            emit dataChanged(index(row, 0), index(row, std::max(0, columnCount() - 1)), {SelectedRole});
    }
}

void LibraryModel::statusChanged() {
    if (m_rows.empty())
        return;
    emit dataChanged(index(0, 0), index(static_cast<int>(m_rows.size()) - 1, std::max(0, columnCount() - 1)),
                     {Qt::DisplayRole, ProxyStateRole, ProxyProgressRole});
}

} // namespace wabcut::app
