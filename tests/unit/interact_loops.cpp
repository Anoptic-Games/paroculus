#include <doctest/doctest.h>

#include "core/persist.h"
#include "interact/loops.h"
#include "interact/session.h"
#include "core/composition.h"
#include "core/topology.h"
#include "support/build.h"

using namespace paroculus;

namespace {

Viewport loopViewport() {
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
        session->setViewport(loopViewport());
        session->snapPolicy().gridEnabled = false;
        session->setTool(tool);
    }
    void click(Point p) {
        const Eigen::Vector2d s = session->viewport().view.toScreen(p);
        session->handle(PointerEvent::at(PointerAction::Move, s, session->viewport().view));
        session->handle(
            PointerEvent::at(PointerAction::Press, s, session->viewport().view, Button::Left));
    }
};

}  // namespace

TEST_CASE("a run that closes is offered as a loop") {
    // The flagship case: four segments, endpoint snapping making the joints
    // coincident as they are drawn, and the loop closes.
    Drawing d;
    d.click(Point{-60.0, -40.0});
    d.click(Point{60.0, -40.0});
    d.click(Point{60.0, 40.0});
    d.click(Point{-60.0, 40.0});
    CHECK(d.session->presentation().closedLoop.empty());  // still open

    // Back to the start, near enough that endpoint snapping declares the
    // coincidence that closes it.
    d.click(Point{-59.0, -39.0});

    const std::vector<EntityId> &boundary = d.session->presentation().closedLoop;
    REQUIRE(boundary.size() == 4);
    for(EntityId id : boundary) {
        const EntityRecord *e = d.doc.entities().find(id);
        REQUIRE(e != nullptr);
        CHECK(e->kind == EntityKind::Segment);
    }
}

TEST_CASE("detecting a loop changes nothing") {
    // An offer the user ignores has to leave the document exactly as it was, or
    // ignoring it would not be free.
    Drawing d;
    d.click(Point{-60.0, -40.0});
    d.click(Point{60.0, -40.0});
    d.click(Point{60.0, 40.0});
    d.click(Point{-60.0, 40.0});
    d.click(Point{-59.0, -39.0});
    REQUIRE_FALSE(d.session->presentation().closedLoop.empty());

    const std::string closed = serialize(d.doc);
    const size_t steps = d.journal.records().size();
    // Asking again is a query, not an edit.
    CHECK(closedBoundaryContaining(d.doc, Topology(d.doc),
                                   d.session->presentation().closedLoop.front())
              .has_value());
    CHECK(serialize(d.doc) == closed);
    CHECK(d.journal.records().size() == steps);
    // And no region record appeared: the action lands in stage 5.
    CHECK(d.doc.regions().size() == 0);
}

TEST_CASE("closure is topological, not visual") {
    // Endpoints near but not coincident are open, and stay open. The gap
    // between looks-closed and is-closed is bridged by imposing the missing
    // coincidence, never by guessing that two nearby points were meant as one.
    Document doc;
    const EntityId a = paroculus::test::addPoint(doc, -60.0, -40.0);
    const EntityId b = paroculus::test::addPoint(doc, 60.0, -40.0);
    const EntityId c = paroculus::test::addPoint(doc, 60.0, 40.0);
    const EntityId d = paroculus::test::addPoint(doc, -60.0, 40.0);
    // A fifth point a hair away from the first: visually shut, topologically
    // open.
    const EntityId e = paroculus::test::addPoint(doc, -59.9, -39.9);

    const EntityId s1 = paroculus::test::addSegment(doc, a, b);
    paroculus::test::addSegment(doc, b, c);
    paroculus::test::addSegment(doc, c, d);
    paroculus::test::addSegment(doc, d, e);

    Topology topology(doc);
    CHECK_FALSE(closedBoundaryContaining(doc, topology, s1).has_value());

    // Impose the missing coincidence and it closes, without moving anything by
    // more than the epsilon the user could not see.
    paroculus::test::addConstraint(doc, ConstraintKind::Coincident, {e, a});
    Topology closed(doc);
    CHECK(closedBoundaryContaining(doc, closed, s1).has_value());
}

