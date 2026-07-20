#include <doctest/doctest.h>

#include <cmath>

#include "core/persist.h"
#include "core/tags.h"
#include "interact/script.h"
#include "interact/session.h"
#include "solve/solve.h"
#include "support/build.h"

using namespace paroculus;

namespace {

Viewport shapeViewport() {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(400.0, 300.0));
    m.scale(Eigen::Vector2d(1.0, -1.0));
    Viewport v;
    v.view = ViewTransform(m);
    v.width = 800.0;
    v.height = 600.0;
    return v;
}

struct Sketch {
    Document doc;
    UndoJournal journal;
    std::unique_ptr<Session> session;

    explicit Sketch(ToolKind tool) {
        session = std::make_unique<Session>(doc, journal);
        session->setViewport(shapeViewport());
        session->snapPolicy().gridEnabled = false;
        session->setTool(tool);
    }

    void click(Point p) {
        const Eigen::Vector2d s = session->viewport().view.toScreen(p);
        session->handle(PointerEvent::at(PointerAction::Move, s, session->viewport().view));
        session->handle(
            PointerEvent::at(PointerAction::Press, s, session->viewport().view, Button::Left));
    }

    size_t count(EntityKind kind) const {
        size_t n = 0;
        for(const EntityRecord &e : doc.entities().records()) {
            if(e.kind == kind) n++;
        }
        return n;
    }
    size_t count(ConstraintKind kind) const {
        size_t n = 0;
        for(const ConstraintRecord &c : doc.constraints().records()) {
            if(c.kind == kind) n++;
        }
        return n;
    }
    const EntityRecord *first(EntityKind kind) const {
        for(const EntityRecord &e : doc.entities().records()) {
            if(e.kind == kind) return &e;
        }
        return nullptr;
    }
};

}  // namespace

TEST_CASE("a circle is a centre and a radius it owns") {
    Sketch s(ToolKind::Circle);
    s.click(Point{0.0, 0.0});
    CHECK(s.doc.entities().size() == 0);  // the centre click alone commits nothing

    s.click(Point{50.0, 0.0});
    CHECK(s.count(EntityKind::Point) == 1);
    CHECK(s.count(EntityKind::Circle) == 1);

    const EntityRecord *circle = s.first(EntityKind::Circle);
    REQUIRE(circle != nullptr);
    CHECK(s.session->pose().radius(circle->id) == doctest::Approx(50.0));
    // One gesture, one step.
    CHECK(s.journal.records().size() == 1);
    s.session->handle(Key::Undo);
    CHECK(s.doc.entities().size() == 0);
}

TEST_CASE("a circle with no radius is refused rather than committed") {
    Sketch s(ToolKind::Circle);
    s.click(Point{0.0, 0.0});
    s.click(Point{0.0, 0.0});
    CHECK(s.doc.entities().size() == 0);
    // And the centre survives, so the gesture can be finished rather than
    // restarted.
    CHECK(s.session->presentation().toolPreview.active);
}

TEST_CASE("a rectangle is four segments, four coincidences and four axis relations") {
    // A rectangle is not a type. What lands is ordinary constrained geometry,
    // which is what makes there be no convert-to-path cliff to fall off later.
    Sketch s(ToolKind::Rectangle);
    s.click(Point{-60.0, -40.0});
    s.click(Point{60.0, 40.0});

    CHECK(s.count(EntityKind::Segment) == 4);
    CHECK(s.count(EntityKind::Point) == 8);
    CHECK(s.count(ConstraintKind::Coincident) == 4);
    CHECK(s.count(ConstraintKind::Horizontal) == 2);
    CHECK(s.count(ConstraintKind::Vertical) == 2);

    // Sixteen parameters, less eight for the coincidences, less four for the
    // axes: position, width and height. That is what a rectangle is.
    CHECK(s.session->presentation().dof == 4);
    CHECK(s.session->presentation().status == SolveStatus::Okay);

    // One gesture, one step, inferences included.
    CHECK(s.journal.records().size() == 1);
    s.session->handle(Key::Undo);
    CHECK(s.doc.entities().size() == 0);
    CHECK(s.doc.constraints().size() == 0);
}

