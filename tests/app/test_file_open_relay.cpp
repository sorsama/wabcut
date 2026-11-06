// macOS first-class: Finder hands a double-clicked, Open With or Dock-dropped file to the running application as a
// QFileOpenEvent, also right after launch, before any window exists (FileOpenRelay, main()). The relay keeps those files
// until the window can take them and then passes them on exactly once, in order; a URL that is not a local file and every
// other event are not its business.

#include "FileOpenRelay.h"

#include <catch2/catch_test_macros.hpp>

#include <QEvent>
#include <QFileOpenEvent>
#include <QList>
#include <QUrl>

using wabcut::app::FileOpenRelay;

namespace {

struct Collector {
    explicit Collector(FileOpenRelay& relay) {
        QObject::connect(&relay, &FileOpenRelay::filesRequested, &relay,
                         [this](const QList<QUrl>& files) { batches.append(files); });
    }
    QList<QList<QUrl>> batches;
};

QUrl projectUrl(const char* name) {
    return QUrl::fromLocalFile(QStringLiteral("/Users/editor/Films/") + QString::fromLatin1(name));
}

} // namespace

TEST_CASE("File open events wait for the window and are passed on once, in order", "[app][fileopen]") {
    FileOpenRelay relay;
    Collector seen(relay);
    const QUrl first = projectUrl("first.wabcut");
    const QUrl second = projectUrl("second.wabcut");
    QFileOpenEvent a(first);
    QFileOpenEvent b(second);

    // Before the window exists (Finder launched Wabcut for the file): kept, nothing emitted, the event consumed.
    CHECK(relay.eventFilter(nullptr, &a));
    CHECK(relay.eventFilter(nullptr, &b));
    CHECK(seen.batches.isEmpty());
    CHECK(relay.pending() == QList<QUrl>{first, second});

    relay.setReady(true);
    REQUIRE(seen.batches.size() == 1);
    CHECK(seen.batches[0] == QList<QUrl>{first, second});
    CHECK(relay.pending().isEmpty());

    // The window is up: a later double-click goes straight through.
    const QUrl third = projectUrl("third.wabcut");
    QFileOpenEvent c(third);
    CHECK(relay.eventFilter(nullptr, &c));
    REQUIRE(seen.batches.size() == 2);
    CHECK(seen.batches[1] == QList<QUrl>{third});

    // Ready again with nothing waiting emits nothing; the window going away holds new files back.
    relay.setReady(true);
    CHECK(seen.batches.size() == 2);
    relay.setReady(false);
    QFileOpenEvent d(first);
    CHECK(relay.eventFilter(nullptr, &d));
    CHECK(seen.batches.size() == 2);
    CHECK(relay.pending() == QList<QUrl>{first});
}

TEST_CASE("File open events of other kinds are ignored", "[app][fileopen]") {
    FileOpenRelay relay;
    Collector seen(relay);
    relay.setReady(true);

    QFileOpenEvent web(QUrl(QStringLiteral("https://example.com/film.wabcut")));
    CHECK(relay.eventFilter(nullptr, &web)); // handled (not passed on to Qt), but nothing to open
    QEvent other(QEvent::User);
    CHECK_FALSE(relay.eventFilter(nullptr, &other));
    CHECK(seen.batches.isEmpty());
    CHECK(relay.pending().isEmpty());
}
