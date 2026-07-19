// The gesture corpus: feel invariants, held by a test.
//
// Every entry drives synthetic input through the interact layer with no toolkit
// present. That is the whole point — feel is subjective at discovery time and
// objective forever after, and the way it becomes objective is that a script
// can replay the gesture and a machine can check the invariant.
//
// The opening set covers what stage 3 commits to: drag locality, saturation
// with attribution, release-commits-seeds, no spring-back, undo restoring the
// pre-drag bytes, deletion counts, and off-screen ripple.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "core/persist.h"
#include "interact/script.h"
#include "interact/session.h"
#include "solve/demosketch.h"
#include "support/build.h"

using namespace paroculus;
using paroculus::test::addCircle;
using paroculus::test::addConstraint;
using paroculus::test::addPoint;
using paroculus::test::addSegment;

namespace {

constexpr double VIEW_WIDTH = 800.0;
constexpr double VIEW_HEIGHT = 600.0;

// A plain 1:1 view with the origin at the viewport centre and Y flipped, so
// document coordinates map to pixels predictably and a test can reason about
// both regimes without arithmetic getting in the way.
Viewport testViewport(double scale = 1.0) {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(VIEW_WIDTH * 0.5, VIEW_HEIGHT * 0.5));
    m.scale(Eigen::Vector2d(scale, -scale));
    Viewport viewport;
    viewport.view = ViewTransform(m);
    viewport.width = VIEW_WIDTH;
    viewport.height = VIEW_HEIGHT;
    return viewport;
}

// Drives one press-move-release gesture, in screen pixels, exactly as the shell
// would. Steps are interpolated because a drag is a stream of moves and the
// solver is warm-started from each.
void dragGesture(Session &session, Eigen::Vector2d from, Eigen::Vector2d to, int steps = 8) {
    const Viewport &v = session.viewport();
    auto at = [&](PointerAction action, Eigen::Vector2d screen) {
        return PointerEvent::at(action, screen, v.view, Button::Left);
    };

    session.handle(at(PointerAction::Press, from));
    for(int i = 1; i <= steps; i++) {
        const double t = static_cast<double>(i) / steps;
        session.handle(at(PointerAction::Move, from + t * (to - from)));
    }
    session.handle(at(PointerAction::Release, to));
}

Eigen::Vector2d screenOf(const Session &session, EntityId id) {
    return session.viewport().view.toScreen(*session.pose().point(id));
}

// A horizontal segment with its left end pinned, plus an unconnected point far
// away. The standard fixture: one thing that moves, one thing that must not.
struct Fixture {
    Document doc;
    UndoJournal journal;
    EntityId anchor, free, segment, lonely;
};

Fixture buildFixture() {
    Fixture f;
    f.anchor = addPoint(f.doc, -100.0, 0.0);
    f.free = addPoint(f.doc, 100.0, 0.0);
    f.segment = addSegment(f.doc, f.anchor, f.free);
    addConstraint(f.doc, ConstraintKind::Pin, {f.anchor});
    addConstraint(f.doc, ConstraintKind::Horizontal, {f.segment});
    f.lonely = addPoint(f.doc, -300.0, -200.0);
    return f;
}

}  // namespace

TEST_CASE("gesture: a click selects what is under the cursor") {
    Fixture f = buildFixture();
    Session session(f.doc, f.journal);
    session.setViewport(testViewport());

    const Eigen::Vector2d target = screenOf(session, f.free);
    session.handle(PointerEvent::at(PointerAction::Press, target, session.viewport().view,
                                    Button::Left));
    session.handle(PointerEvent::at(PointerAction::Release, target, session.viewport().view,
                                    Button::Left));

    CHECK_FALSE(session.selection().empty());
    CHECK(session.selection().contains(f.free));
    // A click selects the connected shape, not the bare vertex.
    CHECK(session.selection().contains(f.segment));
    CHECK_FALSE(session.selection().contains(f.lonely));
}

