#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "interact/glyphs.h"
#include "interact/hit.h"
#include "interact/registry.h"
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

TEST_CASE("an arc carries the marks of what binds it") {
    // anchorOn() handled points, segments and circles and returned nullopt for
    // an arc, so the arc macro's own point-on-circle was invisible from the arc
    // side. Mark-per-operand is not per-operand if a kind can silently opt out.
    Document doc;
    const EntityId centre = paroculus::test::addPoint(doc, 0.0, 0.0);
    const EntityId start = paroculus::test::addPoint(doc, 50.0, 0.0);
    const EntityId end = paroculus::test::addPoint(doc, 0.0, 50.0);
    const EntityId arc = paroculus::test::addArc(doc, centre, start, end);
    const EntityId through = paroculus::test::addPoint(doc, 50.0, 0.0);

    const ConstraintId onArc =
        paroculus::test::addConstraint(doc, ConstraintKind::PointOnCircle, {through, arc});
    REQUIRE(onArc.valid());

    const Pose pose(doc);
    std::vector<GlyphMark> marks;
    for(const ConstraintRecord &r : doc.constraints().records()) marksFor(doc, pose, r, marks);

    CHECK(marksOf(marks, onArc) == 2);

    const auto onCurve = std::find_if(marks.begin(), marks.end(),
                                      [&](const GlyphMark &m) { return m.on == arc; });
    REQUIRE(onCurve != marks.end());
    // On the rim at the middle of the sweep, where the arc is drawn — not on
    // its centre, which is construction geometry carrying its own marks.
    CHECK(onCurve->anchor.x == doctest::Approx(50.0 * std::cos(0.785398163)));
    CHECK(onCurve->anchor.y == doctest::Approx(50.0 * std::sin(0.785398163)));
}

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
        visibleGlyphs(doc, pose, glyphView(), W, H, context, policy).marks;
    CHECK(marks.size() <= 10);
    CHECK_FALSE(marks.empty());

    // Selecting one puts it in the set, whatever the budget: a mark the user is
    // looking at is the one they asked about.
    const ConstraintRecord *last = doc.constraints().find(all.back());
    REQUIRE(last != nullptr);
    const std::vector<EntityId> selected{last->operands[0]};
    context.selected = selected;
    const std::vector<GlyphMark> withSelection =
        visibleGlyphs(doc, pose, glyphView(), W, H, context, policy).marks;
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
        visibleGlyphs(doc, pose, glyphView(), W, H, context, policy).marks;
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
        visibleGlyphs(doc, pose, glyphView(), W, H, context, GlyphPolicy()).marks;
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

    const std::vector<GlyphMark> a = visibleGlyphs(doc, pose, glyphView(), W, H, context, policy).marks;
    const std::vector<GlyphMark> b = visibleGlyphs(doc, pose, glyphView(), W, H, context, policy).marks;
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
        ghostGlyphs(candidates, Point{10.0, 20.0}, PlacementRoles{true, true, false},
                    Point{0.0, 0.0});

    REQUIRE(ghosts.size() == 1);
    CHECK(ghosts[0].kind == ConstraintKind::Coincident);
    CHECK(ghosts[0].ghost);
    CHECK(ghosts[0].anchor.x == doctest::Approx(10.0));

    // Confirming the parallel makes it a relation about to exist, so it ghosts.
    std::vector<SnapCandidate> confirmed = candidates;
    confirmed[1].confirmed = true;
    const std::vector<GlyphMark> withParallel =
        ghostGlyphs(confirmed, Point{10.0, 20.0}, PlacementRoles{true, true, false},
                    Point{0.0, 0.0});
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

TEST_CASE("a mark is picked where it is drawn") {
    // Placement is shared with render through layOutGlyphs. If hit testing
    // computed the fan-out separately the two would drift and the user would
    // click a mark and select nothing — the same failure the pose exists to
    // rule out, one layer up.
    Document doc;
    const EntityId a = paroculus::test::addPoint(doc, -40.0, 0.0);
    const EntityId b = paroculus::test::addPoint(doc, 40.0, 0.0);
    const EntityId segment = paroculus::test::addSegment(doc, a, b);
    const ConstraintId horizontal =
        paroculus::test::addConstraint(doc, ConstraintKind::Horizontal, {segment});

    UndoJournal journal;
    Session session(doc, journal);
    Viewport viewport;
    viewport.view = glyphView();
    viewport.width = W;
    viewport.height = H;
    session.setViewport(viewport);

    const std::vector<GlyphMark> marks = session.glyphs();
    REQUIRE(marksOf(marks, horizontal) >= 1);

    const std::vector<Eigen::Vector2d> places = layOutGlyphs(marks, viewport.view);
    REQUIRE(places.size() == marks.size());

    // Every mark is pickable at exactly the place it was laid out.
    for(size_t i = 0; i < marks.size(); i++) {
        const std::optional<GlyphHit> hit = hitGlyph(marks, viewport.view, places[i]);
        REQUIRE(hit);
        CHECK(hit->constraint == marks[i].constraint);
    }

    // And nowhere near it, nothing is picked.
    CHECK_FALSE(hitGlyph(marks, viewport.view, Eigen::Vector2d(5.0, 5.0)));
}

