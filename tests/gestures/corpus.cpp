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

#include <cmath>

#include "core/persist.h"
#include "interact/session.h"
#include "support/build.h"

using namespace paroculus;
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