TEST_CASE("gesture: clicking empty space clears the selection") {
    Fixture f = buildFixture();
    Session session(f.doc, f.journal);
    session.setViewport(testViewport());

    const Eigen::Vector2d target = screenOf(session, f.free);
    session.handle(PointerEvent::at(PointerAction::Press, target, session.viewport().view,
                                    Button::Left));
    session.handle(PointerEvent::at(PointerAction::Release, target, session.viewport().view,
                                    Button::Left));
    REQUIRE_FALSE(session.selection().empty());

    const Eigen::Vector2d empty(20.0, 20.0);
    session.handle(
        PointerEvent::at(PointerAction::Press, empty, session.viewport().view, Button::Left));
    session.handle(
        PointerEvent::at(PointerAction::Release, empty, session.viewport().view, Button::Left));
    CHECK(session.selection().empty());
}

TEST_CASE("gesture: a drag moves the grabbed point and the constraint holds") {
    Fixture f = buildFixture();
    Session session(f.doc, f.journal);
    session.setViewport(testViewport());

    const Eigen::Vector2d from = screenOf(session, f.free);
    dragGesture(session, from, from + Eigen::Vector2d(60.0, -40.0));

    const Point moved = *session.pose().point(f.free);
    // Horizontality held: the free end tracked x and stayed level with the pin.
    CHECK(moved.x > 100.0);
    CHECK(moved.y == doctest::Approx(0.0));
    // The pin held.
    CHECK(session.pose().point(f.anchor)->x == doctest::Approx(-100.0));
}

TEST_CASE("gesture: drag locality — unconnected geometry is bit-unchanged") {
    // The invariant, stated in bits: geometry outside the dragged component
    // cannot move, so its seeds come back byte-identical.
    Fixture f = buildFixture();
    Session session(f.doc, f.journal);
    session.setViewport(testViewport());

    const auto before = f.doc.entities().find(f.lonely)->seeds;
    const Eigen::Vector2d from = screenOf(session, f.free);
    dragGesture(session, from, from + Eigen::Vector2d(120.0, 90.0));

    CHECK(f.doc.entities().find(f.lonely)->seeds == before);
}

TEST_CASE("gesture: release commits what is on screen and nothing springs back") {
    Fixture f = buildFixture();
    Session session(f.doc, f.journal);
    session.setViewport(testViewport());

    const Eigen::Vector2d from = screenOf(session, f.free);
    dragGesture(session, from, from + Eigen::Vector2d(75.0, 0.0));

    // What the pose showed at release is what the document now stores.
    const Point onScreen = *session.pose().point(f.free);
    const auto committed = f.doc.entities().find(f.free)->seeds;
    CHECK(committed[0] == onScreen.x);
    CHECK(committed[1] == onScreen.y);
    CHECK(committed[0] > 100.0);
}

TEST_CASE("gesture: undo restores the pre-drag bytes exactly") {
    Fixture f = buildFixture();
    const std::string before = serialize(f.doc);

    Session session(f.doc, f.journal);
    session.setViewport(testViewport());
    const Eigen::Vector2d from = screenOf(session, f.free);
    dragGesture(session, from, from + Eigen::Vector2d(90.0, 30.0));
    REQUIRE(serialize(f.doc) != before);

    session.handle(Key::Undo);
    // Byte-identical, not merely close: a drag is one step and undo restores
    // what was seen.
    CHECK(serialize(f.doc) == before);
}

TEST_CASE("gesture: a drag that moves nothing journals nothing") {
    Fixture f = buildFixture();
    Session session(f.doc, f.journal);
    session.setViewport(testViewport());

    const Eigen::Vector2d from = screenOf(session, f.free);
    // Below the drag threshold: this is a click, not a drag.
    dragGesture(session, from, from + Eigen::Vector2d(1.0, 1.0), 2);
    CHECK_FALSE(f.journal.canUndo());
}

