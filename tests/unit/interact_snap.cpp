#include <doctest/doctest.h>

#include <algorithm>

#include "interact/session.h"
#include "support/build.h"

using namespace paroculus;

namespace {

Viewport snapViewport() {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(400.0, 300.0));
    m.scale(Eigen::Vector2d(1.0, -1.0));
    Viewport v;
    v.view = ViewTransform(m);
    v.width = 800.0;
    v.height = 600.0;
    return v;
}

// A document with one non-axis-aligned segment to infer against, plus a
// separate horizontal one. Grid off unless a case wants it, so a case tests the
// kind it names rather than the grid.
struct Bench {
    Document doc;
    Pose pose{doc};
    SpatialIndex index;
    SnapPolicy policy;
    ViewTransform view = snapViewport().view;

    EntityId a, b, c, d, slanted, flat;

    Bench() {
        a = paroculus::test::addPoint(doc, 0.0, 0.0);
        b = paroculus::test::addPoint(doc, 100.0, 60.0);
        slanted = paroculus::test::addSegment(doc, a, b);
        c = paroculus::test::addPoint(doc, -200.0, -100.0);
        d = paroculus::test::addPoint(doc, -100.0, -100.0);
        flat = paroculus::test::addSegment(doc, c, d);
        pose = Pose(doc);
        index.rebuild(pose);
        policy.gridEnabled = false;
    }

    SnapResult at(Point cursor, bool haveAnchor = false, Point anchor = {},
                  EntityId anchorEntity = {}) const {
        SnapRequest r;
        r.cursor = cursor;
        r.haveAnchor = haveAnchor;
        r.anchor = anchor;
        r.anchorEntity = anchorEntity;
        return snap(doc, pose, index, view, r, policy);
    }
};

bool has(const std::vector<SnapCandidate> &candidates, SnapKind kind) {
    return std::any_of(candidates.begin(), candidates.end(),
                       [&](const SnapCandidate &c) { return c.kind == kind; });
}