TEST_CASE("clicking a mark selects the relation it stands for") {
    Document doc;
    const EntityId a = paroculus::test::addPoint(doc, -40.0, 0.0);
    const EntityId b = paroculus::test::addPoint(doc, 40.0, 0.0);
    const EntityId segment = paroculus::test::addSegment(doc, a, b);
    const ConstraintId horizontal =
        paroculus::test::addConstraint(doc, ConstraintKind::Horizontal, {segment});

    UndoJournal journal;
    Session session(doc, journal);
    Viewport viewport;
    viewport.view = glyphView();
    viewport.width = W;
    viewport.height = H;
    session.setViewport(viewport);

    const std::vector<GlyphMark> marks = session.glyphs();
    const std::vector<Eigen::Vector2d> places = layOutGlyphs(marks, viewport.view);
    REQUIRE(!places.empty());

    session.handle(
        PointerEvent::at(PointerAction::Press, places.front(), viewport.view, Button::Left));

    // The relation is selected, and the geometry is not: the question the user
    // asked was about the relation.
    CHECK(session.selection().contains(horizontal));
    CHECK(session.selection().items().empty());

    // Which makes the driving toggle applicable, so a conflict set is walkable
    // and a dimension is editable by exactly the machinery geometry uses.
    CHECK(invokeAction(session, "relation.toggle-driving"));
    CHECK_FALSE(doc.constraints().find(horizontal)->driving);
}

TEST_CASE("a selected relation deletes like any other selection") {
    // The gesture the conflict walk exists to enable: read the set, delete the
    // relation that is wrong. deleteSelection built its step from the entity
    // items alone and edit.delete read the geometry signature, so Delete on a
    // constraint-only selection was a silent no-op and the palette dimmed it
    // exactly while a walked conflict set was selected.
    Document doc;
    const EntityId a = paroculus::test::addPoint(doc, -40.0, 0.0);
    const EntityId b = paroculus::test::addPoint(doc, 40.0, 0.0);
    const EntityId segment = paroculus::test::addSegment(doc, a, b);
    const ConstraintId horizontal =
        paroculus::test::addConstraint(doc, ConstraintKind::Horizontal, {segment});

    UndoJournal journal;
    Session session(doc, journal);
    Viewport viewport;
    viewport.view = glyphView();
    viewport.width = W;
    viewport.height = H;
    session.setViewport(viewport);

    const std::vector<GlyphMark> marks = session.glyphs();
    const std::vector<Eigen::Vector2d> places = layOutGlyphs(marks, viewport.view);
    REQUIRE(!places.empty());
    session.handle(
        PointerEvent::at(PointerAction::Press, places.front(), viewport.view, Button::Left));
    REQUIRE(session.selection().contains(horizontal));

    // Offered, and it acts.
    REQUIRE(invokeAction(session, "edit.delete"));
    CHECK_FALSE(doc.constraints().contains(horizontal));
    // The geometry the relation bound is untouched: only the declaration went.
    CHECK(doc.entities().contains(segment));
    CHECK(session.presentation().deletedRelations == 1);
    CHECK(session.presentation().deletedEntities == 0);

    // And it is one undoable step like every other edit.
    session.handle(Key::Undo);
    CHECK(doc.constraints().contains(horizontal));
}

TEST_CASE("a mark does not swallow the press that starts a drag") {
    // Adorners sit above geometry in the priority policy, but a mark sits a few
    // pixels off the vertex it annotates — well inside that vertex's own hit
    // radius. A mark that always won would spend the gesture the tool is mostly
    // used for on the one it is occasionally used for. Nearer wins.
    Document doc;
    const EntityId a = paroculus::test::addPoint(doc, -40.0, 0.0);
    const EntityId b = paroculus::test::addPoint(doc, 40.0, 0.0);
    const EntityId segment = paroculus::test::addSegment(doc, a, b);
    paroculus::test::addConstraint(doc, ConstraintKind::Horizontal, {segment});

    UndoJournal journal;
    Session session(doc, journal);
    Viewport viewport;
    viewport.view = glyphView();
    viewport.width = W;
    viewport.height = H;
    session.setViewport(viewport);

    // Dead on the vertex: the geometry wins, and the drag starts.
    const Eigen::Vector2d onVertex = viewport.view.toScreen(*session.pose().point(b));
    session.handle(PointerEvent::at(PointerAction::Press, onVertex, viewport.view, Button::Left));
    CHECK(session.selection().contains(b));
    CHECK(session.selection().constraints().empty());
}

