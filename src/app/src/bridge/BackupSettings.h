#pragma once

// BackupSettings (0.7, card 029): the user's choices for the ".bakN" copies of saved versions and the ".autosave" files,
// kept in QSettings under project/ and handed to the ProjectDocument (ProjectLifecycle applies them).
//
//   * project/backupLocation: "beside" (next to the project, how every version before 0.7 did it), "folder" (in
//     "Wabcut Auto-Save" next to the project) or "chosen" (in project/backupFolder, one sub-folder per project).
//   * project/backupFolder: the chosen root (a local path), used by "chosen".
//   * project/backupCount: saved versions kept as ".bak1" .. (0-10, default 5).
//   * project/autosaveCopies: autosave versions kept (1-10, default 3).
//
// An install that already has settings keeps "beside" until the user changes it (nothing moves silently); a new install
// (no settings at all yet) starts with "folder" - chooseDefaultForNewInstall() decides that once, early in main().

#include <wabcut/model/ProjectIO.h>

#include <QString>

class QSettings;

namespace wabcut::app::backup {

inline constexpr int kDefaultBackupCount = 5;
inline constexpr int kDefaultAutosaveCopies = 3;

/// "beside", "folder" or "chosen" (anything else reads as "beside").
QString normalizedLocation(const QString& value);

/// Writes project/backupLocation when it is not set yet: "folder" when `settings` holds nothing at all (a new install),
/// "beside" when it holds anything (an install from before this setting existed).
void chooseDefaultForNewInstall(QSettings& settings);
void chooseDefaultForNewInstall(); ///< on a default-constructed QSettings

/// What the settings say now. A "chosen" location without a usable folder falls back to "beside"; the chosen folder is
/// kept in `root` whatever the mode, so recovery still finds the copies written there.
wabcut::io::BackupLocation locationFromSettings(const QSettings& settings);
int backupCountFromSettings(const QSettings& settings);   ///< 0-10
int autosaveCopiesFromSettings(const QSettings& settings); ///< 1-10

} // namespace wabcut::app::backup