TEST_CASE("gesture: saturation — geometry rides the boundary and names its resistance") {
    // Pull the free end straight up. Horizontality forbids it, so the cursor
    // outruns the geometry and the constraints doing the resisting must be
    // reported rather than the drag simply refusing.
    Fixture f = buildFixture();
    Session session(f.doc, f.journal);
    session.setViewport(testViewport());

    const Eigen::Vector2d from = screenOf(session, f.free);
    const Viewport &v = session.viewport();
    session.handle(
        PointerEvent::at(PointerAction::Press, from, v.view, Button::Left));
    for(int i = 1; i <= 12; i++) {
        session.handle(PointerEvent::at(PointerAction::Move,
                                        from + Eigen::Vector2d(0.0, -10.0 * i), v.view,
                                        Button::Left));
    }

    CHECK(session.presentation().saturated);
    // Resistance with attribution: the user is told what is pushing back.
    CHECK_FALSE(session.presentation().resisting.empty());
    for(ConstraintId id : session.presentation().resisting) {
        CHECK(f.doc.constraints().contains(id));
    }
    // And the geometry stayed legal throughout rather than tearing free.
    CHECK(session.pose().point(f.free)->y == doctest::Approx(0.0));

    session.handle(PointerEvent::at(PointerAction::Release,
                                    from + Eigen::Vector2d(0.0, -120.0), v.view, Button::Left));
    CHECK_FALSE(session.presentation().saturated);
    CHECK(session.presentation().resisting.empty());
}

TEST_CASE("gesture: an unsaturated drag reports no resistance") {
    Fixture f = buildFixture();
    Session session(f.doc, f.journal);
    session.setViewport(testViewport());

    const Eigen::Vector2d from = screenOf(session, f.free);
    // Along the constraint, which the geometry can follow exactly.
    dragGesture(session, from, from + Eigen::Vector2d(50.0, 0.0));
    CHECK_FALSE(session.presentation().saturated);
    CHECK(session.presentation().resisting.empty());
}

TEST_CASE("gesture: dragging a circle's rim resizes it around a fixed centre") {
    // A circle owns exactly one parameter, its radius, and its centre is a
    // point dragged by grabbing the point. So grabbing the circle itself can
    // only mean the rim, and the rim means the radius.
    // Deliberately off-origin and dragged on a diagonal, so no coordinate of
    // the cursor coincides with the radius it should produce. A rim drag that
    // assigned the cursor's x to the radius would pass a due-east drag from a
    // circle centred on the origin, which is the shape of the bug this pins.
    Document doc;
    UndoJournal journal;
    const EntityId centre = addPoint(doc, 100.0, 50.0);
    const EntityId circle = addCircle(doc, centre, 40.0);
    REQUIRE(circle.valid());

    Session session(doc, journal);
    session.setViewport(testViewport());
    REQUIRE(session.pose().radius(circle) == doctest::Approx(40.0));

    const double diagonal = std::sqrt(0.5);
    auto onRay = [&](double distance) {
        return Point{100.0 + distance * diagonal, 50.0 + distance * diagonal};
    };
    const Viewport &v = session.viewport();
    dragGesture(session, v.view.toScreen(onRay(40.0)), v.view.toScreen(onRay(75.0)));

    CHECK(session.pose().radius(circle) == doctest::Approx(75.0));
    // The centre is a parameter of its own and nothing asked it to move.
    CHECK(session.pose().point(centre)->x == doctest::Approx(100.0));
    CHECK(session.pose().point(centre)->y == doctest::Approx(50.0));
    // Release commits what is on screen, radius included.
    CHECK(doc.entities().find(circle)->seeds[0] == doctest::Approx(75.0));
    // A circle owns one parameter. The second slot is not scratch space for a
    // drag to leave a cursor coordinate in: junk there survives the commit,
    // serializes, and compares unequal to a document that means the same thing.
    CHECK(doc.entities().find(circle)->seeds[1] == 0.0);
}

