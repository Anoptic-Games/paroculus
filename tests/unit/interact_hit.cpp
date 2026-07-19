#include <doctest/doctest.h>

#include <algorithm>

#include "core/topology.h"
#include "interact/drag.h"
#include "interact/hit.h"
#include "support/build.h"

using namespace paroculus;
using paroculus::test::addPoint;
using paroculus::test::addSegment;

namespace {

// 1:1 with the origin at the viewport centre and Y flipped, so a document unit
// is a pixel and the two regimes can be reasoned about separately.
ViewTransform unitView(double scale = 1.0) {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(400.0, 300.0));
    m.scale(Eigen::Vector2d(scale, -scale));
    return ViewTransform(m);
}

}  // namespace

TEST_CASE("a point beats the edge it sits on") {
    // A vertex is smaller, harder to hit, and almost always what the user meant
    // when both are under the cursor.
    Document doc;
    const EntityId a = addPoint(doc, -50.0, 0.0);
    const EntityId b = addPoint(doc, 50.0, 0.0);
    addSegment(doc, a, b);

    const Pose pose(doc);
    SpatialIndex index;
    index.rebuild(pose);
    const ViewTransform view = unitView();
    const HitPolicy policy;

    const std::optional<Hit> hit =
        hitTest(pose, index, view, view.toScreen(Point{50.0, 0.0}), policy);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == b);
    CHECK(hit->kind == HitKind::Point);
}

TEST_CASE("the edge is hit away from its endpoints") {
    Document doc;
    const EntityId a = addPoint(doc, -50.0, 0.0);
    const EntityId b = addPoint(doc, 50.0, 0.0);
    const EntityId segment = addSegment(doc, a, b);

    const Pose pose(doc);
    SpatialIndex index;
    index.rebuild(pose);
    const ViewTransform view = unitView();

    const std::optional<Hit> hit =
        hitTest(pose, index, view, view.toScreen(Point{0.0, 1.0}), HitPolicy{});
    REQUIRE(hit.has_value());
    CHECK(hit->entity == segment);
    CHECK(hit->kind == HitKind::Edge);
}

TEST_CASE("construction geometry is demoted but still pickable") {
    // A guide participates in constraints identically and is only presented
    // differently, so demoting rather than excluding keeps it reachable when it
    // is genuinely what the user aimed at.
    Document doc;
    const EntityId a = addPoint(doc, -50.0, 0.0);
    const EntityId b = addPoint(doc, 50.0, 0.0);
    const EntityId normal = addSegment(doc, a, b);

    const EntityId c = addPoint(doc, -50.0, 0.5);
    const EntityId d = addPoint(doc, 50.0, 0.5);
    const EntityId guide = addSegment(doc, c, d);
    EntityRecord asGuide = *doc.entities().find(guide);
    asGuide.role = Role::Construction;
    REQUIRE(doc.apply(SetRecord<EntityRecord>{asGuide}).ok());

    const Pose pose(doc);
    SpatialIndex index;
    index.rebuild(pose);
    const ViewTransform view = unitView();

    // Both edges are within reach; the ordinary one wins.
    const std::vector<Hit> hits =
        hitTestAll(pose, index, view, view.toScreen(Point{0.0, 0.4}), HitPolicy{});
    REQUIRE(hits.size() >= 2);
    CHECK(hits.front().entity == normal);
    // But the guide is still in the list, not filtered out.
    CHECK(std::any_of(hits.begin(), hits.end(),
                      [&](const Hit &h) { return h.entity == guide; }));
}

TEST_CASE("a selected entity is favoured over an unselected one") {
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 0.0, 0.0);  // exactly coincident

    const Pose pose(doc);
    SpatialIndex index;
    index.rebuild(pose);
    const ViewTransform view = unitView();
    const Eigen::Vector2d cursor = view.toScreen(Point{0.0, 0.0});

    // With nothing selected the tie breaks on ID, so hover never flickers.
    CHECK(hitTest(pose, index, view, cursor, HitPolicy{})->entity == a);
    // With b selected, b wins.
    CHECK(hitTest(pose, index, view, cursor, HitPolicy{}, {b})->entity == b);
}

TEST_CASE("tolerances are pixels, so zoom changes what is reachable") {
    // The same sloppy gesture should infer coarser relations zoomed out than
    // zoomed in. That is the whole reason radii live in pixels.
    Document doc;
    addPoint(doc, 0.0, 0.0);

    const Pose pose(doc);
    SpatialIndex index;
    index.rebuild(pose);
    const HitPolicy policy;

    // Zoomed in: 8 document units is far in pixels, so this misses.
    const ViewTransform zoomedIn = unitView(4.0);
    CHECK_FALSE(
        hitTest(pose, index, zoomedIn, zoomedIn.toScreen(Point{8.0, 0.0}), policy).has_value());

    // Zoomed out: the same 8 document units is close in pixels, so it hits.
    const ViewTransform zoomedOut = unitView(0.25);
    CHECK(hitTest(pose, index, zoomedOut, zoomedOut.toScreen(Point{8.0, 0.0}), policy)
              .has_value());
}

