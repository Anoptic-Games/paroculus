#include <doctest/doctest.h>

#include <algorithm>

#include "interact/glyphs.h"
#include "interact/session.h"
#include "support/build.h"

using namespace paroculus;

namespace {

constexpr double W = 800.0;
constexpr double H = 600.0;

ViewTransform glyphView() {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(W * 0.5, H * 0.5));
    m.scale(Eigen::Vector2d(1.0, -1.0));
    return ViewTransform(m);
}

size_t marksOf(const std::vector<GlyphMark> &marks, ConstraintId id) {
    return static_cast<size_t>(
        std::count_if(marks.begin(), marks.end(),
                      [&](const GlyphMark &m) { return m.constraint == id; }));
}

}  // namespace

TEST_CASE("every constraint gets a mark on every operand it binds") {
    // No invisible constraints, ever. A parallel that marked only one of its
    // two segments would be invisible from the other, and reachable-from-the-
    // geometry-it-binds means from each piece of it.
    Document doc;
    const EntityId a = paroculus::test::addPoint(doc, -40.0, 0.0);
    const EntityId b = paroculus::test::addPoint(doc, 40.0, 0.0);
    const EntityId first = paroculus::test::addSegment(doc, a, b);
    const EntityId c = paroculus::test::addPoint(doc, -40.0, 40.0);
    const EntityId d = paroculus::test::addPoint(doc, 40.0, 60.0);
    const EntityId second = paroculus::test::addSegment(doc, c, d);

    const ConstraintId horizontal =
        paroculus::test::addConstraint(doc, ConstraintKind::Horizontal, {first});
    const ConstraintId parallel =
        paroculus::test::addConstraint(doc, ConstraintKind::Parallel, {first, second});

    const Pose pose(doc);
    std::vector<GlyphMark> marks;
    for(const ConstraintRecord &r : doc.constraints().records()) marksFor(doc, pose, r, marks);

    CHECK(marksOf(marks, horizontal) == 1);  // one operand
    CHECK(marksOf(marks, parallel) == 2);    // both segments carry it

    // And each mark knows which piece it sits on, so hover and selection can
    // reach it from that piece.
    for(const GlyphMark &m : marks) {
        CHECK(m.on.valid());
        CHECK(doc.entities().contains(m.on));
    }
}

TEST_CASE("a mark sits on the middle of what it binds") {
    // The middle rather than an end, because a mark at an endpoint collides
    // with every other relation binding the same vertex, and vertices are where
    // relations cluster.
    Document doc;
    const EntityId a = paroculus::test::addPoint(doc, -40.0, 10.0);
    const EntityId b = paroculus::test::addPoint(doc, 40.0, 10.0);
    const EntityId segment = paroculus::test::addSegment(doc, a, b);
    paroculus::test::addConstraint(doc, ConstraintKind::Horizontal, {segment});

    const Pose pose(doc);
    std::vector<GlyphMark> marks;
    marksFor(doc, pose, doc.constraints().records().front(), marks);
    REQUIRE(marks.size() == 1);
    CHECK(marks[0].anchor.x == doctest::Approx(0.0));
    CHECK(marks[0].anchor.y == doctest::Approx(10.0));
}

TEST_CASE("the overlay has a budget, and spends it on what matters") {
    // Glyph overload is real at scale. The answer is a budget over the whole
    // overlay rather than each mark deciding for itself whether it matters.
    Document doc;
    std::vector<ConstraintId> all;
    for(int i = 0; i < 60; i++) {
        const EntityId a = paroculus::test::addPoint(doc, -100.0 + i * 3.0, -100.0);
        const EntityId b = paroculus::test::addPoint(doc, -100.0 + i * 3.0, 100.0);
        const EntityId s = paroculus::test::addSegment(doc, a, b);
        all.push_back(paroculus::test::addConstraint(doc, ConstraintKind::Vertical, {s}));
    }

    const Pose pose(doc);
    GlyphPolicy policy;
    policy.density = 20.0;  // 800x600 is 0.48 megapixels, so ~9 marks
    GlyphContext context;

    const std::vector<GlyphMark> marks =
        visibleGlyphs(doc, pose, glyphView(), W, H, context, policy);
    CHECK(marks.size() <= 10);
    CHECK_FALSE(marks.empty());

    // Selecting one puts it in the set, whatever the budget: a mark the user is
    // looking at is the one they asked about.
    const ConstraintRecord *last = doc.constraints().find(all.back());
    REQUIRE(last != nullptr);
    const std::vector<EntityId> selected{last->operands[0]};
    context.selected = selected;
    const std::vector<GlyphMark> withSelection =
        visibleGlyphs(doc, pose, glyphView(), W, H, context, policy);
    CHECK(withSelection.size() <= 10);
    CHECK(marksOf(withSelection, all.back()) == 1);
}

TEST_CASE("a fresh relation outranks an ordinary one") {
    // The moment a relation appears is the moment it most needs to be seen.
    Document doc;
    std::vector<ConstraintId> all;
    for(int i = 0; i < 40; i++) {
        const EntityId a = paroculus::test::addPoint(doc, -100.0 + i * 4.0, -100.0);
        const EntityId b = paroculus::test::addPoint(doc, -100.0 + i * 4.0, 100.0);
        const EntityId s = paroculus::test::addSegment(doc, a, b);
        all.push_back(paroculus::test::addConstraint(doc, ConstraintKind::Vertical, {s}));
    }

    const Pose pose(doc);
    GlyphPolicy policy;
    policy.density = 10.0;
    const std::vector<ConstraintId> fresh{all.back()};
    GlyphContext context;
    context.fresh = fresh;

    const std::vector<GlyphMark> marks =
        visibleGlyphs(doc, pose, glyphView(), W, H, context, policy);
    CHECK(marksOf(marks, all.back()) == 1);
    for(const GlyphMark &m : marks) {
        if(m.constraint == all.back()) CHECK(m.fresh);
    }
}