TEST_CASE("gesture: a dimensioned radius resists the rim and says so") {
    // The same saturation contract points already have, on the one other
    // parameter an entity can own: the cursor outruns the rim, the geometry
    // stays legal, and the dimension holding it is named.
    Document doc;
    UndoJournal journal;
    const EntityId centre = addPoint(doc, 100.0, 50.0);
    const EntityId circle = addCircle(doc, centre, 40.0);
    addConstraint(doc, ConstraintKind::Pin, {centre});
    const ConstraintId dimension =
        addConstraint(doc, ConstraintKind::Radius, {circle}, Slot(40.0));
    REQUIRE(dimension.valid());

    Session session(doc, journal);
    session.setViewport(testViewport());

    const Viewport &v = session.viewport();
    const double diagonal = std::sqrt(0.5);
    const Eigen::Vector2d from =
        v.view.toScreen(Point{100.0 + 40.0 * diagonal, 50.0 + 40.0 * diagonal});
    session.handle(PointerEvent::at(PointerAction::Press, from, v.view, Button::Left));
    for(int i = 1; i <= 12; i++) {
        session.handle(PointerEvent::at(
            PointerAction::Move, from + Eigen::Vector2d(10.0 * i * diagonal, -10.0 * i * diagonal),
            v.view, Button::Left));
    }

    CHECK(session.presentation().saturated);
    CHECK(session.pose().radius(circle) == doctest::Approx(40.0));
    const auto &resisting = session.presentation().resisting;
    CHECK(std::find(resisting.begin(), resisting.end(), dimension) != resisting.end());

    // And a drag the geometry refused must leave nothing behind: the radius is
    // where the dimension holds it and the unused slot is untouched.
    session.handle(PointerEvent::at(PointerAction::Release,
                                    from + Eigen::Vector2d(120.0 * diagonal, -120.0 * diagonal),
                                    v.view, Button::Left));
    CHECK(doc.entities().find(circle)->seeds[0] == doctest::Approx(40.0));
    CHECK(doc.entities().find(circle)->seeds[1] == 0.0);
}

TEST_CASE("gesture: movement outside the viewport raises a ripple") {
    // Off-screen consequence without indication is how parametric tools lose
    // trust. A long segment whose far end leaves the viewport must say so.
    Document doc;
    UndoJournal journal;
    const EntityId anchor = addPoint(doc, 0.0, 0.0);
    const EntityId far = addPoint(doc, 380.0, 0.0);
    const EntityId segment = addSegment(doc, anchor, far);
    addConstraint(doc, ConstraintKind::Horizontal, {segment});
    addConstraint(doc, ConstraintKind::PointPointDistance, {anchor, far}, Slot(380.0));

    Session session(doc, journal);
    session.setViewport(testViewport());

    // The far end starts just inside the right edge. Drag the near end right,
    // and the fixed distance pushes the far end past it — a consequence the
    // user cannot see, which is exactly what has to be announced.
    const Eigen::Vector2d from = screenOf(session, anchor);
    REQUIRE(session.viewport().contains(*session.pose().point(far)));

    const Viewport &v = session.viewport();
    session.handle(PointerEvent::at(PointerAction::Press, from, v.view, Button::Left));
    bool rippled = false;
    for(int i = 1; i <= 10; i++) {
        session.handle(PointerEvent::at(PointerAction::Move,
                                        from + Eigen::Vector2d(12.0 * i, 0.0), v.view,
                                        Button::Left));
        rippled = rippled || session.presentation().rippledOffScreen;
    }
    CHECK(rippled);
    CHECK_FALSE(session.viewport().contains(*session.pose().point(far)));
}

TEST_CASE("gesture: delete reports what it took with it") {
    // There is no "cannot delete: in use" anywhere in the tool. The answer to a
    // deletion is a bigger deletion, counted.
    Fixture f = buildFixture();
    Session session(f.doc, f.journal);
    session.setViewport(testViewport());

    const Eigen::Vector2d target = screenOf(session, f.free);
    const Viewport &v = session.viewport();
    session.handle(PointerEvent::at(PointerAction::Press, target, v.view, Button::Left));
    session.handle(PointerEvent::at(PointerAction::Release, target, v.view, Button::Left));
    REQUIRE_FALSE(session.selection().empty());

    session.handle(Key::Delete);

    CHECK(session.presentation().deletedEntities >= 3);   // two points and the segment
    CHECK(session.presentation().deletedRelations >= 2);  // the pin and the horizontal
    CHECK(f.doc.entities().contains(f.lonely));
    CHECK(session.selection().empty());
}