TEST_CASE("a crowded anchor caps its fan and adds one overflow mark") {
    // The per-anchor fan cap: beyond the cap the marks collapse into one ⋯ that
    // stands for the crowd and opens the inspector on the anchor's operand.
    Document doc;
    const EntityId centre = paroculus::test::addPoint(doc, 0.0, 0.0);
    std::vector<ConstraintId> coincidences;
    for(int i = 0; i < 8; i++) {
        const EntityId q = paroculus::test::addPoint(doc, 20.0 + i, 30.0 + i);
        coincidences.push_back(
            paroculus::test::addConstraint(doc, ConstraintKind::Coincident, {centre, q}));
    }

    const Pose pose(doc);
    GlyphPolicy policy;  // fanLimit 5, density 90 — no global truncation here
    const GlyphContext context;
    const VisibleGlyphs vg =
        visibleGlyphs(doc, pose, glyphView(), W, H, context, policy);

    // Exactly one ⋯, sitting on the crowded anchor's operand.
    const size_t overflow = static_cast<size_t>(
        std::count_if(vg.marks.begin(), vg.marks.end(),
                      [](const GlyphMark &m) { return m.overflow; }));
    CHECK(overflow == 1);
    const auto it = std::find_if(vg.marks.begin(), vg.marks.end(),
                                 [](const GlyphMark &m) { return m.overflow; });
    REQUIRE(it != vg.marks.end());
    CHECK(it->on == centre);
    // At most fanLimit non-overflow marks sit on the centre.
    const size_t onCentre = static_cast<size_t>(std::count_if(
        vg.marks.begin(), vg.marks.end(),
        [&](const GlyphMark &m) { return !m.overflow && m.on == centre; }));
    CHECK(onCentre == policy.fanLimit);
}

TEST_CASE("hit testing picks the overflow mark where the shared layout draws it") {
    // One-layout-two-readers: the ⋯ is placed by layOutGlyphs and picked through
    // the same layOutGlyphs, so a click lands on it exactly where it is drawn.
    Document doc;
    const EntityId centre = paroculus::test::addPoint(doc, 0.0, 0.0);
    for(int i = 0; i < 8; i++) {
        const EntityId q = paroculus::test::addPoint(doc, 20.0 + i, 30.0 + i);
        paroculus::test::addConstraint(doc, ConstraintKind::Coincident, {centre, q});
    }
    const Pose pose(doc);
    const VisibleGlyphs vg =
        visibleGlyphs(doc, pose, glyphView(), W, H, GlyphContext(), GlyphPolicy());

    const std::vector<Eigen::Vector2d> places = layOutGlyphs(vg.marks, glyphView());
    size_t overflowIndex = vg.marks.size();
    for(size_t i = 0; i < vg.marks.size(); i++) {
        if(vg.marks[i].overflow) overflowIndex = i;
    }
    REQUIRE(overflowIndex < vg.marks.size());

    const std::optional<GlyphHit> hit =
        hitGlyph(vg.marks, glyphView(), places[overflowIndex]);
    REQUIRE(hit.has_value());
    CHECK(hit->overflow);
    CHECK(hit->on == centre);
    CHECK_FALSE(hit->constraint.valid());
}

TEST_CASE("visibleGlyphs reports how many relations it showed of how many exist") {
    // Honest truncation: the global budget drops whole relations, and the counts
    // say so rather than the overlay vanishing them silently.
    Document doc;
    std::vector<ConstraintId> all;
    for(int i = 0; i < 40; i++) {
        const EntityId a = paroculus::test::addPoint(doc, -100.0 + i * 3.0, -100.0);
        const EntityId b = paroculus::test::addPoint(doc, -100.0 + i * 3.0, 100.0);
        const EntityId s = paroculus::test::addSegment(doc, a, b);
        all.push_back(paroculus::test::addConstraint(doc, ConstraintKind::Vertical, {s}));
    }
    const Pose pose(doc);
    GlyphPolicy policy;
    policy.density = 20.0;  // 0.48 megapixels -> budget 9
    const VisibleGlyphs vg =
        visibleGlyphs(doc, pose, glyphView(), W, H, GlyphContext(), policy);
    CHECK(vg.total == 40);
    CHECK(vg.shown < vg.total);
    CHECK(vg.shown == 9);
}