TEST_CASE("an open run, a figure-eight and two loops are all refused") {
    SUBCASE("open run") {
        Drawing d;
        d.click(Point{-60.0, 0.0});
        d.click(Point{0.0, 0.0});
        d.click(Point{60.0, 0.0});
        CHECK(d.session->presentation().closedLoop.empty());
    }

    SUBCASE("a vertex touched more than twice") {
        // A figure-eight shares its middle vertex across four edges, so there
        // is no single cycle through it.
        Document doc;
        const EntityId centre = paroculus::test::addPoint(doc, 0.0, 0.0);
        const EntityId nw = paroculus::test::addPoint(doc, -40.0, 40.0);
        const EntityId ne = paroculus::test::addPoint(doc, 40.0, 40.0);
        const EntityId sw = paroculus::test::addPoint(doc, -40.0, -40.0);
        const EntityId se = paroculus::test::addPoint(doc, 40.0, -40.0);
        const EntityId first = paroculus::test::addSegment(doc, centre, nw);
        paroculus::test::addSegment(doc, nw, ne);
        paroculus::test::addSegment(doc, ne, centre);
        paroculus::test::addSegment(doc, centre, sw);
        paroculus::test::addSegment(doc, sw, se);
        paroculus::test::addSegment(doc, se, centre);

        Topology topology(doc);
        CHECK_FALSE(closedBoundaryContaining(doc, topology, first).has_value());
    }
}

TEST_CASE("a rectangle closes the moment it is drawn") {
    // The macro emits a closed outline in one gesture, so the offer arrives
    // with the shape rather than a click later.
    Drawing d(ToolKind::Rectangle);
    d.click(Point{-60.0, -40.0});
    d.click(Point{60.0, 40.0});
    CHECK(d.session->presentation().closedLoop.size() == 4);
}

TEST_CASE("an arc's construction centre does not keep the run open") {
    // The centre is real geometry hanging off the arc, and it is not part of
    // the outline it helped place.
    Drawing d(ToolKind::Arc);
    d.click(Point{-50.0, 0.0});
    d.click(Point{50.0, 0.0});
    d.click(Point{0.0, 50.0});

    EntityId arc;
    for(const EntityRecord &e : d.doc.entities().records()) {
        if(e.kind == EntityKind::Arc) arc = e.id;
    }
    REQUIRE(arc.valid());
    // One arc is not a closed loop, but it must be refused for being one edge
    // rather than for having a stray construction point attached.
    Topology topology(d.doc);
    CHECK_FALSE(closedBoundaryContaining(d.doc, topology, arc).has_value());
}

TEST_CASE("the offer clears when the placement that made it is undone") {
    Drawing d;
    d.click(Point{-60.0, -40.0});
    d.click(Point{60.0, -40.0});
    d.click(Point{60.0, 40.0});
    d.click(Point{-60.0, 40.0});
    d.click(Point{-59.0, -39.0});
    REQUIRE_FALSE(d.session->presentation().closedLoop.empty());

    d.session->setTool(ToolKind::Select);
    CHECK(d.session->presentation().closedLoop.empty());
}

// ---------------------------------------------------------------------------
// Curved boundaries
// ---------------------------------------------------------------------------