TEST_CASE("off-screen marks do not compete for the budget") {
    // A relation the user cannot see is not clutter, and letting it displace
    // one they can would make scrolling change which relations are legible.
    Document doc;
    const EntityId a = paroculus::test::addPoint(doc, 0.0, 0.0);
    const EntityId b = paroculus::test::addPoint(doc, 40.0, 0.0);
    const EntityId onScreen = paroculus::test::addSegment(doc, a, b);
    const ConstraintId visible =
        paroculus::test::addConstraint(doc, ConstraintKind::Horizontal, {onScreen});

    const EntityId c = paroculus::test::addPoint(doc, 90000.0, 90000.0);
    const EntityId d = paroculus::test::addPoint(doc, 90040.0, 90000.0);
    const EntityId far = paroculus::test::addSegment(doc, c, d);
    const ConstraintId hidden =
        paroculus::test::addConstraint(doc, ConstraintKind::Horizontal, {far});

    const Pose pose(doc);
    const GlyphContext context;
    const std::vector<GlyphMark> marks =
        visibleGlyphs(doc, pose, glyphView(), W, H, context, GlyphPolicy());
    CHECK(marksOf(marks, visible) == 1);
    CHECK(marksOf(marks, hidden) == 0);
}

TEST_CASE("the visible set is deterministic") {
    Document doc;
    for(int i = 0; i < 30; i++) {
        const EntityId a = paroculus::test::addPoint(doc, -100.0 + i * 5.0, -50.0);
        const EntityId b = paroculus::test::addPoint(doc, -100.0 + i * 5.0, 50.0);
        const EntityId s = paroculus::test::addSegment(doc, a, b);
        paroculus::test::addConstraint(doc, ConstraintKind::Vertical, {s});
    }
    const Pose pose(doc);
    GlyphPolicy policy;
    policy.density = 15.0;
    const GlyphContext context;

    const std::vector<GlyphMark> a = visibleGlyphs(doc, pose, glyphView(), W, H, context, policy);
    const std::vector<GlyphMark> b = visibleGlyphs(doc, pose, glyphView(), W, H, context, policy);
    REQUIRE(a.size() == b.size());
    for(size_t i = 0; i < a.size(); i++) {
        CHECK(a[i].constraint == b[i].constraint);
        CHECK(a[i].on == b[i].on);
    }
}

TEST_CASE("ghosts preview only what would actually be declared") {
    // An offer the user has not confirmed is a suggestion, not a relation about
    // to exist, and ghosting it would promise something commit will not do.
    SnapCandidate endpoint;
    endpoint.kind = SnapKind::Endpoint;  // auto-commits
    endpoint.subject = SnapSubject::PlacedPoint;

    SnapCandidate parallel;
    parallel.kind = SnapKind::Parallel;  // offered, unconfirmed
    parallel.subject = SnapSubject::PlacedSegment;

    SnapCandidate grid;
    grid.kind = SnapKind::Grid;  // declares nothing at all

    const std::vector<SnapCandidate> candidates{endpoint, parallel, grid};
    const std::vector<GlyphMark> ghosts =
        ghostGlyphs(candidates, Point{10.0, 20.0}, true, Point{0.0, 0.0});

    REQUIRE(ghosts.size() == 1);
    CHECK(ghosts[0].kind == ConstraintKind::Coincident);
    CHECK(ghosts[0].ghost);
    CHECK(ghosts[0].anchor.x == doctest::Approx(10.0));

    // Confirming the parallel makes it a relation about to exist, so it ghosts.
    std::vector<SnapCandidate> confirmed = candidates;
    confirmed[1].confirmed = true;
    const std::vector<GlyphMark> withParallel =
        ghostGlyphs(confirmed, Point{10.0, 20.0}, true, Point{0.0, 0.0});
    REQUIRE(withParallel.size() == 2);
    // A direction-valued relation sits on the segment, not on either end.
    const GlyphMark &onSegment = withParallel[1];
    CHECK(onSegment.kind == ConstraintKind::Parallel);
    CHECK(onSegment.anchor.x == doctest::Approx(5.0));
    CHECK(onSegment.anchor.y == doctest::Approx(10.0));
}

TEST_CASE("a session surfaces marks for what it has drawn") {
    // End to end: draw something, and the relations it declared are visible
    // rather than merely present.
    Document doc;
    paroculus::test::addPoint(doc, 40.0, 40.0);
    UndoJournal journal;
    Session session(doc, journal);
    Viewport viewport;
    viewport.view = glyphView();
    viewport.width = W;
    viewport.height = H;
    session.setViewport(viewport);
    session.setTool(ToolKind::Line);

    auto click = [&](Point p) {
        const Eigen::Vector2d s = viewport.view.toScreen(p);
        session.handle(PointerEvent::at(PointerAction::Move, s, viewport.view));
        session.handle(PointerEvent::at(PointerAction::Press, s, viewport.view, Button::Left));
    };
    click(Point{38.0, 41.0});
    click(Point{160.0, 41.0});

    REQUIRE_FALSE(session.presentation().inferred.empty());
    const std::vector<GlyphMark> marks = session.glyphs();
    CHECK_FALSE(marks.empty());
    for(ConstraintId id : session.presentation().inferred) {
        CHECK(marksOf(marks, id) >= 1);
    }
    // Just-declared relations are marked fresh, so the shell can say so.
    bool anyFresh = false;
    for(const GlyphMark &m : marks) anyFresh = anyFresh || m.fresh;
    CHECK(anyFresh);
}