const SnapCandidate *find(const std::vector<SnapCandidate> &candidates, SnapKind kind) {
    for(const SnapCandidate &c : candidates) {
        if(c.kind == kind) return &c;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("every snap kind carries the constraint it commits") {
    // The engine is a projection of the taxonomy, not a second list. If a row
    // gains a kind, this is what says the engine has to mean something by it.
    for(const SnapKindInfo &info : SNAP_KINDS) {
        if(info.tier == SnapTier::PlacementOnly) {
            CHECK_FALSE(info.commitsConstraint);
        } else {
            CHECK(info.commitsConstraint);
        }
    }
}

TEST_CASE("snap candidates are generated per kind") {
    const Bench bench;

    SUBCASE("endpoint captures a nearby vertex, exactly") {
        const SnapResult r = bench.at(Point{3.0, 2.0});
        REQUIRE(has(r.candidates, SnapKind::Endpoint));
        const SnapCandidate *e = find(r.candidates, SnapKind::Endpoint);
        CHECK(e->target == bench.a);
        // Exactly on the vertex, not merely near it: the placement is what the
        // coincidence will make true anyway.
        CHECK(e->placement.x == doctest::Approx(0.0));
        CHECK(e->placement.y == doctest::Approx(0.0));
    }

    SUBCASE("midpoint captures the centre of a segment") {
        const SnapResult r = bench.at(Point{50.0, 32.0});
        const SnapCandidate *m = find(r.candidates, SnapKind::Midpoint);
        REQUIRE(m != nullptr);
        CHECK(m->target == bench.slanted);
        CHECK(m->placement.x == doctest::Approx(50.0));
        CHECK(m->placement.y == doctest::Approx(30.0));
    }

    SUBCASE("on-line projects onto the segment") {
        const SnapResult r = bench.at(Point{25.0, 18.0});
        const SnapCandidate *l = find(r.candidates, SnapKind::OnLine);
        REQUIRE(l != nullptr);
        CHECK(l->target == bench.slanted);
        // The foot of the perpendicular lies on the line.
        const double cross = l->placement.x * 60.0 - l->placement.y * 100.0;
        CHECK(cross == doctest::Approx(0.0).epsilon(1e-9));
    }

    SUBCASE("nothing within tolerance yields nothing") {
        const SnapResult r = bench.at(Point{300.0, 300.0});
        CHECK(r.candidates.empty());
        // And the placement is left exactly where the user put it.
        CHECK(r.placement.x == 300.0);
        CHECK(r.placement.y == 300.0);
    }
}

TEST_CASE("direction kinds need a segment in flight") {
    const Bench bench;
    // Without an anchor there is no segment being drawn, so a direction has
    // nothing to describe.
    const SnapResult none = bench.at(Point{200.0, 1.0});
    CHECK_FALSE(has(none.candidates, SnapKind::Horizontal));

    const SnapResult drawing = bench.at(Point{200.0, 1.0}, true, Point{100.0, 0.0});
    CHECK(has(drawing.candidates, SnapKind::Horizontal));
    const SnapCandidate *h = find(drawing.candidates, SnapKind::Horizontal);
    // Projected onto the axis: the smallest move that makes it true.
    CHECK(h->placement.x == doctest::Approx(200.0));
    CHECK(h->placement.y == doctest::Approx(0.0));
}

TEST_CASE("vertical is inferred the same way, and only near the axis") {
    const Bench bench;
    const SnapResult near = bench.at(Point{101.0, 200.0}, true, Point{100.0, 0.0});
    CHECK(has(near.candidates, SnapKind::Vertical));

    const SnapResult off = bench.at(Point{160.0, 200.0}, true, Point{100.0, 0.0});
    CHECK_FALSE(has(off.candidates, SnapKind::Vertical));
}

TEST_CASE("parallel and perpendicular reference real segments") {
    const Bench bench;
    // The slanted segment runs (0,0)->(100,60). Drawing alongside it.
    const SnapResult par = bench.at(Point{300.0, 179.0}, true, Point{200.0, 119.0});
    const SnapCandidate *p = find(par.candidates, SnapKind::Parallel);
    REQUIRE(p != nullptr);
    CHECK(p->target == bench.slanted);

    // And across it: (100,60) rotated is (-60,100).
    const SnapResult perp = bench.at(Point{140.0, 220.0}, true, Point{200.0, 119.0});
    const SnapCandidate *q = find(perp.candidates, SnapKind::Perpendicular);
    REQUIRE(q != nullptr);
    CHECK(q->target == bench.slanted);
}

TEST_CASE("an axis-aligned segment is never a parallel reference") {
    // A segment parallel to a horizontal one is horizontal. Declaring the
    // derived relation instead of the plain one buries the intent and makes the
    // document harder to read than the drawing was.
    const Bench bench;
    const SnapResult r = bench.at(Point{200.0, -100.0}, true, Point{100.0, -100.0});
    CHECK(has(r.candidates, SnapKind::Horizontal));
    for(const SnapCandidate &c : r.candidates) {
        CHECK(c.kind != SnapKind::Parallel);
    }
}

TEST_CASE("a placement never snaps to its own anchor") {
    // Pointing near your own anchor means "short segment", not "zero-length
    // one", and a coincidence with the anchor would propose exactly that.
    Bench bench;
    const SnapResult r = bench.at(Point{2.0, 1.0}, true, Point{0.0, 0.0}, bench.a);
    for(const SnapCandidate &c : r.candidates) {
        CHECK(c.target != bench.a);
    }
}

TEST_CASE("grid places but never declares") {
    // A document where every point is grid-pinned is rigidity by helpfulness.
    Bench bench;
    bench.policy.gridEnabled = true;
    bench.policy.gridStep = 20.0;

    const SnapResult r = bench.at(Point{302.0, 298.0});
    REQUIRE(has(r.candidates, SnapKind::Grid));
    CHECK(r.placement.x == doctest::Approx(300.0));
    CHECK(r.placement.y == doctest::Approx(300.0));

    // It corrects the placement and proposes no constraint at all.
    CHECK(r.autoCommitted().empty());
    CHECK(r.offered().empty());
    const SnapCandidate *g = find(r.candidates, SnapKind::Grid);
    CHECK_FALSE(constraintFor(*g, EntityId(1), EntityId(2)).has_value());
}

TEST_CASE("ranking puts auto-commit above offered, whatever the distance") {
    // An auto-committing coincidence must never lose to an offered relation
    // that happens to be nearer: the two are not competing for the same thing.
    // One is going to be declared, the other suggested.
    const Bench bench;
    // Near the slanted segment's midpoint, and also near its far endpoint.
    const SnapResult r = bench.at(Point{97.0, 58.0}, true, Point{-100.0, 200.0});
    REQUIRE(has(r.candidates, SnapKind::Endpoint));
    CHECK(r.candidates.front().kind == SnapKind::Endpoint);
    CHECK(r.placement.x == doctest::Approx(100.0));
    CHECK(r.placement.y == doctest::Approx(60.0));
}

TEST_CASE("ranking is deterministic") {
    const Bench bench;
    const SnapResult a = bench.at(Point{50.0, 31.0}, true, Point{-100.0, 200.0});
    const SnapResult b = bench.at(Point{50.0, 31.0}, true, Point{-100.0, 200.0});
    REQUIRE(a.candidates.size() == b.candidates.size());
    for(size_t i = 0; i < a.candidates.size(); i++) {
        CHECK(a.candidates[i].kind == b.candidates[i].kind);
        CHECK(a.candidates[i].target == b.candidates[i].target);
        CHECK(a.candidates[i].score == b.candidates[i].score);
    }
}

TEST_CASE("WYSIWYG: the previewed set is the committed set") {
    // The property the whole inference design rests on. What the user sees
    // mid-gesture has to be what commit produces, and the way that is
    // guaranteed is that both read one inference call rather than two that
    // have to be kept in agreement.
    Document doc;
    const EntityId corner = paroculus::test::addPoint(doc, 40.0, 40.0);
    UndoJournal journal;
    Session session(doc, journal);
    session.setViewport(snapViewport());
    session.setTool(ToolKind::Line);

    auto moveTo = [&](Point p) {
        session.handle(PointerEvent::at(PointerAction::Move,
                                        session.viewport().view.toScreen(p),
                                        session.viewport().view));
    };
    auto pressAt = [&](Point p) {
        const Eigen::Vector2d s = session.viewport().view.toScreen(p);
        session.handle(
            PointerEvent::at(PointerAction::Press, s, session.viewport().view, Button::Left));
    };

    // Hover just off an existing vertex, then place there. Then draw a run that
    // reads as horizontal.
    moveTo(Point{38.0, 41.0});
    const std::vector<SnapCandidate> previewedStart =
        session.presentation().snapCandidates;
    REQUIRE_FALSE(previewedStart.empty());
    CHECK(previewedStart.front().kind == SnapKind::Endpoint);
    pressAt(Point{38.0, 41.0});

    moveTo(Point{160.0, 41.0});
    const std::vector<SnapCandidate> previewedEnd = session.presentation().snapCandidates;
    // Horizontal is what a nearly-flat run means.
    CHECK(std::any_of(previewedEnd.begin(), previewedEnd.end(),
                      [](const SnapCandidate &c) { return c.kind == SnapKind::Horizontal; }));
    pressAt(Point{160.0, 41.0});

    // What got declared is exactly what was previewed as auto-committing:
    // coincidence for the start (held one click, because the point it binds had
    // no id until the segment justified it) and horizontal for the segment.
    std::vector<ConstraintKind> declared;
    for(const ConstraintRecord &c : doc.constraints().records()) declared.push_back(c.kind);
    std::sort(declared.begin(), declared.end());
    std::vector<ConstraintKind> expected{ConstraintKind::Coincident, ConstraintKind::Horizontal};
    std::sort(expected.begin(), expected.end());
    CHECK(declared == expected);

    // The coincidence binds the placed start to the vertex the user aimed at.
    bool bound = false;
    for(const ConstraintRecord &c : doc.constraints().records()) {
        if(c.kind == ConstraintKind::Coincident) {
            bound = c.operands[0] == corner || c.operands[1] == corner;
        }
    }
    CHECK(bound);

    // And the whole placement — geometry plus its inferences — is one step.
    CHECK(journal.records().size() == 1);
    session.handle(Key::Undo);
    CHECK(doc.constraints().size() == 0);
    CHECK(doc.entities().size() == 1);  // back to the lone vertex
}

TEST_CASE("offered candidates are not declared") {
    // Only the strongest auto-commit. The rest are surfaced, and surfacing is
    // not declaring — helpful rigidity is its own failure mode.
    Document doc;
    const EntityId a = paroculus::test::addPoint(doc, 0.0, 0.0);
    const EntityId b = paroculus::test::addPoint(doc, 100.0, 60.0);
    paroculus::test::addSegment(doc, a, b);

    UndoJournal journal;
    Session session(doc, journal);
    session.setViewport(snapViewport());
    session.snapPolicy().gridEnabled = false;
    session.setTool(ToolKind::Line);

    auto click = [&](Point p) {
        const Eigen::Vector2d s = session.viewport().view.toScreen(p);
        session.handle(PointerEvent::at(PointerAction::Move, s, session.viewport().view));
        session.handle(
            PointerEvent::at(PointerAction::Press, s, session.viewport().view, Button::Left));
    };

    // A run parallel to the slanted segment, away from any vertex.
    click(Point{200.0, 119.0});
    click(Point{300.0, 179.0});

    bool offeredParallel = false;
    for(const SnapCandidate &c : session.presentation().snapCandidates) {
        if(c.kind == SnapKind::Parallel) offeredParallel = true;
    }
    CHECK(offeredParallel);

    // Offered, and therefore not declared.
    for(const ConstraintRecord &c : doc.constraints().records()) {
        CHECK(c.kind != ConstraintKind::Parallel);
    }
}

TEST_CASE("an inferred constraint is reported at commit") {
    // No silent changes: what was declared is named, by id, at the moment it is
    // declared rather than discovered later by hovering.
    Document doc;
    paroculus::test::addPoint(doc, 40.0, 40.0);
    UndoJournal journal;
    Session session(doc, journal);
    session.setViewport(snapViewport());
    session.setTool(ToolKind::Line);

    auto click = [&](Point p) {
        const Eigen::Vector2d s = session.viewport().view.toScreen(p);
        session.handle(PointerEvent::at(PointerAction::Move, s, session.viewport().view));
        session.handle(
            PointerEvent::at(PointerAction::Press, s, session.viewport().view, Button::Left));
    };
    click(Point{38.0, 41.0});
    click(Point{160.0, 41.0});

    CHECK_FALSE(session.presentation().inferred.empty());
    for(ConstraintId id : session.presentation().inferred) {
        CHECK(doc.constraints().contains(id));
    }
}

TEST_CASE("candidates map onto taxonomy constraints in the declared operand order") {
    const Bench bench;
    SnapCandidate endpoint;
    endpoint.kind = SnapKind::Endpoint;
    endpoint.subject = SnapSubject::PlacedPoint;
    endpoint.target = bench.a;
    const auto coincident = constraintFor(endpoint, EntityId(7), EntityId(8));
    REQUIRE(coincident.has_value());
    CHECK(coincident->kind == ConstraintKind::Coincident);
    CHECK(coincident->operands[0] == EntityId(7));  // the placed point
    CHECK(coincident->operands[1] == bench.a);

    SnapCandidate horizontal;
    horizontal.kind = SnapKind::Horizontal;
    horizontal.subject = SnapSubject::PlacedSegment;
    const auto flat = constraintFor(horizontal, EntityId(7), EntityId(8));
    REQUIRE(flat.has_value());
    CHECK(flat->kind == ConstraintKind::Horizontal);
    CHECK(flat->operands[0] == EntityId(8));  // the placed segment
    CHECK_FALSE(flat->operands[1].valid());

    // A candidate whose subject the placement did not create declares nothing.
    CHECK_FALSE(constraintFor(horizontal, EntityId(7), EntityId()).has_value());
}