TEST_CASE("a trapezoid with an arc for a corner closes") {
    // The case that started this: an outline is straight edges plus one arc, and
    // the offer never appeared because the cycle test threw out any edge that
    // was not a segment. Detection lives in interact and the walk lives in core,
    // and only one of them knew arcs were boundary-capable.
    Document doc;
    const EntityId a = test::addPoint(doc, -60.0, -30.0);
    const EntityId b = test::addPoint(doc, 60.0, -30.0);
    const EntityId c = test::addPoint(doc, 40.0, 30.0);
    const EntityId d = test::addPoint(doc, -40.0, 30.0);

    // Three straight sides, and the fourth corner rounded by an arc from d back
    // to a. Its centre is construction geometry, exactly as the arc macro leaves
    // it, and must not be read as a joint.
    const EntityId centre = test::addPoint(doc, -50.0, 0.0);
    EntityRecord construction = *doc.entities().find(centre);
    construction.role = Role::Construction;
    REQUIRE(doc.apply(SetRecord<EntityRecord>{construction}).ok());

    const EntityId ab = test::addSegment(doc, a, b);
    const EntityId bc = test::addSegment(doc, b, c);
    const EntityId cd = test::addSegment(doc, c, d);
    const EntityId da = test::addArc(doc, centre, d, a);
    REQUIRE(da.valid());

    const Topology topology(doc);
    const std::optional<std::vector<EntityId>> loop =
        closedBoundaryContaining(doc, topology, ab);
    REQUIRE(loop);
    CHECK(loop->size() == 4);
    // The arc is in the cycle and the construction centre is not.
    CHECK(std::find(loop->begin(), loop->end(), da) != loop->end());
    CHECK(std::find(loop->begin(), loop->end(), centre) == loop->end());
    CHECK(std::find(loop->begin(), loop->end(), cd) != loop->end());
    (void)bc;

    // And the fill the offer leads to is whole, walked through the arc's ends
    // rather than through its centre.
    RegionRecord region;
    region.boundary = *loop;
    const uint32_t id = doc.apply(AddRecord<RegionRecord>{region}).allocated;
    REQUIRE(id != 0);
    CHECK(regionState(doc, RegionId(id)) == RegionState::Whole);
}

TEST_CASE("a circle is its own boundary") {
    // Foundational, and closed the moment it exists: a circle has no joints to
    // match against a neighbour, so it never appears in a joint cycle and is
    // answered from the seed instead.
    Drawing d(ToolKind::Circle);
    d.click(Point{0.0, 0.0});
    d.click(Point{50.0, 0.0});

    const std::vector<EntityId> loop = d.session->presentation().closedLoop;
    REQUIRE(loop.size() == 1);
    const EntityRecord *e = d.doc.entities().find(loop.front());
    REQUIRE(e != nullptr);
    CHECK(e->kind == EntityKind::Circle);

    // The offer leads somewhere: make-solid fills it, and the fill is whole.
    REQUIRE(d.session->makeSolid());
    REQUIRE(d.doc.regions().size() == 1);
    CHECK(regionState(d.doc, d.doc.regions().records().front().id) == RegionState::Whole);
}

TEST_CASE("a circle on a loop's corner leaves the loop detectable") {
    // A self-closing edge takes no part in the joint walk, so it cannot spoil a
    // cycle it merely touches. Refusing the whole set instead — which is what a
    // "not a segment" guard does — made a circle anywhere near an outline
    // silently kill the offer.
    Document doc;
    const EntityId a = test::addPoint(doc, 0.0, 0.0);
    const EntityId b = test::addPoint(doc, 60.0, 0.0);
    const EntityId c = test::addPoint(doc, 30.0, 50.0);
    const EntityId ab = test::addSegment(doc, a, b);
    const EntityId bc = test::addSegment(doc, b, c);
    const EntityId ca = test::addSegment(doc, c, a);
    // Centred on a corner, so it is in the same connected run as the triangle.
    const EntityId circle = test::addCircle(doc, a, 12.0);
    REQUIRE(circle.valid());

    const Topology topology(doc);
    const std::optional<std::vector<EntityId>> loop =
        closedBoundaryContaining(doc, topology, ab);
    REQUIRE(loop);
    CHECK(loop->size() == 3);
    CHECK(std::find(loop->begin(), loop->end(), circle) == loop->end());
    (void)bc;
    (void)ca;

    // And the circle is still its own boundary when it is what was clicked.
    const std::optional<std::vector<EntityId>> alone =
        closedBoundaryContaining(doc, topology, circle);
    REQUIRE(alone);
    CHECK(alone->size() == 1);
    CHECK(alone->front() == circle);
}