TEST_CASE("nothing within tolerance is nothing, not the nearest thing") {
    Document doc;
    addPoint(doc, 0.0, 0.0);

    const Pose pose(doc);
    SpatialIndex index;
    index.rebuild(pose);
    const ViewTransform view = unitView();

    CHECK_FALSE(
        hitTest(pose, index, view, view.toScreen(Point{500.0, 500.0}), HitPolicy{}).has_value());
}

TEST_CASE("the index shortlist is id-ordered and deduplicated") {
    // A segment spanning several cells must appear once, and iteration order
    // must be a function of the document.
    Document doc;
    const EntityId a = addPoint(doc, -200.0, 0.0);
    const EntityId b = addPoint(doc, 200.0, 0.0);
    addSegment(doc, a, b);

    const Pose pose(doc);
    SpatialIndex index;
    index.rebuild(pose);

    const std::vector<EntityId> shortlist = index.near(Point{0.0, 0.0}, 500.0);
    CHECK(std::is_sorted(shortlist.begin(), shortlist.end()));
    CHECK(std::adjacent_find(shortlist.begin(), shortlist.end()) == shortlist.end());
    CHECK(shortlist.size() == 3);
}

TEST_CASE("the marquee takes what it wholly contains") {
    // A marquee that grabs what it merely grazes is how a selection ends up
    // holding things the user never saw.
    Document doc;
    const EntityId inside = addPoint(doc, 0.0, 0.0);
    const EntityId a = addPoint(doc, -10.0, -10.0);
    const EntityId b = addPoint(doc, 10.0, 10.0);
    const EntityId contained = addSegment(doc, a, b);

    const EntityId far = addPoint(doc, 300.0, 0.0);
    const EntityId straddling = addSegment(doc, b, far);

    const Pose pose(doc);
    const ViewTransform view = unitView();

    const std::vector<EntityId> caught =
        marquee(pose, view, view.toScreen(Point{-50.0, -50.0}),
                view.toScreen(Point{50.0, 50.0}));

    CHECK(std::find(caught.begin(), caught.end(), inside) != caught.end());
    CHECK(std::find(caught.begin(), caught.end(), contained) != caught.end());
    CHECK(std::find(caught.begin(), caught.end(), far) == caught.end());
    // One end in, one end out: not taken.
    CHECK(std::find(caught.begin(), caught.end(), straddling) == caught.end());
}

TEST_CASE("the marquee takes the curves it wholly contains") {
    // Written for points and segments and never extended, the marquee took a
    // circle's centre and left the circle — a selection holding half of what
    // the user dragged a box around.
    Document doc;
    const EntityId centre = addPoint(doc, 0.0, 0.0);
    const EntityId circle = paroculus::test::addCircle(doc, centre, 20.0);
    const EntityId wideCentre = addPoint(doc, 0.0, 0.0);
    const EntityId wide = paroculus::test::addCircle(doc, wideCentre, 200.0);

    // A quarter arc in the upper right, sweeping counter-clockwise from due
    // west to due south of its centre. Its swept path stays inside the box; the
    // circle it belongs to reaches out to (70, 70) and does not.
    const EntityId arcCentre = addPoint(doc, 40.0, 40.0);
    const EntityId arcStart = addPoint(doc, 10.0, 40.0);
    const EntityId arcEnd = addPoint(doc, 40.0, 10.0);
    const EntityId arc = paroculus::test::addArc(doc, arcCentre, arcStart, arcEnd);

    const Pose pose(doc);
    const ViewTransform view = unitView();
    const std::vector<EntityId> caught = marquee(
        pose, view, view.toScreen(Point{-50.0, -50.0}), view.toScreen(Point{50.0, 50.0}));
    auto took = [&](EntityId id) {
        return std::find(caught.begin(), caught.end(), id) != caught.end();
    };

    CHECK(took(circle));
    // Centre inside, rim far outside: grazed, not contained.
    CHECK(!took(wide));
    // Exact, not the enclosing square. Here the bound is the test — there is no
    // shortlist a loose one gets refined against, so a square would refuse this.
    CHECK(took(arc));
}

TEST_CASE("hit priority is table-driven and replaceable") {
    // The policy stage 3's discovery window is expected to rewrite. Pinned here
    // so a rewrite is a deliberate corpus update rather than a silent drift.
    HitCandidate point{EntityId(1), HitKind::Point, Role::Normal, false, 5.0};
    HitCandidate edge{EntityId(2), HitKind::Edge, Role::Normal, false, 1.0};
    HitCandidate guide{EntityId(3), HitKind::Edge, Role::Construction, false, 0.0};
    HitCandidate selectedEdge{EntityId(4), HitKind::Edge, Role::Normal, true, 1.0};

    // A point beats a closer edge.
    CHECK(hitBeats(point, edge));
    // An ordinary edge beats a closer guide.
    CHECK(hitBeats(edge, guide));
    // A selected edge beats an equidistant unselected one.
    CHECK(hitBeats(selectedEdge, edge));
    // Distance breaks ties within a tier.
    HitCandidate nearPoint{EntityId(5), HitKind::Point, Role::Normal, false, 1.0};
    CHECK(hitBeats(nearPoint, point));
}