TEST_CASE("gesture: delete is one undo step") {
    Fixture f = buildFixture();
    const std::string before = serialize(f.doc);

    Session session(f.doc, f.journal);
    session.setViewport(testViewport());
    const Eigen::Vector2d target = screenOf(session, f.free);
    const Viewport &v = session.viewport();
    session.handle(PointerEvent::at(PointerAction::Press, target, v.view, Button::Left));
    session.handle(PointerEvent::at(PointerAction::Release, target, v.view, Button::Left));
    session.handle(Key::Delete);
    REQUIRE(serialize(f.doc) != before);

    session.handle(Key::Undo);
    CHECK(serialize(f.doc) == before);
}

TEST_CASE("gesture: a marquee catches what it wholly contains") {
    Fixture f = buildFixture();
    Session session(f.doc, f.journal);
    session.setViewport(testViewport());
    const Viewport &v = session.viewport();

    // A box around the segment but nowhere near the lonely point.
    const Eigen::Vector2d from(250.0, 250.0);
    const Eigen::Vector2d to(560.0, 350.0);
    session.handle(PointerEvent::at(PointerAction::Press, from, v.view, Button::Left));
    session.handle(PointerEvent::at(PointerAction::Move, (from + to) * 0.5, v.view,
                                    Button::Left));
    CHECK(session.presentation().marqueeActive);
    session.handle(PointerEvent::at(PointerAction::Move, to, v.view, Button::Left));
    session.handle(PointerEvent::at(PointerAction::Release, to, v.view, Button::Left));

    CHECK(session.selection().contains(f.anchor));
    CHECK(session.selection().contains(f.free));
    CHECK(session.selection().contains(f.segment));
    CHECK_FALSE(session.selection().contains(f.lonely));
    CHECK_FALSE(session.presentation().marqueeActive);
}

TEST_CASE("gesture: shift-click extends the selection") {
    Fixture f = buildFixture();
    Session session(f.doc, f.journal);
    session.setViewport(testViewport());
    const Viewport &v = session.viewport();

    const Eigen::Vector2d first = screenOf(session, f.free);
    session.handle(PointerEvent::at(PointerAction::Press, first, v.view, Button::Left));
    session.handle(PointerEvent::at(PointerAction::Release, first, v.view, Button::Left));
    const size_t after = session.selection().size();

    const Eigen::Vector2d second = screenOf(session, f.lonely);
    session.handle(PointerEvent::at(PointerAction::Press, second, v.view, Button::Left,
                                    Modifier::Shift));
    session.handle(PointerEvent::at(PointerAction::Release, second, v.view, Button::Left,
                                    Modifier::Shift));

    CHECK(session.selection().size() == after + 1);
    CHECK(session.selection().contains(f.lonely));
}

TEST_CASE("gesture: escape lands on the home state") {
    Fixture f = buildFixture();
    Session session(f.doc, f.journal);
    session.setViewport(testViewport());
    const Viewport &v = session.viewport();

    const Eigen::Vector2d target = screenOf(session, f.free);
    session.handle(PointerEvent::at(PointerAction::Press, target, v.view, Button::Left));
    session.handle(PointerEvent::at(PointerAction::Release, target, v.view, Button::Left));
    REQUIRE_FALSE(session.selection().empty());

    // However deep the selection went, Esc eventually clears it.
    for(int i = 0; i < 4; i++) session.handle(Key::Escape);
    CHECK(session.selection().empty());
    CHECK(session.selection().depth() == 0);
}

