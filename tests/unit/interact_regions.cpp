// Segments to solid: the flagship equivalence.
//
// Making a closed outline a solid is not a conversion. A region record
// references the cycle of edge IDs; no geometry is copied, no path object is
// synthesized, no constraint is touched. What that buys is tested here — the
// outline keeps operating, dragging a vertex moves the fill because the fill
// has no geometry of its own to go stale, and the inverse is deleting one
// record.
//
// Heal-and-fill is the other half: closure is topological, never visual, so an
// outline that only looks closed is bridged by imposing the missing
// coincidences. Its epsilon motion is the explicit point of the action, which
// is why it is a separate offer and why it reports what it moved.
#include <doctest/doctest.h>

#include <memory>

#include "core/persist.h"
#include "interact/loops.h"
#include "interact/registry.h"
#include "interact/session.h"
#include "interact/surface.h"
#include "support/build.h"

using namespace paroculus;
using namespace paroculus::test;

namespace {

Viewport regionViewport() {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(400.0, 300.0));
    m.scale(Eigen::Vector2d(1.0, -1.0));
    Viewport v;
    v.view = ViewTransform(m);
    v.width = 800.0;
    v.height = 600.0;
    return v;
}

struct Drawing {
    Document doc;
    UndoJournal journal;
    std::unique_ptr<Session> session;

    explicit Drawing(ToolKind tool = ToolKind::Line) {
        session = std::make_unique<Session>(doc, journal);
        session->setViewport(regionViewport());
        session->snapPolicy().gridEnabled = false;
        session->setTool(tool);
    }
    void click(Point p) {
        const Eigen::Vector2d s = session->viewport().view.toScreen(p);
        session->handle(PointerEvent::at(PointerAction::Move, s, session->viewport().view));
        session->handle(
            PointerEvent::at(PointerAction::Press, s, session->viewport().view, Button::Left));
    }
    // Four segments whose last click lands close enough to the first corner for
    // endpoint snapping to declare the coincidence that closes the loop.
    void drawClosedSquare() {
        click(Point{-60.0, -40.0});
        click(Point{60.0, -40.0});
        click(Point{60.0, 40.0});
        click(Point{-60.0, 40.0});
        click(Point{-59.5, -39.5});
    }
};

}  // namespace

TEST_CASE("a closed outline becomes a solid without touching the outline") {
    Drawing d;
    d.drawClosedSquare();
    REQUIRE(d.session->presentation().closedLoop.size() == 4);

    const size_t entitiesBefore = d.doc.entities().records().size();
    const size_t relationsBefore = d.doc.constraints().records().size();
    const std::vector<EntityId> offered = d.session->presentation().closedLoop;

    REQUIRE(invokeAction(*d.session, "region.make-solid"));

    // One record added, and nothing else changed. No geometry copied, no
    // constraint touched: the region references the cycle it was handed.
    REQUIRE(d.doc.regions().records().size() == 1);
    CHECK(d.doc.entities().records().size() == entitiesBefore);
    CHECK(d.doc.constraints().records().size() == relationsBefore);

    const RegionRecord &region = d.doc.regions().records().front();
    CHECK(region.boundary == offered);
    CHECK(d.session->presentation().filled == region.id);

    // And the offer is withdrawn. The loop is still closed, so it stayed lit
    // and a second press stacked an identical region over the first — doubled
    // alpha on screen, and an action that looks like it did nothing.
    CHECK(d.session->presentation().closedLoop.empty());
    CHECK_FALSE(invokeAction(*d.session, "region.make-solid"));
    CHECK(d.doc.regions().records().size() == 1);
}

TEST_CASE("the fill follows the outline because it has no geometry of its own") {
    Drawing d;
    d.drawClosedSquare();
    REQUIRE(invokeAction(*d.session, "region.make-solid"));
    const std::vector<EntityId> boundary = d.doc.regions().records().front().boundary;

    // Where the boundary is before the drag.
    const EntityRecord *edge = d.doc.entities().find(boundary.front());
    REQUIRE(edge != nullptr);
    const EntityId corner = edge->points[0];
    const Point was = *d.session->pose().point(corner);

    // Drag that corner, and only that corner. A drag holds every selected
    // parameter, so grabbing a vertex with the whole square selected asks a
    // rigid body to deform — correct behaviour, and not what this test is
    // about.
    d.session->setTool(ToolKind::Select);
    d.session->select({corner});
    const ViewTransform &view = d.session->viewport().view;
    const Eigen::Vector2d from = view.toScreen(was);
    const Eigen::Vector2d to = view.toScreen(Point{was.x - 30.0, was.y - 25.0});
    // Interpolated, because a drag is a stream of moves and each solve is warm
    // started from the last.
    d.session->handle(PointerEvent::at(PointerAction::Press, from, view, Button::Left));
    for(int i = 1; i <= 8; i++) {
        const double t = static_cast<double>(i) / 8.0;
        d.session->handle(
            PointerEvent::at(PointerAction::Move, from + t * (to - from), view, Button::Left));
    }
    d.session->handle(PointerEvent::at(PointerAction::Release, to, view, Button::Left));

    const Point now = *d.session->pose().point(corner);
    CHECK(std::fabs(now.x - was.x) > 1.0);

    // The region still names the same edges, and there is no second copy of
    // the geometry anywhere for it to have gone stale against.
    CHECK(d.doc.regions().records().front().boundary == boundary);
    CHECK(d.doc.regions().records().size() == 1);
}

