// Branch selection, determinism, and component isolation.
//
// Constraint systems generically admit several solutions, and Newton converges
// to the one nearest its seed. Which branch the user is looking at is therefore
// part of the document, not an artefact of solving — and a branch flip mid-
// gesture is a bug by definition, reproducible from recorded seeds, testable.
// This file is where that gets tested.
#include <doctest/doctest.h>

#include <cmath>

#include "core/topology.h"
#include "solve/solve.h"
#include "support/build.h"

using namespace paroculus;
using paroculus::test::Rng;
using paroculus::test::addConstraint;
using paroculus::test::addPoint;
using paroculus::test::addSegment;

namespace {

// The canonical two-solution fixture: a point at fixed distances from two
// pinned points. The two circles meet twice — once above the baseline, once
// below — and both satisfy every constraint. Only the seed decides which one
// the user gets.
struct TwoBranch {
    Document doc;
    EntityId left, right, apex;
    ConstraintId leftDistance, rightDistance;
};

TwoBranch buildTwoBranch(double apexSeedY, double leftSpan = 10.0, double reach = 13.0) {
    TwoBranch f;
    f.left = addPoint(f.doc, 0.0, 0.0);
    f.right = addPoint(f.doc, leftSpan, 0.0);
    // Seeded off the solution on purpose; only the sign of y should decide the
    // branch, not the accuracy of the guess.
    f.apex = addPoint(f.doc, leftSpan * 0.5 + 1.0, apexSeedY);

    addConstraint(f.doc, ConstraintKind::Pin, {f.left});
    addConstraint(f.doc, ConstraintKind::Pin, {f.right});
    f.leftDistance =
        addConstraint(f.doc, ConstraintKind::PointPointDistance, {f.left, f.apex}, Slot(reach));
    f.rightDistance =
        addConstraint(f.doc, ConstraintKind::PointPointDistance, {f.right, f.apex}, Slot(reach));
    return f;
}

double solvedApexY(const TwoBranch &f) {
    SolveContext context = SolveContext::forWholeDocument(f.doc);
    const SolveOutcome outcome = solve(f.doc, context);
    REQUIRE(outcome.ok());
    return context.point(f.apex)->y;
}

}  // namespace

TEST_CASE("seed proximity selects the branch") {
    // The same declaration, two seeds, two different legal answers. A document
    // without seeds would be free to hand back either one on reopen.
    CHECK(solvedApexY(buildTwoBranch(5.0)) > 0.0);
    CHECK(solvedApexY(buildTwoBranch(-5.0)) < 0.0);

    // Both are genuinely solutions: same magnitude, opposite sign.
    CHECK(std::fabs(solvedApexY(buildTwoBranch(5.0)) + solvedApexY(buildTwoBranch(-5.0))) <=
          1e-9);
}

TEST_CASE("a cold re-solve from stored seeds reproduces the recorded branch") {
    // File reopen, undo, and scrub are all this operation. Re-solving from
    // scratch is never correctness-neutral, which is why seeds are persisted.
    for(double seedY : {7.0, -7.0}) {
        TwoBranch f = buildTwoBranch(seedY);

        // Solve, then commit the solved pose back as the document's seeds, as
        // a release would.
        SolveContext context = SolveContext::forWholeDocument(f.doc);
        REQUIRE(solve(f.doc, context).ok());
        for(const Command &c : context.commitCommands(f.doc)) REQUIRE(f.doc.apply(c).ok());
        const double recorded = context.point(f.apex)->y;

        // Now solve cold from those stored seeds, as a fresh open would.
        SolveContext reopened = SolveContext::forWholeDocument(f.doc);
        REQUIRE(solve(f.doc, reopened).ok());
        CHECK(reopened.point(f.apex)->y == doctest::Approx(recorded));
        CHECK((recorded > 0.0) == (seedY > 0.0));
    }
}