TEST_CASE("a multi-selection drag holds every selected parameter") {
    // PRINCIPLES: multi-selection drags put all selected parameters in the set.
    // The set is the solver's soft hint — held, not targeted — so only the grab
    // is asked to be at the cursor and the rest are marked so what has to give
    // gives in the geometry the user did not select.
    Document doc;
    const EntityId a = paroculus::test::addPoint(doc, 0.0, 0.0);
    const EntityId b = paroculus::test::addPoint(doc, 100.0, 0.0);
    const EntityId segment = paroculus::test::addSegment(doc, a, b);
    // A second, unconnected shape: selected, but not in this solve.
    const EntityId far = paroculus::test::addPoint(doc, 900.0, 900.0);

    Topology topology(doc);
    HitPolicy policy;

    SUBCASE("the whole selection is held") {
        const std::optional<DragSession> drag =
            DragSession::begin(doc, topology, b, {a, b, segment}, policy);
        REQUIRE(drag.has_value());
        // Grab first, so the anchor is unambiguous.
        REQUIRE_FALSE(drag->dragged().empty());
        CHECK(drag->dragged().front() == b);
        CHECK(std::find(drag->dragged().begin(), drag->dragged().end(), a) !=
              drag->dragged().end());
        // A segment owns no parameters of its own, so there is nothing of it to
        // hold: it moves by its endpoints, which are already in the set.
        CHECK(std::find(drag->dragged().begin(), drag->dragged().end(), segment) ==
              drag->dragged().end());
    }

    SUBCASE("a selection outside the dragged component is not in the solve") {
        // Locality outranks it. Nothing connects the two, so holding the far
        // point would only widen the solve for no possible effect.
        const std::optional<DragSession> drag =
            DragSession::begin(doc, topology, b, {a, b, far}, policy);
        REQUIRE(drag.has_value());
        CHECK(std::find(drag->dragged().begin(), drag->dragged().end(), far) ==
              drag->dragged().end());
        CHECK_FALSE(drag->context().contains(far));
    }

    SUBCASE("an empty selection still holds the grab") {
        const std::optional<DragSession> drag = DragSession::begin(doc, topology, b, {}, policy);
        REQUIRE(drag.has_value());
        CHECK(drag->dragged().size() == 1);
        CHECK(drag->dragged().front() == b);
    }
}

TEST_CASE("holding a point makes the drag spend its slack elsewhere") {
    // The observable difference the dragged set buys. Two free points joined to
    // a pinned one: dragging the far end has to move something, and which thing
    // moves is what "held" decides.
    Document doc;
    const EntityId anchor = paroculus::test::addPoint(doc, 0.0, 0.0);
    const EntityId middle = paroculus::test::addPoint(doc, 100.0, 0.0);
    const EntityId end = paroculus::test::addPoint(doc, 200.0, 0.0);
    paroculus::test::addConstraint(doc, ConstraintKind::Pin, {anchor});
    paroculus::test::addConstraint(doc, ConstraintKind::PointPointDistance, {anchor, middle},
                                   Slot(100.0));
    paroculus::test::addConstraint(doc, ConstraintKind::PointPointDistance, {middle, end},
                                   Slot(100.0));

    Topology topology(doc);
    HitPolicy policy;
    Viewport viewport;
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(400.0, 300.0));
    m.scale(Eigen::Vector2d(1.0, -1.0));
    viewport.view = ViewTransform(m);
    viewport.width = 800.0;
    viewport.height = 600.0;

    const Point target{140.0, 120.0};
    auto travelOfMiddle = [&](const std::vector<EntityId> &selection) {
        std::optional<DragSession> drag =
            DragSession::begin(doc, topology, end, selection, policy);
        REQUIRE(drag.has_value());
        for(int i = 1; i <= 8; i++) {
            const double t = static_cast<double>(i) / 8.0;
            drag->update(doc, viewport, Point{200.0 + (target.x - 200.0) * t, target.y * t},
                         false);
        }
        const Point moved = *drag->context().point(middle);
        return std::hypot(moved.x - 100.0, moved.y);
    };

    const double freeTravel = travelOfMiddle({end});
    const double heldTravel = travelOfMiddle({end, middle});
    // Strictly less, not merely no more. Equal would mean the set never reached
    // the solver, which is exactly the state this finding was about — and it is
    // the only assertion here that reaches past DragSession into SolveOptions.
    // Held is still a preference and not a pin, so the travel is smaller rather
    // than zero.
    CHECK(heldTravel < freeTravel);
    CHECK(heldTravel > 0.0);
}