TEST_CASE("deleting the region leaves the outline and every constraint untouched") {
    Drawing d;
    d.drawClosedSquare();
    const std::string outlineOnly = serialize(d.doc);

    REQUIRE(invokeAction(*d.session, "region.make-solid"));
    CHECK(serialize(d.doc) != outlineOnly);

    // The inverse is deleting one record, and round-tripping is exact because
    // nothing was translated in either direction.
    REQUIRE(invokeAction(*d.session, "edit.undo"));
    CHECK(sameRecords(d.doc, [&] {
        Document again;
        deserialize(outlineOnly, again);
        return again;
    }()));
}

TEST_CASE("two edges do not enclose anything") {
    // Two straight segments over one pair of vertices pass the degree and
    // connectivity tests and walk closed while enclosing nothing. Harmless
    // while closure only notices; wrong the moment make-solid fills what it is
    // handed.
    Document doc;
    UndoJournal journal;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 40.0, 0.0);
    const EntityId c = addPoint(doc, 0.0, 0.0);
    const EntityId e = addPoint(doc, 40.0, 0.0);
    const EntityId first = addSegment(doc, a, b);
    const EntityId second = addSegment(doc, c, e);
    addConstraint(doc, ConstraintKind::Coincident, {a, c});
    addConstraint(doc, ConstraintKind::Coincident, {b, e});

    Session session(doc, journal);
    session.setViewport(regionViewport());
    session.select({first, second});

    CHECK(session.presentation().closedLoop.empty());
    CHECK_FALSE(invokeAction(session, "region.make-solid"));
    CHECK(doc.regions().records().empty());
}

TEST_CASE("an outline that only looks closed is healed and filled") {
    // Closure is topological, never visual. The run stays open; the gap is
    // bridged by imposing the missing coincidence, and the geometry moves by
    // the epsilon the user already could not see.
    Document doc;
    UndoJournal journal;
    const EntityId p0 = addPoint(doc, 0.0, 0.0);
    const EntityId p1 = addPoint(doc, 60.0, 0.0);
    const EntityId p2 = addPoint(doc, 60.0, 0.0);
    const EntityId p3 = addPoint(doc, 60.0, 45.0);
    const EntityId p4 = addPoint(doc, 60.0, 45.0);
    // The gap: this corner is a little short of where the first one started.
    const EntityId p5 = addPoint(doc, 0.6, 0.5);
    const EntityId e0 = addSegment(doc, p0, p1);
    const EntityId e1 = addSegment(doc, p2, p3);
    const EntityId e2 = addSegment(doc, p4, p5);
    addConstraint(doc, ConstraintKind::Coincident, {p1, p2});
    addConstraint(doc, ConstraintKind::Coincident, {p3, p4});

    Session session(doc, journal);
    session.setViewport(regionViewport());
    session.select({e0, e1, e2});

    // Open, not closed — so make-solid is not on offer and heal-and-fill is.
    CHECK(session.presentation().closedLoop.empty());
    REQUIRE(session.presentation().healableLoop.size() == 3);
    REQUIRE(session.presentation().healableGaps.size() == 1);
    CHECK(session.presentation().healableWidestGap < 2.0);
    CHECK_FALSE(invokeAction(session, "region.make-solid"));

    const double offered = session.presentation().healableWidestGap;
    REQUIRE(invokeAction(session, "region.heal-and-fill"));

    // The joints are shut, the region is attached, and nothing travelled
    // further than the gap it was offered as.
    REQUIRE(doc.regions().records().size() == 1);
    CHECK(doc.regions().records().front().boundary.size() == 3);
    CHECK(session.presentation().impositionMotion <= offered + 1e-9);
    CHECK(session.presentation().impositionMotion > 0.0);

    const Point start = *session.pose().point(p0);
    const Point end = *session.pose().point(p5);
    CHECK(std::hypot(end.x - start.x, end.y - start.y) < 1e-6);

    // One gesture, one undo step: taking it back means the fill and the joints
    // together, not the fill and a drawing that has quietly moved.
    REQUIRE(invokeAction(session, "edit.undo"));
    CHECK(doc.regions().records().empty());
    CHECK(doc.constraints().records().size() == 2);
}