TEST_CASE("a rectangle corner opens when its coincidence is deleted") {
    // The graceful dissolution the macro design promises: separate points
    // joined by coincidence, so a corner can be opened by deleting one
    // relation. Shared points could not be un-shared without rebuilding.
    Sketch s(ToolKind::Rectangle);
    s.click(Point{-60.0, -40.0});
    s.click(Point{60.0, 40.0});

    ConstraintId join;
    for(const ConstraintRecord &c : s.doc.constraints().records()) {
        if(c.kind == ConstraintKind::Coincident) {
            join = c.id;
            break;
        }
    }
    REQUIRE(join.valid());
    // Through deletionStep, because the tag names this relation and a bare
    // removal would leave it dangling — which the model refuses. The step
    // shrinks the tag and takes the relation, in that order.
    const ConstraintId doomed[] = {join};
    REQUIRE(s.journal.applyStep(s.doc, "open", deletionStep(s.doc, doomed)) ==
            CommandError::None);
    s.session->refresh();

    // The geometry survives entirely; only the relation went.
    CHECK(s.count(EntityKind::Segment) == 4);
    CHECK(s.count(ConstraintKind::Coincident) == 3);
    // And two more degrees of freedom appeared where the corner opened.
    CHECK(s.session->presentation().dof == 6);

    // The tag is still here and offering nothing. It owned none of the above,
    // so opening the corner cost the user the affordances and not a primitive.
    REQUIRE(s.doc.tags().size() == 1);
    const TagRecord &tag = s.doc.tags().records().front();
    CHECK(tagState(s.doc, tag) == TagState::Broken);
    CHECK(!rectangleFrame(s.doc, tag).has_value());
    CHECK(tag.entities.size() == 4);
}

TEST_CASE("a rectangle arrives tagged, and the tag resolves to its affordances") {
    Sketch s(ToolKind::Rectangle);
    s.click(Point{-60.0, -40.0});
    s.click(Point{60.0, 40.0});

    REQUIRE(s.doc.tags().size() == 1);
    const TagRecord &tag = s.doc.tags().records().front();
    CHECK(tag.kind == TagKind::Rectangle);
    // Four edges and the eight relations that square them. Not the corner
    // points: they are reached through the edges, and naming them would make the
    // tag's own bookkeeping the thing that decides when a rectangle is a
    // rectangle.
    CHECK(tag.entities.size() == 4);
    CHECK(tag.constraints.size() == 8);
    CHECK(tagState(s.doc, tag) == TagState::Whole);

    const std::optional<RectangleFrame> frame = rectangleFrame(s.doc, tag);
    REQUIRE(frame.has_value());
    CHECK(frame->widthEdge.valid());
    CHECK(frame->heightEdge.valid());
    CHECK(frame->widthEdge != frame->heightEdge);

    const Pose pose = s.session->pose();
    const std::optional<std::array<Point, 4>> handles = rectangleHandles(pose, *frame);
    REQUIRE(handles.has_value());
    // One handle per corner, and the ring is the ring: consecutive handles differ
    // in exactly one coordinate, because consecutive edges are axis constrained.
    for(size_t i = 0; i < 4; i++) {
        const Point a = (*handles)[i];
        const Point b = (*handles)[(i + 1) % 4];
        CHECK((std::abs(a.x - b.x) < 1e-9) != (std::abs(a.y - b.y) < 1e-9));
    }

    const std::optional<RectangleSize> size = rectangleSize(s.doc, pose, *frame);
    REQUIRE(size.has_value());
    CHECK(size->width == doctest::Approx(120.0));
    CHECK(size->height == doctest::Approx(80.0));
    // Undimensioned, so the handle moves geometry the ordinary way and a number
    // typed into the panel imposes the dimension the handle then drives.
    CHECK(!size->widthDimension.valid());
    CHECK(!size->heightDimension.valid());
}

TEST_CASE("undo takes the rectangle's tag back with its geometry") {
    Sketch s(ToolKind::Rectangle);
    s.click(Point{-60.0, -40.0});
    s.click(Point{60.0, 40.0});
    REQUIRE(s.doc.tags().size() == 1);

    s.session->handle(Key::Undo);
    CHECK(s.doc.tags().size() == 0);
    CHECK(s.doc.entities().size() == 0);
}

TEST_CASE("a degenerate rectangle is refused") {
    Sketch s(ToolKind::Rectangle);
    s.click(Point{-60.0, -40.0});
    s.click(Point{-60.0, 40.0});  // no width
    CHECK(s.doc.entities().size() == 0);
}

TEST_CASE("an arc lands as its macro") {
    // Centre-form solver arc, a construction centre, and the through point held
    // on the arc by a declared relation rather than by having been clicked once.
    Sketch s(ToolKind::Arc);
    s.click(Point{-50.0, 0.0});  // start
    s.click(Point{50.0, 0.0});   // end
    s.click(Point{0.0, 50.0});   // bulge

    REQUIRE(s.count(EntityKind::Arc) == 1);
    CHECK(s.count(ConstraintKind::PointOnCircle) == 1);
    // Centre, two endpoints, and the through point.
    CHECK(s.count(EntityKind::Point) == 4);

    const EntityRecord *arc = s.first(EntityKind::Arc);
    REQUIRE(arc != nullptr);
    const EntityRecord *centre = s.doc.entities().find(arc->points[0]);
    REQUIRE(centre != nullptr);
    // The centre is construction: real geometry, but not part of the shape.
    CHECK(centre->role == Role::Construction);

    // Through-point on the circle within tolerance, which is the macro's
    // defining invariant.
    const Pose pose = s.session->pose();
    const std::optional<Pose::ArcGeometry> g = pose.arc(arc->id);
    REQUIRE(g.has_value());
    CHECK(g->radius == doctest::Approx(50.0));
    CHECK(g->centre.x == doctest::Approx(0.0));
    CHECK(g->centre.y == doctest::Approx(0.0));

    for(const ConstraintRecord &c : s.doc.constraints().records()) {
        if(c.kind != ConstraintKind::PointOnCircle) continue;
        const std::optional<Point> through = pose.point(c.operands[0]);
        REQUIRE(through.has_value());
        const double d = std::hypot(through->x - g->centre.x, through->y - g->centre.y);
        CHECK(d == doctest::Approx(g->radius).epsilon(1e-9));
    }
}