TEST_CASE("a warm-started sweep never flips the branch") {
    // Consecutive solves along a parameter sweep stay on the same branch
    // because each starts from the last. A flip mid-sweep is the bug this
    // sign invariant exists to catch.
    for(double seedY : {6.0, -6.0}) {
        TwoBranch f = buildTwoBranch(seedY);
        SolveContext context = SolveContext::forWholeDocument(f.doc);
        REQUIRE(solve(f.doc, context).ok());
        const bool above = context.point(f.apex)->y > 0.0;

        // Scrub the reach up and back down, warm-starting each step from the
        // last solved pose — which is what a scrub actually does.
        for(double reach = 13.0; reach <= 40.0; reach += 0.5) {
            ConstraintRecord updated = *f.doc.constraints().find(f.leftDistance);
            updated.value = Slot(reach);
            REQUIRE(f.doc.apply(SetRecord<ConstraintRecord>{updated}).ok());
            ConstraintRecord other = *f.doc.constraints().find(f.rightDistance);
            other.value = Slot(reach);
            REQUIRE(f.doc.apply(SetRecord<ConstraintRecord>{other}).ok());

            const SolveOutcome outcome = solve(f.doc, context);
            REQUIRE(outcome.ok());
            CHECK_MESSAGE((context.point(f.apex)->y > 0.0) == above, "flipped at reach ", reach);
        }
        for(double reach = 40.0; reach >= 13.0; reach -= 0.5) {
            ConstraintRecord updated = *f.doc.constraints().find(f.leftDistance);
            updated.value = Slot(reach);
            REQUIRE(f.doc.apply(SetRecord<ConstraintRecord>{updated}).ok());
            ConstraintRecord other = *f.doc.constraints().find(f.rightDistance);
            other.value = Slot(reach);
            REQUIRE(f.doc.apply(SetRecord<ConstraintRecord>{other}).ok());

            REQUIRE(solve(f.doc, context).ok());
            CHECK_MESSAGE((context.point(f.apex)->y > 0.0) == above, "flipped at reach ", reach);
        }
    }
}

TEST_CASE("repeated solves are bitwise identical") {
    // Determinism is a document property. Same declaration, same seeds, same
    // geometry — every run, or scrubbing and undo fidelity mean nothing.
    const TwoBranch f = buildTwoBranch(5.0);

    SolveContext first = SolveContext::forWholeDocument(f.doc);
    REQUIRE(solve(f.doc, first).ok());

    for(int i = 0; i < 16; i++) {
        SolveContext again = SolveContext::forWholeDocument(f.doc);
        REQUIRE(solve(f.doc, again).ok());
        // Bit-exact, not approximate.
        CHECK(again.params() == first.params());
    }
}

TEST_CASE("a drag leaves other components bit-unchanged") {
    // Drag locality, stated as a measurable invariant: geometry that is not
    // connected cannot move at all, so a solve scoped to one component leaves
    // every other parameter untouched to the bit.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 3.0);
    const EntityId near = addSegment(doc, a, b);
    // Pinned so the solved pose is determinate: horizontality alone leaves the
    // solver free to move either endpoint, and this test is about locality,
    // not about which end it picks.
    addConstraint(doc, ConstraintKind::Pin, {a});

    const EntityId c = addPoint(doc, 200.0, 200.0);
    const EntityId d = addPoint(doc, 210.0, 207.0);
    const EntityId far = addSegment(doc, c, d);
    addConstraint(doc, ConstraintKind::Horizontal, {far});

    Topology topology(doc);
    REQUIRE(topology.componentCount() == 2);

    // Record the far component before touching the near one.
    SolveContext farBefore = SolveContext::forComponent(doc, topology, c);
    REQUIRE(solve(doc, farBefore).ok());

    addConstraint(doc, ConstraintKind::Horizontal, {near});
    topology.markDirty();

    SolveContext nearContext = SolveContext::forComponent(doc, topology, a);
    REQUIRE(solve(doc, nearContext).ok());
    // The near component actually moved, so the check below means something.
    CHECK(nearContext.point(b)->y == doctest::Approx(0.0));
    CHECK_FALSE(nearContext.contains(c));

    SolveContext farAfter = SolveContext::forComponent(doc, topology, c);
    REQUIRE(solve(doc, farAfter).ok());
    CHECK(farAfter.params() == farBefore.params());
}

TEST_CASE("a component solve carries only the relations that bind it") {
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 3.0);
    addSegment(doc, a, b);
    const EntityId lonely = addPoint(doc, 50.0, 50.0);

    Topology topology(doc);
    SolveContext context = SolveContext::forComponent(doc, topology, a);
    CHECK(context.contains(a));
    CHECK_FALSE(context.contains(lonely));

    const SolveOutcome outcome = solve(doc, context);
    REQUIRE(outcome.ok());
    // Four free parameters in the component, none from the lonely point.
    CHECK(outcome.dof == 4);
}

TEST_CASE("the dragged set is soft and a pin is hard") {
    // Dragging favours keeping a point put; pinning refuses to move it. The
    // user-facing pin is one keystroke and visible like any constraint, and it
    // can over-constrain — which is exactly the difference being tested.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 0.0);
    addConstraint(doc, ConstraintKind::Pin, {a});
    addConstraint(doc, ConstraintKind::PointPointDistance, {a, b}, Slot(20.0));

    SolveOptions options;
    options.dragged = {b};

    SolveContext context = SolveContext::forWholeDocument(doc);
    REQUIRE(solve(doc, context, options).ok());

    // The pin held; the dragged point moved to satisfy the distance.
    CHECK(context.point(a)->x == doctest::Approx(0.0));
    CHECK(context.point(a)->y == doctest::Approx(0.0));
    const Point moved = *context.point(b);
    CHECK(std::hypot(moved.x, moved.y) == doctest::Approx(20.0));
    // And it stayed on its own side rather than jumping across the pin.
    CHECK(moved.x > 0.0);
}