TEST_CASE("a gap wider than the hand is not a gap") {
    Document doc;
    UndoJournal journal;
    const EntityId p0 = addPoint(doc, 0.0, 0.0);
    const EntityId p1 = addPoint(doc, 60.0, 0.0);
    const EntityId p2 = addPoint(doc, 60.0, 0.0);
    const EntityId p3 = addPoint(doc, 60.0, 45.0);
    const EntityId p4 = addPoint(doc, 60.0, 45.0);
    const EntityId p5 = addPoint(doc, 25.0, 20.0);  // nowhere near p0
    const EntityId e0 = addSegment(doc, p0, p1);
    const EntityId e1 = addSegment(doc, p2, p3);
    const EntityId e2 = addSegment(doc, p4, p5);
    addConstraint(doc, ConstraintKind::Coincident, {p1, p2});
    addConstraint(doc, ConstraintKind::Coincident, {p3, p4});

    Session session(doc, journal);
    session.setViewport(regionViewport());
    session.select({e0, e1, e2});

    CHECK(session.presentation().healableLoop.empty());
    CHECK_FALSE(invokeAction(session, "region.heal-and-fill"));
    CHECK(doc.regions().records().empty());
}

TEST_CASE("an already closed loop is filled rather than healed") {
    // The two offers are never both live. One moves nothing and the other moves
    // geometry, and a surface that ran them together would be moving geometry
    // on an action that promised not to.
    Drawing d;
    d.drawClosedSquare();
    CHECK_FALSE(d.session->presentation().closedLoop.empty());
    CHECK(d.session->presentation().healableLoop.empty());
    CHECK_FALSE(invokeAction(*d.session, "region.heal-and-fill"));
}

TEST_CASE("the fill offer reaches the strip the moment the outline closes") {
    Drawing d;
    d.click(Point{-60.0, -40.0});
    d.click(Point{60.0, -40.0});
    d.click(Point{60.0, 40.0});
    for(const SurfaceEntry &e : stripEntries(*d.session)) {
        CHECK(e.action->name != std::string("region.make-solid"));
    }

    d.click(Point{-60.0, 40.0});
    d.click(Point{-59.5, -39.5});

    bool offered = false;
    for(const SurfaceEntry &e : stripEntries(*d.session)) {
        if(e.action->name == std::string("region.make-solid")) offered = true;
    }
    CHECK(offered);
}

TEST_CASE("an area enclosed by crossing segments is not a loop") {
    // The deferred case PRINCIPLES names. Two segments that cross in the middle
    // enclose an area visually and share no endpoint, so there is no cycle in
    // the coincidence graph and nothing to attach a region to. Filling it needs
    // explicit intersection points — a construction point carrying two on-line
    // constraints — and building the cycle through them. Deferred, not
    // forgotten, and refused rather than guessed at in the meantime.
    Document doc;
    UndoJournal journal;

    // A bowtie: four segments whose ends meet at two vertices, crossing in the
    // middle. The crossing is not a joint, so the enclosed triangles are not
    // bounded by anything the model can name.
    const EntityId a = addPoint(doc, -50.0, -30.0);
    const EntityId b = addPoint(doc, 50.0, 30.0);
    const EntityId c = addPoint(doc, -50.0, 30.0);
    const EntityId d = addPoint(doc, 50.0, -30.0);
    const EntityId first = addSegment(doc, a, b);
    const EntityId second = addSegment(doc, c, d);

    Session session(doc, journal);
    session.setViewport(regionViewport());
    session.select({first, second});

    CHECK(session.presentation().closedLoop.empty());
    CHECK(session.presentation().healableLoop.empty());
    CHECK_FALSE(invokeAction(session, "region.make-solid"));
    CHECK_FALSE(invokeAction(session, "region.heal-and-fill"));
    CHECK(doc.regions().records().empty());

    // And nothing was healed into existence behind the user's back: the
    // crossing is not near-miss geometry, it is geometry that means something
    // else.
    CHECK(doc.constraints().records().empty());
    CHECK(session.presentation().healableWidestGap == 0.0);

    // Refused, and said. "These edges enclose nothing" and "these edges enclose
    // something this cannot name yet" are different facts about a drawing, and
    // the second one plainly encloses something — so the deferred case is named
    // rather than left as the same silence as the first.
    REQUIRE(session.presentation().crossing);
    CHECK(session.presentation().crossing->first == std::min(first, second));
    CHECK(session.presentation().crossing->second == std::max(first, second));
}

TEST_CASE("edges that merely meet at their ends are not crossing") {
    // A boundary is built out of ends that meet, so a crossing test that
    // counted them would call every drawn outline the deferred case — and the
    // message would fire on the one drawing it is least true of.
    Drawing d;
    d.drawClosedSquare();
    REQUIRE(d.session->presentation().closedLoop.size() == 4);
    CHECK_FALSE(d.session->presentation().crossing);
}
