#pragma once

// Which clips have a camera gyro Wabcut can use (0.7, Warp Stabilizer's Method "Gyro (camera metadata)"; docs/motion.md).
//
// Finding out takes a look into the media file (its data tracks, the start of the gyro track), which never happens on the
// GUI thread: lookup() answers from a cache and, for a file it has not seen, starts the probe on a worker thread and
// emits changed() when the answer is in. Effect Controls offers the Gyro method (and shows what the file has) from it; the
// Stabilizer bridge reports it to the Effect Controls header. One answer per file path.

#include <wabcut/editor/Gyro.h>
#include <wabcut/model/ProjectState.h>

#include <QHash>
#include <QObject>
#include <QString>

#include <filesystem>
#include <functional>
#include <optional>

namespace wabcut::app {

class GyroAvailability final : public QObject {
    Q_OBJECT

public:
    static GyroAvailability& instance();

    /// The probe of `item`'s file, once known. nullopt while it is being read in the background (changed() follows), and for
    /// a file that is offline or has no video.
    std::optional<editor::GyroProbe> lookup(const MediaItem& item, const std::filesystem::path& projectFile);

    /// A counter that moves whenever an answer arrives (QML bindings read it to re-ask).
    int revision() const { return m_revision; }

    // ---- Tests ----
    /// Replaces the file probe; `synchronous` runs it on the calling thread, at the first lookup.
    void setProbeForTests(std::function<editor::GyroProbe(const std::string&)> probe, bool synchronous = true);
    /// Forgets every answer (tests, a new project).
    void clear();

signals:
    void changed();

private:
    GyroAvailability() = default;
    struct Entry {
        bool done = false;
        editor::GyroProbe probe;
    };
    void finish(const QString& path, editor::GyroProbe probe);

    QHash<QString, Entry> m_entries;
    std::function<editor::GyroProbe(const std::string&)> m_probe;
    bool m_synchronous = false;
    int m_revision = 0;
};

} // namespace wabcut::app