TEST_CASE("the arc follows the hand rather than a convention") {
    // Bulging one way or the other has to produce the arc that was drawn, not
    // its complement.
    Sketch up(ToolKind::Arc);
    up.click(Point{-50.0, 0.0});
    up.click(Point{50.0, 0.0});
    up.click(Point{0.0, 50.0});
    const Pose upPose = up.session->pose();
    const std::optional<Pose::ArcGeometry> a = upPose.arc(up.first(EntityKind::Arc)->id);
    REQUIRE(a.has_value());

    Sketch down(ToolKind::Arc);
    down.click(Point{-50.0, 0.0});
    down.click(Point{50.0, 0.0});
    down.click(Point{0.0, -50.0});
    const Pose downPose = down.session->pose();
    const std::optional<Pose::ArcGeometry> b = downPose.arc(down.first(EntityKind::Arc)->id);
    REQUIRE(b.has_value());

    // Same circle, opposite halves: the midpoint of each sweep is on the side
    // the user bulged toward.
    auto midpointOf = [](const Pose::ArcGeometry &g) {
        const double angle = g.startAngle + g.sweep * 0.5;
        return Point{g.centre.x + g.radius * std::cos(angle),
                     g.centre.y + g.radius * std::sin(angle)};
    };
    CHECK(midpointOf(*a).y > 0.0);
    CHECK(midpointOf(*b).y < 0.0);
}

TEST_CASE("three collinear clicks describe no arc, and commit nothing") {
    Sketch s(ToolKind::Arc);
    s.click(Point{-50.0, 0.0});
    s.click(Point{50.0, 0.0});
    s.click(Point{0.0, 0.0});
    CHECK(s.count(EntityKind::Arc) == 0);
    CHECK(s.doc.entities().size() == 0);
}

TEST_CASE("an arc's centre does not attract the next placement") {
    // Construction geometry participates in constraints identically and is only
    // presented differently — but a sketch full of arcs must not become a
    // sketch full of magnets nobody aimed at.
    Sketch s(ToolKind::Arc);
    s.click(Point{-50.0, 0.0});
    s.click(Point{50.0, 0.0});
    s.click(Point{0.0, 50.0});

    const EntityRecord *arc = s.first(EntityKind::Arc);
    REQUIRE(arc != nullptr);
    const EntityId centre = arc->points[0];

    // Draw a line starting right on the centre.
    s.session->setTool(ToolKind::Line);
    s.click(Point{1.0, 1.0});
    s.click(Point{80.0, 80.0});

    for(const ConstraintRecord &c : s.doc.constraints().records()) {
        if(c.kind != ConstraintKind::Coincident) continue;
        CHECK(c.operands[0] != centre);
        CHECK(c.operands[1] != centre);
    }

    // But it is reachable when the policy asks for it: excluded by default is
    // not excluded by construction.
    s.session->snapPolicy().snapToConstruction = true;
    s.session->setTool(ToolKind::Line);
    s.click(Point{1.0, 1.0});
    s.click(Point{-80.0, 80.0});
    bool bound = false;
    for(const ConstraintRecord &c : s.doc.constraints().records()) {
        if(c.kind != ConstraintKind::Coincident) continue;
        bound = bound || c.operands[0] == centre || c.operands[1] == centre;
    }
    CHECK(bound);
}

TEST_CASE("a placement can snap onto an arc's rim") {
    Sketch s(ToolKind::Arc);
    s.click(Point{-50.0, 0.0});
    s.click(Point{50.0, 0.0});
    s.click(Point{0.0, 50.0});
    const EntityRecord *arc = s.first(EntityKind::Arc);
    REQUIRE(arc != nullptr);

    s.session->setTool(ToolKind::Line);
    // Just off the rim at the top of the sweep.
    const Eigen::Vector2d screen = s.session->viewport().view.toScreen(Point{0.0, 48.0});
    s.session->handle(
        PointerEvent::at(PointerAction::Move, screen, s.session->viewport().view));

    bool offered = false;
    for(const SnapCandidate &c : s.session->presentation().snapCandidates) {
        if(c.kind == SnapKind::OnCircle && c.target == arc->id) offered = true;
    }
    CHECK(offered);
}

