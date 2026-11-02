#pragma once

// DiskSpace (P1 #29): the one free-space lookup behind the low-disk-space warnings - the Export panel's estimate,
// running exports (ExportController::diskSpaceLow), Render In to Out and proxy generation (MediaReadinessBridge).
// Tests replace the lookup with a fake (setFreeBytesProviderForTests), so a "full drive" needs no full drive.

#include <QString>

#include <functional>

namespace wabcut::app::diskspace {

/// The bytes free for this user on the volume holding `path` (a file or a folder, a path or a file URL; the nearest
/// existing parent is asked when it does not exist yet), or -1 when unknown (no path, an unreachable volume).
qint64 freeBytesAt(const QString& path);

/// The volume's name for messages: its display name ("Data"), else its root ("D:/"), else the folder.
QString volumeNameOf(const QString& path);

/// Tests: replaces freeBytesAt's lookup (receives the path as given); an empty function restores the real one.
void setFreeBytesProviderForTests(std::function<qint64(const QString&)> provider);

/// The margin a job wants over its own estimate: the warning shows when needed x kMargin does not fit.
inline constexpr double kMargin = 1.2;

/// "Only 1.2 GB is free on Data; about 3.4 GB is needed for <what>." when `needed` x kMargin bytes do not fit on the
/// volume of `path`, else "" (also when the free space is unknown). `what`: "the export", "the previews", ...
QString lowSpaceMessage(const QString& path, double needed, const QString& what);

/// "850 KB", "142 MB", "1.24 GB" (decimal units, as drives are sold).
QString bytesText(double bytes);

} // namespace wabcut::app::diskspace