TEST_CASE("gesture: hover reports what a click would take") {
    // What the user sees under the cursor and what a press would select must be
    // the same thing, or picking is a guess.
    Fixture f = buildFixture();
    Session session(f.doc, f.journal);
    session.setViewport(testViewport());
    const Viewport &v = session.viewport();

    session.handle(PointerEvent::at(PointerAction::Move, screenOf(session, f.free), v.view));
    CHECK(session.presentation().hovered == f.free);

    session.handle(PointerEvent::at(PointerAction::Move, Eigen::Vector2d(5.0, 5.0), v.view));
    CHECK_FALSE(session.presentation().hovered.valid());
}

TEST_CASE("gesture: attribution survives a hard pull, at any zoom") {
    // Pulling harder must not cost the user the explanation. The earlier ratio
    // test lost it: freeing a rotational degree of freedom buys the point a
    // roughly fixed distance, which shrinks as a fraction of the gap the further
    // the cursor runs, so the constraint doing the resisting quietly dropped out
    // of the report exactly when the user was pushing hardest to discover it.
    //
    // Zoom is swept too, because the floor is a fraction of the geometry rather
    // than of the screen: the same shape must attribute the same way however
    // close the user has zoomed in.
    for(double scale : {0.25, 1.0, 4.0}) {
        for(double distance : {40.0, 120.0, 400.0, 1200.0}) {
            Document doc = demoDocument(1.618);
            UndoJournal journal;
            Session session(doc, journal);
            session.setViewport(testViewport(scale));

            // The upper-right end, held vertically by horizontality alone.
            const EntityId a1 = doc.entities().records()[1].id;
            const Eigen::Vector2d from = screenOf(session, a1);
            const Viewport &v = session.viewport();

            session.handle(PointerEvent::at(PointerAction::Press, from, v.view, Button::Left));
            for(int i = 1; i <= 12; i++) {
                session.handle(PointerEvent::at(PointerAction::Move,
                                                from + Eigen::Vector2d(0.0, -distance * i / 12.0),
                                                v.view, Button::Left));
            }

            REQUIRE(session.presentation().saturated);
            // Horizontality is what forbids this drag, so it must be named —
            // however far the cursor went and however far in the view is zoomed.
            bool namesHorizontal = false;
            for(ConstraintId id : session.presentation().resisting) {
                if(doc.constraints().find(id)->kind == ConstraintKind::Horizontal) {
                    namesHorizontal = true;
                }
            }
            INFO("zoom ", scale, ", pulled ", distance, " px");
            CHECK(namesHorizontal);
        }
    }
}

TEST_CASE("gesture: a drag the solver cannot follow holds the last legal pose") {
    // The failure mode this pins down: SolveSpace leaves the parameters at the
    // seeds it was handed when it does not converge, and the seed a drag hands
    // it is the cursor. Read back naively that looks like a perfect track — zero
    // gap, no saturation — while the geometry has walked through its own
    // constraints, and release would commit the violation as seeds.
    //
    // Pull the lower segment's free end far past what the length ratio allows.
    Document doc = demoDocument(1.618);
    UndoJournal journal;
    Session session(doc, journal);
    session.setViewport(testViewport());

    const EntityId a0 = doc.entities().records()[0].id;
    const EntityId a1 = doc.entities().records()[1].id;
    const EntityId b0 = doc.entities().records()[2].id;
    const EntityId b1 = doc.entities().records()[3].id;

    auto ratioOf = [&](const Pose &pose) {
        const Point pa0 = *pose.point(a0), pa1 = *pose.point(a1);
        const Point pb0 = *pose.point(b0), pb1 = *pose.point(b1);
        return std::hypot(pa1.x - pa0.x, pa1.y - pa0.y) /
               std::hypot(pb1.x - pb0.x, pb1.y - pb0.y);
    };

    const Eigen::Vector2d from = screenOf(session, b1);
    const Viewport &v = session.viewport();
    session.handle(PointerEvent::at(PointerAction::Press, from, v.view, Button::Left));
    for(int i = 1; i <= 20; i++) {
        session.handle(PointerEvent::at(PointerAction::Move,
                                        from + Eigen::Vector2d(20.0 * i, 0.0), v.view,
                                        Button::Left));
        // Every frame is legal, not merely the last one.
        CHECK(ratioOf(session.pose()) == doctest::Approx(1.618));
    }
    // Unreachable is saturated, never free.
    CHECK(session.presentation().saturated);

    session.handle(PointerEvent::at(PointerAction::Release,
                                    from + Eigen::Vector2d(400.0, 0.0), v.view, Button::Left));
    // And what got committed is the legal pose, not the cursor's wish.
    CHECK(ratioOf(session.pose()) == doctest::Approx(1.618));
}