TEST_CASE("mnemonic labels ride the budget: on when loose, off when tight, never on values") {
    SUBCASE("a loose overlay labels its unvalued marks") {
        Document doc;
        for(int i = 0; i < 3; i++) {
            const EntityId a = paroculus::test::addPoint(doc, -60.0 + i * 40.0, 0.0);
            const EntityId b = paroculus::test::addPoint(doc, -40.0 + i * 40.0, 0.0);
            const EntityId s = paroculus::test::addSegment(doc, a, b);
            paroculus::test::addConstraint(doc, ConstraintKind::Horizontal, {s});
        }
        const Pose pose(doc);
        const VisibleGlyphs vg =
            visibleGlyphs(doc, pose, glyphView(), W, H, GlyphContext(), GlyphPolicy());
        REQUIRE(vg.marks.size() == 3);
        for(const GlyphMark &m : vg.marks) CHECK(m.showLabel);
    }

    SUBCASE("a tight overlay drops the labels before the marks") {
        Document doc;
        for(int i = 0; i < 22; i++) {
            const EntityId a = paroculus::test::addPoint(doc, -120.0 + i * 10.0, -20.0);
            const EntityId b = paroculus::test::addPoint(doc, -110.0 + i * 10.0, -20.0);
            const EntityId s = paroculus::test::addSegment(doc, a, b);
            paroculus::test::addConstraint(doc, ConstraintKind::Horizontal, {s});
        }
        const Pose pose(doc);
        const VisibleGlyphs vg =
            visibleGlyphs(doc, pose, glyphView(), W, H, GlyphContext(), GlyphPolicy());
        // Many marks survive, but the density is now above the label threshold,
        // so the mnemonics are the first thing gone.
        REQUIRE(vg.marks.size() > 10);
        for(const GlyphMark &m : vg.marks) CHECK_FALSE(m.showLabel);
    }

    SUBCASE("a valued mark never carries a mnemonic — its value is its label") {
        Document doc;
        const EntityId a = paroculus::test::addPoint(doc, -30.0, 0.0);
        const EntityId b = paroculus::test::addPoint(doc, 30.0, 0.0);
        paroculus::test::addConstraint(doc, ConstraintKind::PointPointDistance, {a, b},
                                       Slot(60.0));
        const Pose pose(doc);
        const VisibleGlyphs vg =
            visibleGlyphs(doc, pose, glyphView(), W, H, GlyphContext(), GlyphPolicy());
        for(const GlyphMark &m : vg.marks) CHECK_FALSE(m.showLabel);
    }
}

TEST_CASE("pressing the overflow mark selects the anchor and asks to reveal the inspector") {
    // The ⋯ opens the crowd: it selects its anchor's operand — so the inspector's
    // unbudgeted list is filtered to that anchor — and flags the shell to reveal
    // the panel. It selects geometry, never a constraint.
    Document doc;
    const EntityId centre = paroculus::test::addPoint(doc, 0.0, 0.0);
    for(int i = 0; i < 8; i++) {
        const EntityId q = paroculus::test::addPoint(doc, 20.0 + i, 30.0 + i);
        paroculus::test::addConstraint(doc, ConstraintKind::Coincident, {centre, q});
    }
    UndoJournal journal;
    Session session(doc, journal);
    Viewport viewport;
    viewport.view = glyphView();
    viewport.width = W;
    viewport.height = H;
    session.setViewport(viewport);

    const std::vector<GlyphMark> marks = session.glyphs();
    const std::vector<Eigen::Vector2d> places = layOutGlyphs(marks, viewport.view);
    size_t overflowIndex = marks.size();
    for(size_t i = 0; i < marks.size(); i++) {
        if(marks[i].overflow) overflowIndex = i;
    }
    REQUIRE(overflowIndex < marks.size());

    session.handle(PointerEvent::at(PointerAction::Press, places[overflowIndex], viewport.view,
                                    Button::Left));
    CHECK(session.selection().contains(centre));
    CHECK(session.selection().constraints().empty());
    CHECK(session.presentation().overflowPicked);

    // And a subsequent event clears the one-shot flag.
    session.handle(PointerEvent::at(PointerAction::Move, Eigen::Vector2d(1.0, 1.0), viewport.view,
                                    Button::None));
    CHECK_FALSE(session.presentation().overflowPicked);
}