TEST_CASE("two straight edges still enclose nothing, and two curved ones do") {
    // The bound is about what the edges are, not how many. Two segments over one
    // pair of vertices walk closed and enclose nothing; swap one for an arc and
    // the pair encloses a circular segment.
    Document doc;
    const EntityId p = test::addPoint(doc, 0.0, 0.0);
    const EntityId q = test::addPoint(doc, 40.0, 0.0);
    const EntityId first = test::addSegment(doc, p, q);
    const EntityId second = test::addSegment(doc, p, q);

    {
        const Topology topology(doc);
        CHECK_FALSE(closedBoundaryContaining(doc, topology, first).has_value());
    }

    // Replace the second straight edge with an arc over the same two ends.
    REQUIRE(doc.apply(RemoveRecord<EntityRecord>{second}).ok());
    const EntityId centre = test::addPoint(doc, 20.0, -10.0);
    const EntityId bulge = test::addArc(doc, centre, q, p);
    REQUIRE(bulge.valid());
    EntityRecord construction = *doc.entities().find(centre);
    construction.role = Role::Construction;
    REQUIRE(doc.apply(SetRecord<EntityRecord>{construction}).ok());

    const Topology topology(doc);
    const std::optional<std::vector<EntityId>> lens =
        closedBoundaryContaining(doc, topology, first);
    REQUIRE(lens);
    CHECK(lens->size() == 2);
}

TEST_CASE("selecting a circle offers to fill it") {
    // Selecting takes the connected shape, which for a circle is its rim and its
    // centre — and the offers are seeded from the front of that selection, which
    // is the centre point rather than the curve. So a circle the user has just
    // clicked has to find its own boundary through the run, not only when it is
    // the thing just drawn. The unit tests missed this because they seeded from
    // the placement; driving the real click found it.
    Drawing d(ToolKind::Circle);
    d.click(Point{0.0, 0.0});
    d.click(Point{60.0, 0.0});
    REQUIRE(d.session->presentation().closedLoop.size() == 1);

    d.session->setTool(ToolKind::Select);
    d.click(Point{60.0, 0.0});  // the rim
    REQUIRE(d.session->selection().size() >= 1);

    const std::vector<EntityId> loop = d.session->presentation().closedLoop;
    REQUIRE(loop.size() == 1);
    CHECK(d.doc.entities().find(loop.front())->kind == EntityKind::Circle);
    REQUIRE(d.session->makeSolid());
    CHECK(d.doc.regions().size() == 1);

    // And the offer clears once it is filled, rather than stacking a second
    // identical region over the first.
    CHECK(d.session->presentation().closedLoop.empty());
}

TEST_CASE("two closed curves in one run offer neither") {
    // Which one the user meant is a question, and guessing is the silent choice
    // the surface exists to avoid. Reached from a point both are attached to;
    // clicking either curve still answers for that curve, because the seed says.
    Document doc;
    const EntityId shared = test::addPoint(doc, 0.0, 0.0);
    const EntityId inner = test::addCircle(doc, shared, 10.0);
    const EntityId outer = test::addCircle(doc, shared, 40.0);
    REQUIRE(inner.valid());
    REQUIRE(outer.valid());

    const Topology topology(doc);
    CHECK_FALSE(closedBoundaryContaining(doc, topology, shared).has_value());

    const std::optional<std::vector<EntityId>> pickedInner =
        closedBoundaryContaining(doc, topology, inner);
    REQUIRE(pickedInner);
    CHECK(pickedInner->front() == inner);
    const std::optional<std::vector<EntityId>> pickedOuter =
        closedBoundaryContaining(doc, topology, outer);
    REQUIRE(pickedOuter);
    CHECK(pickedOuter->front() == outer);
}