TEST_CASE("gesture: a drag stays within the frame budget") {
    // The interactive loop's variable term, measured on the path that actually
    // runs it rather than in isolation.
    Fixture f = buildFixture();
    Session session(f.doc, f.journal);
    session.setViewport(testViewport());

    const Eigen::Vector2d from = screenOf(session, f.free);
    dragGesture(session, from, from + Eigen::Vector2d(100.0, 0.0), 30);
    CHECK(session.presentation().solveMicroseconds > 0.0);
    // Generous: this is a Debug build on a shared machine, and the real gate
    // lives in the bench harness.
    CHECK(session.presentation().solveMicroseconds < 16000.0);
}

TEST_CASE("gesture: a double click descends, and Esc walks back up") {
    // Object versus component is selection depth, not mode. Stage 3 committed
    // to this and the machinery was built, but nothing carried a click count so
    // depth was always zero in the app and Esc-ascend was unreachable.
    Fixture f = buildFixture();
    Session session(f.doc, f.journal);
    session.setViewport(testViewport());

    const Viewport &v = session.viewport();
    const Eigen::Vector2d target = screenOf(session, f.free);
    auto press = [&](int clicks) {
        session.handle(PointerEvent::at(PointerAction::Press, target, v.view, Button::Left,
                                        Modifier::None, clicks));
        session.handle(PointerEvent::at(PointerAction::Release, target, v.view, Button::Left));
    };

    // One click selects the whole shape.
    press(1);
    CHECK(session.selection().depth() == 0);
    CHECK(session.selection().contains(f.segment));
    CHECK(session.selection().contains(f.anchor));

    // A second descends to the edges.
    press(2);
    CHECK(session.selection().depth() == 1);
    CHECK(session.selection().contains(f.segment));
    CHECK_FALSE(session.selection().contains(f.anchor));

    // And again to the points.
    press(2);
    CHECK(session.selection().depth() == 2);
    CHECK(session.selection().contains(f.anchor));
    CHECK_FALSE(session.selection().contains(f.segment));

    // Esc ascends a rung at a time rather than clearing.
    session.handle(Key::Escape);
    CHECK(session.selection().depth() == 1);
    CHECK_FALSE(session.selection().empty());
    session.handle(Key::Escape);
    CHECK(session.selection().depth() == 0);
    CHECK_FALSE(session.selection().empty());
    // Only at the home state does Esc clear.
    session.handle(Key::Escape);
    CHECK(session.selection().empty());
}

TEST_CASE("gesture: a click count survives the script round trip") {
    // A recording that lost the second click would replay as a single one and
    // land the user at a different depth than the session it recorded.
    Fixture f = buildFixture();
    Session session(f.doc, f.journal);
    ScriptRecorder recorder;
    session.setRecorder(&recorder);
    session.setViewport(testViewport());

    const Viewport &v = session.viewport();
    const Eigen::Vector2d target = screenOf(session, f.free);
    session.handle(PointerEvent::at(PointerAction::Press, target, v.view, Button::Left));
    session.handle(PointerEvent::at(PointerAction::Release, target, v.view, Button::Left));
    session.handle(PointerEvent::at(PointerAction::Press, target, v.view, Button::Left,
                                    Modifier::None, 2));
    session.handle(PointerEvent::at(PointerAction::Release, target, v.view, Button::Left));
    REQUIRE(session.selection().depth() == 1);

    GestureScript script;
    script.document = buildFixture().doc;
    script.steps = recorder.steps();
    const std::string text = serializeScript(script);
    CHECK(text.find("clicks=2") != std::string::npos);
    // An ordinary press says nothing, so a script of plain clicks is the file
    // it was before the field existed.
    CHECK(text.find("clicks=1") == std::string::npos);

    GestureScript parsed;
    REQUIRE(parseScript(text, parsed).ok);
    CHECK(serializeScript(parsed) == text);

    Document replayedDoc = parsed.document;
    UndoJournal replayedJournal;
    Session replayed(replayedDoc, replayedJournal);
    replay(replayed, parsed);
    CHECK(replayed.selection().depth() == 1);
}