TEST_CASE("a solve never mutates the document") {
    // The document is immutable during a solve, which is what makes speculative
    // previews safe by construction rather than by remembering not to save.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 7.0);
    const EntityId segment = addSegment(doc, a, b);
    addConstraint(doc, ConstraintKind::Pin, {a});
    addConstraint(doc, ConstraintKind::Horizontal, {segment});

    const Document before = doc;
    SolveContext context = SolveContext::forWholeDocument(doc);
    REQUIRE(solve(doc, context).ok());

    CHECK(doc == before);
    // The solved pose lives in the context until it is explicitly committed.
    CHECK(context.point(b)->y == doctest::Approx(0.0));
    CHECK(doc.entities().find(b)->seeds[1] == 7.0);

    const std::vector<Command> commit = context.commitCommands(doc);
    CHECK_FALSE(commit.empty());
    for(const Command &c : commit) REQUIRE(doc.apply(c).ok());
    CHECK(doc.entities().find(b)->seeds[1] == doctest::Approx(0.0));
}

TEST_CASE("committing an unchanged solve journals nothing") {
    // A drag that moved nothing must not litter the undo stack.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    addPoint(doc, 10.0, 0.0);

    SolveContext context = SolveContext::forWholeDocument(doc);
    REQUIRE(solve(doc, context).ok());
    for(const Command &c : context.commitCommands(doc)) REQUIRE(doc.apply(c).ok());

    SolveContext again = SolveContext::forWholeDocument(doc);
    REQUIRE(solve(doc, again).ok());
    CHECK(again.commitCommands(doc).empty());
    CHECK(a.valid());
}

TEST_CASE("speculative constraints leave no trace") {
    // Preview-does-not-mutate, at the solve seam. A candidate rides in as an
    // extra constraint on a forked context; nothing is copied and nothing is
    // written back.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 7.0);
    const EntityId segment = addSegment(doc, a, b);
    addConstraint(doc, ConstraintKind::Pin, {a});
    const Document before = doc;

    SolveContext committed = SolveContext::forWholeDocument(doc);
    REQUIRE(solve(doc, committed).ok());

    SolveContext ghost = committed;
    SolveOptions options;
    ConstraintRecord candidate;
    candidate.kind = ConstraintKind::Horizontal;
    candidate.operands = {segment, EntityId(), EntityId(), EntityId()};
    options.extra.push_back(candidate);
    REQUIRE(solve(doc, ghost, options).ok());

    // The ghost moved; the document and the committed context did not.
    CHECK(ghost.point(b)->y == doctest::Approx(0.0));
    CHECK(committed.point(b)->y == doctest::Approx(7.0));
    CHECK(doc == before);
    CHECK(doc.constraints().size() == 1);  // the pin, and nothing speculative
}

TEST_CASE("property: solving is insensitive to the order geometry was declared") {
    // Determinism within tolerance under a permuted build. Parameter numbering
    // follows ID order, so a document assembled in a different order is a
    // different numbering of the same system and must still land in the same
    // place.
    for(uint64_t seed = 1; seed <= 12; seed++) {
        Rng rng(seed * 2654435761u);

        // A chain of segments held horizontal, with one end pinned.
        const int count = 4 + static_cast<int>(rng.below(3));
        std::vector<double> xs, ys;
        for(int i = 0; i <= count; i++) {
            xs.push_back(i * 10.0);
            ys.push_back(rng.real(-4.0, 4.0));
        }

        auto build = [&](bool reversed) {
            Document doc;
            std::vector<EntityId> points(xs.size());
            for(size_t i = 0; i < xs.size(); i++) {
                const size_t k = reversed ? xs.size() - 1 - i : i;
                points[k] = addPoint(doc, xs[k], ys[k]);
            }
            for(size_t i = 0; i + 1 < points.size(); i++) {
                const EntityId s = addSegment(doc, points[i], points[i + 1]);
                addConstraint(doc, ConstraintKind::Horizontal, {s});
            }
            addConstraint(doc, ConstraintKind::Pin, {points[0]});

            SolveContext context = SolveContext::forWholeDocument(doc);
            REQUIRE(solve(doc, context).ok());
            std::vector<Point> out;
            for(EntityId p : points) out.push_back(*context.point(p));
            return out;
        };

        const std::vector<Point> forward = build(false);
        const std::vector<Point> backward = build(true);
        REQUIRE(forward.size() == backward.size());
        for(size_t i = 0; i < forward.size(); i++) {
            CHECK_MESSAGE(forward[i].x == doctest::Approx(backward[i].x), "seed ", seed);
            CHECK_MESSAGE(forward[i].y == doctest::Approx(backward[i].y), "seed ", seed);
        }
    }
}