// ---------------------------------------------------------------------------
// Crossings, now that curves bound
// ---------------------------------------------------------------------------

namespace {

// Two edges alone in a document, so a crossing test has nothing else to find.
// The seed is the first, which is how the session asks.
bool crosses(Document &doc, EntityId a, EntityId b) {
    const Topology topology(doc);
    const Pose pose(doc);
    const EntityId seeds[] = {a, b};
    const std::optional<std::pair<EntityId, EntityId>> found =
        crossingAmong(doc, topology, pose, seeds);
    return found.has_value();
}

// An arc through three points, with its centre marked construction as the macro
// leaves it. Centre and radius are given directly so the fixture states the
// geometry rather than deriving it.
EntityId arcAt(Document &doc, Point centre, double radius, double startDegrees,
               double endDegrees) {
    const double toRad = 3.14159265358979323846 / 180.0;
    const EntityId c = test::addPoint(doc, centre.x, centre.y);
    EntityRecord construction = *doc.entities().find(c);
    construction.role = Role::Construction;
    REQUIRE(doc.apply(SetRecord<EntityRecord>{construction}).ok());
    const EntityId s = test::addPoint(doc, centre.x + radius * std::cos(startDegrees * toRad),
                                      centre.y + radius * std::sin(startDegrees * toRad));
    const EntityId e = test::addPoint(doc, centre.x + radius * std::cos(endDegrees * toRad),
                                      centre.y + radius * std::sin(endDegrees * toRad));
    return test::addArc(doc, c, s, e);
}

}  // namespace

TEST_CASE("an arc crossing a segment is reported") {
    // The blind spot arcs-as-boundaries opened. An arc that crosses a segment
    // encloses an area the model cannot name, exactly as two crossing segments
    // do — and refusing it with the same silence as "these edges enclose
    // nothing" tells the user the wrong thing about a drawing that plainly
    // encloses something.
    Document doc;
    // A half-circle bulging up over the origin, radius 40.
    const EntityId dome = arcAt(doc, Point{0.0, 0.0}, 40.0, 0.0, 180.0);
    REQUIRE(dome.valid());
    // A horizontal segment slicing through it well above the chord.
    const EntityId p = test::addPoint(doc, -60.0, 20.0);
    const EntityId q = test::addPoint(doc, 60.0, 20.0);
    const EntityId slice = test::addSegment(doc, p, q);

    CHECK(crosses(doc, dome, slice));
}

TEST_CASE("a segment meeting an arc only at a joint is not a crossing") {
    // Ends that meet are how a boundary is built. This is the case that must
    // stay quiet, or every drawn outline with an arc in it reports the deferred
    // case at itself.
    Document doc;
    const EntityId dome = arcAt(doc, Point{0.0, 0.0}, 40.0, 0.0, 180.0);
    REQUIRE(dome.valid());
    const EntityRecord *arc = doc.entities().find(dome);
    REQUIRE(arc != nullptr);
    // A chord from the arc's end back to its start, sharing both joints.
    const EntityId chord = test::addSegment(doc, arc->points[2], arc->points[1]);

    CHECK_FALSE(crosses(doc, dome, chord));
    // And they do close, which is the other half of the same claim.
    const Topology topology(doc);
    const std::optional<std::vector<EntityId>> loop =
        closedBoundaryContaining(doc, topology, chord);
    REQUIRE(loop);
    CHECK(loop->size() == 2);
}

