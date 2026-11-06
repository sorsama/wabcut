// 0.7, batch A5 (timeline side): the check carried over from A4 - in trim mode Shift on another clip's edge adds that edit
// point instead of ending trim mode (a real mouse press and release on a TimelineView).

#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "TimelineView.h"
#include "TrimBridge.h"

#include <catch2/catch_test_macros.hpp>

#include <QObject>
#include <QVariantMap>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

int pointsOnClip(const TrimBridge& trim, const QString& clipKeyText) {
    int n = 0;
    for (const QVariant& point : trim.points()) {
        if (point.toMap().value(QStringLiteral("clip")).toString() == clipKeyText)
            ++n;
    }
    return n;
}

} // namespace

TEST_CASE("Shift on another track's clip edge adds that edit point and keeps trim mode (A5 check)",
          "[app][timeline][trim][a5]") {
    Rig rig(R"(
        V2 |........Cccccccc|
        V1 |AaaaaaaaBbbbbbbb|
        A1 |AaaaaaaaBbbbbbbb|
    )");
    TrimBridge trim;
    trim.setController(&rig.controller);
    QObject::connect(rig.view, &TimelineView::editPointClicked, &trim, &TrimBridge::selectEditPoint);
    QObject::connect(&trim, &TrimBridge::pointsChanged, rig.view, [&] {
        rig.view->setTrimPoints(trim.points());
        rig.view->setTrimType(trim.type());
    });
    rig.view->setTool(QStringLiteral("ripple"));

    // A plain click on B's head: its edit point (with the linked audio's).
    rig.press({rig.x(8) + 2, rig.y("V1")});
    rig.release({rig.x(8) + 2, rig.y("V1")});
    REQUIRE(trim.active());
    REQUIRE(trim.points().size() == 2);

    // Shift-click on C's head, on another track: trim mode stays and the point is added to the two.
    rig.press({rig.x(8) + 2, rig.y("V2")}, Qt::ShiftModifier);
    CHECK(trim.active()); // the press alone must not end trim mode
    rig.release({rig.x(8) + 2, rig.y("V2")}, Qt::ShiftModifier);
    REQUIRE(trim.active());
    CHECK(trim.points().size() == 3);
    CHECK(pointsOnClip(trim, clipKey(rig.clip("V2@8"))) == 1);
    CHECK(pointsOnClip(trim, clipKey(rig.clip("V1@8"))) == 1);
    CHECK(pointsOnClip(trim, clipKey(rig.clip("A1@8"))) == 1);

    // Shift on a clip's body still toggles the selection (and so ends trim mode, as a click on a clip does).
    rig.press({rig.x(3), rig.y("V1")}, Qt::ShiftModifier);
    rig.release({rig.x(3), rig.y("V1")}, Qt::ShiftModifier);
    CHECK_FALSE(trim.active());
}
