#include <doctest/doctest.h>

#include "core/persist.h"
#include "interact/loops.h"
#include "interact/session.h"
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
