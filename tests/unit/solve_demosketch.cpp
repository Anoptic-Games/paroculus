#include <doctest/doctest.h>

#include <cmath>

#include "solve/demosketch.h"

using namespace paroculus;

TEST_CASE("the demo sketch solves its declared constraints") {
    // The same residual checks --selftest runs, reachable without a Qt
    // application. Stage 2 generalises this shape over the whole catalogue.
    const double ratio = 1.618;
    const Solution s = solveDemoSketch(ratio);

    REQUIRE(s.ok());
    CHECK(s.dof == 0);

    const double ax = s.a1.x - s.a0.x, ay = s.a1.y - s.a0.y;
    const double bx = s.b1.x - s.b0.x, by = s.b1.y - s.b0.y;
    const double lenA = std::hypot(ax, ay), lenB = std::hypot(bx, by);

    // Absolute tolerances, matching --selftest: document units are nominally
    // millimetres and this fixture sits at unit scale.
    CHECK(std::fabs(lenA - 120.0) <= 1e-6);
    CHECK(std::fabs(ay) <= 1e-6);                       // A horizontal
    CHECK(std::fabs(ax * by - ay * bx) <= 1e-5);        // B parallel to A
    CHECK(std::fabs(lenA / lenB - ratio) <= 1e-6);
}

TEST_CASE("the solve is deterministic") {
    // Same declaration, same seeds, same geometry — the precondition for
    // scrubbing, undo fidelity and every property test above this layer.
    const Solution a = solveDemoSketch(1.618);
    const Solution b = solveDemoSketch(1.618);
    CHECK(a.a1.x == b.a1.x);
    CHECK(a.a1.y == b.a1.y);
    CHECK(a.b1.x == b.b1.x);
    CHECK(a.b1.y == b.b1.y);
}

TEST_CASE("a non-positive ratio falls back to 1") {
    const Solution s = solveDemoSketch(-3.0);
    REQUIRE(s.ok());
    const double lenA = std::hypot(s.a1.x - s.a0.x, s.a1.y - s.a0.y);
    const double lenB = std::hypot(s.b1.x - s.b0.x, s.b1.y - s.b0.y);
    CHECK(lenA / lenB == doctest::Approx(1.0));
}

TEST_CASE("the demo is built from the declaration layer") {
    // Stage 1's visible outcome: the demo's geometry comes from a document, not
    // from literals, so the solve is downstream of declarations like every
    // future edit will be.
    const Document doc = paroculus::demoDocument(1.618);

    CHECK(doc.entities().size() == 6);   // four points, two segments
    CHECK(doc.constraints().size() == 6);

    int points = 0, segments = 0;
    for(const EntityRecord &e : doc.entities().records()) {
        if(e.kind == EntityKind::Point) points++;
        if(e.kind == EntityKind::Segment) segments++;
    }
    CHECK(points == 4);
    CHECK(segments == 2);

    // Seeds are off-constraint on purpose: a solver that echoed its input back
    // would report success and still fail every residual check.
    const EntityRecord &a1 = doc.entities().records()[1];
    CHECK(a1.seeds[1] != 0.0);
}

TEST_CASE("the ratio reaches the solver through a slot") {
    // The value is a slot on a constraint record, which is what makes the demo
    // slider the same mechanism a scrub or an expression will use.
    const Document doc = paroculus::demoDocument(2.5);
    bool found = false;
    for(const ConstraintRecord &c : doc.constraints().records()) {
        if(c.kind != ConstraintKind::LengthRatio) continue;
        found = true;
        CHECK(*doc.evaluate(c.value) == doctest::Approx(2.5));
    }
    CHECK(found);
}

TEST_CASE("solver status is mapped, never raw") {
    // SolveStatus is core's vocabulary; solve/ owns the translation from
    // SLVS_RESULT_*. A successful demo solve must land on one of the ok codes.
    const Solution s = solveDemoSketch(1.0);
    CHECK((s.status == SolveStatus::Okay || s.status == SolveStatus::RedundantOkay));
}
