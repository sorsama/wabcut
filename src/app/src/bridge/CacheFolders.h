#pragma once

// CacheFolders (P1 #48): where the app-wide caches live, as chosen in Settings > Media & cache. One place reads the
// settings so the editor (ProjectController's media cache), main() (the AI cache) and MediaReadinessBridge (the rows
// and their setters) agree:
//
//   cache/mediaFolder   the media cache (thumbnails, waveforms, filmstrips); "" = the default, <app cache>/media
//   cache/aiFolder      the AI models and analysis results (ai::userCacheDir through WABCUT_CACHE_DIR); "" = default
//   proxies/location    "project" (next to the project, the default) or "folder" (proxies/folder)
//   proxies/folder      the folder proxies go to with "folder"
//
// The media and AI folders apply when Wabcut starts (the thumbnail service and the AI services read them once); the
// proxy folder applies at once to new proxies. A production's own media cache (Production Settings) still wins.

#include <QString>

namespace wabcut::app::cachefolders {

/// <app cache>/media (native separators off: a Qt path).
QString defaultMediaCacheFolder();
/// The media cache folder to use: the chosen one, else the default.
QString mediaCacheFolder();
/// The chosen media cache folder, "" for the default.
QString chosenMediaCacheFolder();
void setChosenMediaCacheFolder(const QString& folder);

/// The chosen AI cache folder, "" for the default (ai::userCacheDir()).
QString chosenAiCacheFolder();
void setChosenAiCacheFolder(const QString& folder);
/// main(), before any service starts: WABCUT_CACHE_DIR from cache/aiFolder when it is set and the environment does
/// not set one already. Returns whether it set it.
bool applyChosenAiCacheFolder();

/// "project" or "folder".
QString proxyLocation();
void setProxyLocation(const QString& location);
/// The folder for "folder" ("" when none was chosen).
QString proxyFolderSetting();
void setProxyFolderSetting(const QString& folder);
/// The proxy root ProxyManager uses: proxyFolderSetting() with "folder", else "" (next to the project).
QString proxyRoot();

/// A local folder from what QML gives (a file URL or a path); "" stays "".
QString localFolder(const QString& urlOrPath);

/// "412 GB free on Data" for the volume holding `folder` (diskspace::freeBytesAt, the low-space warnings' lookup);
/// "" when unknown.
QString freeSpaceText(const QString& folder);

} // namespace wabcut::app::cachefolders