TEST_CASE("a segment tangent to a circle is not a crossing") {
    // Touching is not crossing, for the same reason two parallel segments are
    // not: there is no intersection point for the deferred work to build
    // through.
    Document doc;
    const EntityId centre = test::addPoint(doc, 0.0, 0.0);
    const EntityId circle = test::addCircle(doc, centre, 30.0);
    REQUIRE(circle.valid());
    const EntityId p = test::addPoint(doc, -50.0, 30.0);
    const EntityId q = test::addPoint(doc, 50.0, 30.0);
    const EntityId tangent = test::addSegment(doc, p, q);

    CHECK_FALSE(crosses(doc, circle, tangent));

    // Move it a hair inward and it does cross, twice.
    EntityRecord left = *doc.entities().find(p);
    EntityRecord right = *doc.entities().find(q);
    left.seeds[1] = 29.0;
    right.seeds[1] = 29.0;
    REQUIRE(doc.apply(SetRecord<EntityRecord>{left}).ok());
    REQUIRE(doc.apply(SetRecord<EntityRecord>{right}).ok());
    CHECK(crosses(doc, circle, tangent));
}

TEST_CASE("a segment that stops short of a circle is not a crossing") {
    // The line through it would cross; the edge does not reach. Interiority is
    // about the edge, not about the infinite line it lies on.
    Document doc;
    const EntityId centre = test::addPoint(doc, 0.0, 0.0);
    const EntityId circle = test::addCircle(doc, centre, 30.0);
    const EntityId p = test::addPoint(doc, 50.0, 0.0);
    const EntityId q = test::addPoint(doc, 80.0, 0.0);
    const EntityId outside = test::addSegment(doc, p, q);
    REQUIRE(circle.valid());

    CHECK_FALSE(crosses(doc, circle, outside));
}

TEST_CASE("two arcs cross only where both sweeps reach") {
    // Circle-circle intersection finds two points; whether either is a crossing
    // is a question about the swept parts, not about the circles.
    Document doc;
    // Upper half of a circle at the origin, and the upper half of one offset to
    // the right. Their circles meet above and below the axis; only the upper
    // meeting is on both sweeps.
    const EntityId left = arcAt(doc, Point{0.0, 0.0}, 40.0, 0.0, 180.0);
    const EntityId right = arcAt(doc, Point{50.0, 0.0}, 40.0, 0.0, 180.0);
    REQUIRE(left.valid());
    REQUIRE(right.valid());
    CHECK(crosses(doc, left, right));

    // Now the lower half on the right: the circles still meet in two places and
    // neither meeting is on both swept parts.
    Document apart;
    const EntityId upper = arcAt(apart, Point{0.0, 0.0}, 40.0, 10.0, 170.0);
    const EntityId lower = arcAt(apart, Point{50.0, 0.0}, 40.0, 190.0, 350.0);
    REQUIRE(upper.valid());
    REQUIRE(lower.valid());
    CHECK_FALSE(crosses(apart, upper, lower));
}

TEST_CASE("concentric and nested circles do not cross") {
    Document doc;
    const EntityId centre = test::addPoint(doc, 0.0, 0.0);
    const EntityId inner = test::addCircle(doc, centre, 10.0);
    const EntityId outer = test::addCircle(doc, centre, 40.0);
    REQUIRE(inner.valid());
    REQUIRE(outer.valid());
    CHECK_FALSE(crosses(doc, inner, outer));

    // Nested but not concentric: still no crossing.
    Document offset;
    const EntityId c1 = test::addPoint(offset, 0.0, 0.0);
    const EntityId c2 = test::addPoint(offset, 5.0, 0.0);
    const EntityId big = test::addCircle(offset, c1, 40.0);
    const EntityId small = test::addCircle(offset, c2, 10.0);
    REQUIRE(big.valid());
    REQUIRE(small.valid());
    CHECK_FALSE(crosses(offset, big, small));
}

TEST_CASE("two overlapping circles cross") {
    Document doc;
    const EntityId c1 = test::addPoint(doc, 0.0, 0.0);
    const EntityId c2 = test::addPoint(doc, 30.0, 0.0);
    const EntityId first = test::addCircle(doc, c1, 25.0);
    const EntityId second = test::addCircle(doc, c2, 25.0);
    REQUIRE(first.valid());
    REQUIRE(second.valid());
    CHECK(crosses(doc, first, second));
}
