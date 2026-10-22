#pragma once

// Program Monitor guides (P.2 E6, Premiere's View > Add Guide / Lock Guides / Clear Guides / Guide Templates;
// docs/monitors.md): lines across the program frame at a position in sequence pixels or in percent of the frame, with a
// colour. Like Premiere's, they belong to the user, not to a project: they persist across projects and sessions
// (QSettings), and named templates save, apply, export and import sets of them ("Wabcut guides" JSON files).
//
// Files and settings are untrusted: everything read is bounded and checked (kMaxGuides, kMaxTemplates, finite values in
// range, names up to kMaxNameLength).

#include <QByteArray>
#include <QColor>
#include <QJsonArray>
#include <QString>

#include <expected>
#include <vector>

namespace wabcut::app::guides {

inline constexpr int kMaxGuides = 256;
inline constexpr int kMaxTemplates = 256;
inline constexpr int kMaxNameLength = 128;
inline constexpr double kMaxPixels = 100000.0;
inline constexpr double kMaxPercent = 1000.0;

struct Guide {
    bool vertical = false; ///< a vertical line (a position along the width), else horizontal (along the height)
    double value = 0.0;    ///< in `percent` of the frame, else in sequence pixels from the frame's left / top edge
    bool percent = false;
    QColor color{0x40, 0xC8, 0xF0};

    /// The position as a fraction of the frame's width (vertical) or height (horizontal) of `width` x `height` pixels.
    double fraction(double width, double height) const;
    /// The value that puts the guide at `fraction` of the frame, in its own units.
    double valueAt(double fraction, double width, double height) const;

    bool operator==(const Guide&) const = default;
};

struct Template {
    QString name;
    std::vector<Guide> guides;

    bool operator==(const Template&) const = default;
};

/// The default colour of a new guide.
QColor defaultColor();

QJsonArray toJson(const std::vector<Guide>& guides);
/// Guides from JSON (settings, a template); InvalidArgument-style messages for anything out of bounds.
std::expected<std::vector<Guide>, QString> fromJson(const QJsonArray& array);

QJsonArray templatesToJson(const std::vector<Template>& templates);
std::expected<std::vector<Template>, QString> templatesFromJson(const QJsonArray& array);

/// A guides file: {"format": "wabcut-guides", "version": 1, "templates": [{"name", "guides": [...]}]}.
QByteArray exportFile(const std::vector<Template>& templates);
std::expected<std::vector<Template>, QString> importFile(const QByteArray& data);

} // namespace wabcut::app::guides