TEST_CASE("gesture: one broken component does not un-solve the canvas") {
    // PRINCIPLES: the document stays editable, geometry holds the last feasible
    // solution, and there are only states with more or less diagnostic
    // adornment. Solving the document as one system broke all three at once —
    // a single contradictory relation blanked every healthy component's display
    // back to its committed seeds, so the whole canvas stopped showing solved
    // geometry because one corner of it was over-constrained.
    Document doc;
    UndoJournal journal;

    // A healthy component: a horizontal segment with its left end pinned and
    // its right end drawn off-axis, so solving visibly moves it.
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 100.0, 40.0);
    const EntityId healthy = addSegment(doc, a, b);
    addConstraint(doc, ConstraintKind::Pin, {a});
    addConstraint(doc, ConstraintKind::Horizontal, {healthy});

    // A broken one, far away and unconnected: both ends pinned where they are,
    // and a distance that contradicts the pins.
    const EntityId c = addPoint(doc, 500.0, 0.0);
    const EntityId d = addPoint(doc, 600.0, 0.0);
    addSegment(doc, c, d);
    addConstraint(doc, ConstraintKind::Pin, {c});
    addConstraint(doc, ConstraintKind::Pin, {d});
    addConstraint(doc, ConstraintKind::PointPointDistance, {c, d}, Slot(250.0));

    Session session(doc, journal);
    session.setViewport(testViewport());

    // The healthy component is solved: its free end came down onto the axis.
    const Pose pose = session.pose();
    CHECK(pose.point(b)->y == doctest::Approx(0.0));
    CHECK(pose.point(b)->x == doctest::Approx(100.0));
    // And the pinned end of it held.
    CHECK(pose.point(a)->x == doctest::Approx(0.0));

    // The broken component holds what it had rather than showing nonsense.
    CHECK(pose.point(c)->x == doctest::Approx(500.0));
    CHECK(pose.point(d)->x == doctest::Approx(600.0));

    // The readout still says something is wrong, because that is the one thing
    // the user has to be able to see.
    CHECK_FALSE(session.presentation().status == SolveStatus::Okay);

    // The document is still editable, and editing the healthy side still works.
    const Eigen::Vector2d from = screenOf(session, b);
    dragGesture(session, from, from + Eigen::Vector2d(40.0, 0.0));
    CHECK(session.pose().point(b)->x > 100.0);
    CHECK(session.pose().point(b)->y == doctest::Approx(0.0));
}

TEST_CASE("gesture: degrees of freedom are summed over the components that solve") {
    // Two independent shapes are two independent systems, and the readout is
    // over the document rather than over whichever one was looked at last.
    Document doc;
    UndoJournal journal;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 100.0, 0.0);
    addSegment(doc, a, b);
    addConstraint(doc, ConstraintKind::Pin, {a});
    // Four parameters, two taken by the pin: two left.

    const EntityId c = addPoint(doc, 400.0, 0.0);
    const EntityId d = addPoint(doc, 500.0, 0.0);
    addSegment(doc, c, d);
    addConstraint(doc, ConstraintKind::Pin, {c});

    Session session(doc, journal);
    session.setViewport(testViewport());
    CHECK(session.presentation().status == SolveStatus::Okay);
    CHECK(session.presentation().dof == 4);
}