TEST_CASE("every shape tool survives a script round-trip") {
    Document doc;
    UndoJournal journal;
    Session session(doc, journal);
    GestureScript script;
    script.document = doc;
    ScriptRecorder recorder;
    session.setRecorder(&recorder);
    session.setViewport(shapeViewport());
    session.setTool(ToolKind::Rectangle);

    auto click = [&](Point p) {
        const Eigen::Vector2d s = session.viewport().view.toScreen(p);
        session.handle(PointerEvent::at(PointerAction::Move, s, session.viewport().view));
        session.handle(
            PointerEvent::at(PointerAction::Press, s, session.viewport().view, Button::Left));
    };
    click(Point{-60.0, -40.0});
    click(Point{60.0, 40.0});
    session.setTool(ToolKind::Circle);
    click(Point{140.0, 0.0});
    click(Point{180.0, 0.0});
    session.setTool(ToolKind::Arc);
    click(Point{-160.0, 0.0});
    click(Point{-100.0, 0.0});
    click(Point{-130.0, 40.0});
    script.steps = recorder.steps();

    const std::string text = serializeScript(script);
    CHECK(text.find("tool name=rectangle") != std::string::npos);
    CHECK(text.find("tool name=circle") != std::string::npos);
    CHECK(text.find("tool name=arc") != std::string::npos);

    GestureScript parsed;
    REQUIRE(parseScript(text, parsed).ok);
    CHECK(serializeScript(parsed) == text);

    Document replayed = parsed.document;
    UndoJournal journal2;
    Session session2(replayed, journal2);
    replay(session2, parsed);
    CHECK(serialize(replayed) == serialize(doc));
}

TEST_CASE("an arc dissolves gracefully when a member is deleted") {
    // Shape tools are macros, not types: there is nothing to convert because
    // there is nothing that was converted. The test of that is what deleting
    // one member leaves behind — ordinary constrained geometry, not a broken
    // special object and not a cascade that takes the whole shape.
    Sketch s(ToolKind::Arc);
    s.click(Point{-50.0, 0.0});
    s.click(Point{50.0, 0.0});
    s.click(Point{0.0, 50.0});

    const EntityRecord *arc = s.first(EntityKind::Arc);
    REQUIRE(arc != nullptr);
    const EntityId arcId = arc->id;
    const EntityId centre = arc->points[0];
    REQUIRE(s.count(ConstraintKind::PointOnCircle) == 1);
    const ConstraintId onCircle = s.doc.constraints().records().front().id;

    SUBCASE("deleting the macro's own relation opens it and keeps the geometry") {
        REQUIRE(s.doc.apply(RemoveRecord<ConstraintRecord>{onCircle}).ok());

        // Everything the macro placed is still there and still an arc.
        CHECK(s.count(EntityKind::Arc) == 1);
        CHECK(s.count(EntityKind::Point) == 4);
        CHECK(s.doc.constraints().empty());

        // And it still solves: what is left is under-constrained, which is the
        // state every document starts in rather than a broken one.
        Topology topology(s.doc);
        SolveContext context = SolveContext::forComponent(s.doc, topology, arcId);
        const SolveOutcome outcome = solve(s.doc, context);
        CHECK(outcome.ok());
        CHECK(outcome.dof > 0);
    }

    SUBCASE("deleting the through point takes its relation and nothing else") {
        // The through point is held on the arc but defines none of it, so the
        // arc outlives it. Which point it is, is the one the relation names.
        const EntityId through = s.doc.constraints().records().front().operands[0];
        for(const Command &c : deletionStep(s.doc, through)) REQUIRE(s.doc.apply(c).ok());

        CHECK(s.count(EntityKind::Arc) == 1);
        CHECK(s.count(EntityKind::Point) == 3);  // centre and the two ends
        CHECK(s.doc.constraints().empty());
        CHECK(s.doc.entities().contains(arcId));
    }

    SUBCASE("deleting a defining point takes the arc, because it was one") {
        // The other direction, and the one that must cascade: an arc without
        // its centre is not a degraded arc, it is nonsense. The through point
        // survives, since it defined nothing.
        const EntityId through = s.doc.constraints().records().front().operands[0];
        for(const Command &c : deletionStep(s.doc, centre)) REQUIRE(s.doc.apply(c).ok());

        CHECK(s.count(EntityKind::Arc) == 0);
        CHECK_FALSE(s.doc.entities().contains(arcId));
        CHECK(s.doc.entities().contains(through));
        CHECK(s.doc.constraints().empty());
    }
}
