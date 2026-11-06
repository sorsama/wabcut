#pragma once

// CloudBridge's private parts (CloudBridge.cpp, CloudBridgeReview.cpp): the location record, the secrets of one, and the state the
// bridge keeps beside its QML-facing lists.

#include "CloudBridge.h"

#include <wabcut/cloud/ReviewLink.h>

#include <QDateTime>
#include <QLocale>
#include <QSet>

#include <map>
#include <set>
#include <string>

namespace wabcut::app {

class HistoryBridge;

namespace cloud_detail {

inline QString qs(const std::string& text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
inline std::string s(const QString& text) { return text.toStdString(); }

/// "Today 14:05", "Yesterday 09:30", or the short date and time; empty for a text that is not a time.
inline QString whenText(const std::string& iso) {
    if (iso.empty())
        return {};
    QDateTime when = QDateTime::fromString(qs(iso), Qt::ISODate);
    if (!when.isValid())
        return qs(iso);
    when = when.toLocalTime();
    const QString time = QLocale().toString(when.time(), QLocale::ShortFormat);
    const QDate today = QDate::currentDate();
    if (when.date() == today)
        return CloudBridge::tr("Today %1").arg(time);
    if (when.date() == today.addDays(-1))
        return CloudBridge::tr("Yesterday %1").arg(time);
    return QLocale().toString(when, QLocale::ShortFormat);
}

/// The undo step a sync that changed the project is committed as.
inline constexpr const char* kSyncStepLabel = "Sync Cloud Project";

} // namespace cloud_detail

struct CloudBridge::Location {
    QString id;
    QString name;
    QString kind;    ///< "folder" | "webdav" | "s3"
    QString purpose; ///< "projects" | "reviews"
    QString folder;
    QString url;
    QString user;
    QString endpoint;
    QString region;
    QString bucket;
    QString prefix;
    bool pathStyle = true;
    bool allowInsecure = false;
    QStringList mediaFolders;
    QString check;     ///< "" | "ok" | "problem"
    QString checkText;
};

struct CloudBridge::Secrets {
    std::string a; ///< WebDAV: the password; S3: the access key id
    std::string b; ///< S3: the secret access key
};

enum class LockKind : std::uint8_t { None, Mine, Other, Unknown };

class CloudBridge::Impl {
public:
    std::vector<Location> locations;
    std::map<QString, Secrets> secrets;
    std::set<QString> secretKnown;
    cloud::Identity who;
    HistoryBridge* history = nullptr;

    // the open project
    std::filesystem::path linkedFile;
    bool wasDirty = false;
    bool hasLink = false;
    cloud::CloudLink link;
    std::vector<MergeConflict> conflicts;
    std::vector<cloud::ReviewLinkRecord> links;
    std::unique_ptr<cloud::RemoteStore> lockStore;
    std::unique_ptr<cloud::CloudLock> lock;
    LockKind lockKind = LockKind::None;
    QString lockText;
    /// The undo step of the last sync that changed the project: while it is not on the undo stack, nothing is sent.
    struct Guard {
        bool active = false;
        std::size_t count = 0;
    } guard;
    /// What the project row last said about the guard (undo and redo change it without a new document).
    bool guardShown = false;

    // the review copy
    struct Prepared {
        bool ready = false;
        std::filesystem::path folder;
        std::filesystem::path video;
        std::uint64_t bytes = 0;
        std::uint64_t sequence = 0;
        std::string sequenceName;
        std::int64_t startTicks = 0;
        std::int64_t durationTicks = 0;
        Rate rate{25, 1};
        bool dropFrame = false;
        std::int64_t frames = 0;
        std::string contentHash;
        QString resolution;
        double seconds = 0.0;
    } prepared;
};

using LockKindAlias = LockKind;

} // namespace wabcut::app
