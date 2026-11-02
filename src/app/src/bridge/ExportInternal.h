#pragma once

// Helpers of ExportController.cpp that its other translation units share (ExportBatches.cpp: several sequences, each
// range as a file, the media usage report; ExportQueueKeeper.cpp: the queue across restarts). Internal: not for QML.

#include <QString>

#include <string_view>

namespace wabcut::app::exportdetail {

/// A default file name made of `name`: characters file systems refuse replaced, no trailing dots, never a Windows
/// device name; "Export" when nothing is left.
QString safeFileName(QString name);

/// The Export workspace's name for a built-in preset id ("YouTube 1080p"), else the export library's English name.
QString presetDisplayName(std::string_view id);

} // namespace wabcut::app::exportdetail
