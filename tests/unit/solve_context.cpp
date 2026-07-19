#include <doctest/doctest.h>

#include "core/topology.h"
#include "solve/arena.h"
#include "solve/context.h"
#include "solve/solve.h"
#include "support/build.h"

using namespace paroculus;
using paroculus::test::addCircle;
using paroculus::test::addConstraint;
using paroculus::test::addPoint;
using paroculus::test::addSegment;

TEST_CASE("the arena hands out zeroed, aligned storage") {
    // The slvs structs are filled field by field, and a stray handle from
    // uninitialised memory is a silent, irreproducible wrong answer.
    SolveArena arena;
    CHECK(arena.bytesAllocated() == 0);

    auto *values = arena.array<double>(64);
    REQUIRE(values != nullptr);
    for(int i = 0; i < 64; i++) CHECK(values[i] == 0.0);
    CHECK(reinterpret_cast<uintptr_t>(values) % alignof(double) == 0);
    CHECK(arena.bytesAllocated() >= 64 * sizeof(double));

    CHECK(arena.array<double>(0) == nullptr);
}

TEST_CASE("an arena moves without double-freeing its heap") {
    SolveArena first;
    auto *values = first.array<int>(8);
    values[0] = 42;

    SolveArena second = std::move(first);
    CHECK(second.bytesAllocated() >= 8 * sizeof(int));
    CHECK(values[0] == 42);
    // The moved-from arena owns nothing and must survive its own destructor.
    CHECK(first.bytesAllocated() == 0);
}

TEST_CASE("a context holds only the param-owning members") {
    Document doc;
    const EntityId a = addPoint(doc, 1.0, 2.0);
    const EntityId b = addPoint(doc, 3.0, 4.0);
    const EntityId segment = addSegment(doc, a, b);

    const SolveContext context = SolveContext::forWholeDocument(doc);
    CHECK(context.members().size() == 3);
    // A segment borrows everything from its endpoints, so it carries no params.
    CHECK(context.params().size() == 2);
    CHECK(context.contains(segment));
    CHECK_FALSE(context.point(segment).has_value());

    CHECK(context.point(a)->x == 1.0);
    CHECK(context.point(b)->y == 4.0);
}

TEST_CASE("a circle's radius is seeded from the context, not from the document") {
    // The context is the parameter store — seeds going in, solved values coming
    // out — and a circle's radius is a parameter like any other. Reading the
    // document record instead would re-seed the radius from the committed value
    // on every frame of a drag, and would discard any speculative context that
    // perturbed it.
    Document doc;
    const EntityId centre = addPoint(doc, 0.0, 0.0);
    const EntityId circle = addCircle(doc, centre, 10.0);
    REQUIRE(circle.valid());

    SolveContext context = SolveContext::forWholeDocument(doc);
    REQUIRE(context.radius(circle) == doctest::Approx(10.0));

    // Perturb the context alone. Nothing constrains the radius, so the solver
    // leaves it where it was handed it — which is the point of the check.
    for(SeedSpan &span : context.params()) {
        if(span.entity == circle) span.seeds[0] = 25.0;
    }

    const SolveOutcome outcome = solve(doc, context, SolveOptions());
    REQUIRE(outcome.ok());
    CHECK(context.radius(circle) == doctest::Approx(25.0));
    // And the document is untouched: a solve never writes back.
    CHECK(doc.entities().find(circle)->seeds[0] == 10.0);
}

TEST_CASE("a speculative radius constraint reaches the circle it names") {
    // The same seam from the other side: options.extra rides in without a
    // document copy, so a preview of "make this radius 30" has to move it.
    Document doc;
    const EntityId centre = addPoint(doc, 0.0, 0.0);
    const EntityId circle = addCircle(doc, centre, 10.0);

    SolveContext context = SolveContext::forWholeDocument(doc);
    ConstraintRecord candidate;
    candidate.kind = ConstraintKind::Radius;
    candidate.operands[0] = circle;
    candidate.value = Slot(30.0);

    SolveOptions options;
    options.extra = {candidate};
    const SolveOutcome outcome = solve(doc, context, options);

    REQUIRE(outcome.ok());
    CHECK(context.radius(circle) == doctest::Approx(30.0));
    CHECK(doc.entities().find(circle)->seeds[0] == 10.0);
}

TEST_CASE("members are id-ordered whatever order the partition enumerated") {
    // Translation order follows this, and therefore so does the solver's
    // parameter numbering, and therefore its arithmetic.
    Document doc;
    for(int i = 0; i < 6; i++) addPoint(doc, i, i);
    const SolveContext context = SolveContext::forWholeDocument(doc);

    for(size_t i = 1; i < context.members().size(); i++) {
        CHECK(context.members()[i - 1] < context.members()[i]);
    }
    for(size_t i = 1; i < context.params().size(); i++) {
        CHECK(context.params()[i - 1].entity < context.params()[i].entity);
    }
}

TEST_CASE("a context is a value type, cheap to fork and independent once forked") {
    // The capability behind previews, warm starts, async solving and animation
    // evaluation: fork the parameters, solve the copy, throw it away.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    addPoint(doc, 10.0, 5.0);

    SolveContext original = SolveContext::forWholeDocument(doc);
    SolveContext fork = original;
    CHECK(fork == original);

    fork.params()[0].seeds = {99.0, 99.0};
    CHECK(fork != original);
    CHECK(original.point(a)->x == 0.0);
}

TEST_CASE("a component context excludes everything it is not connected to") {
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 0.0);
    addSegment(doc, a, b);
    const EntityId far = addPoint(doc, 500.0, 500.0);

    Topology topology(doc);
    const SolveContext near = SolveContext::forComponent(doc, topology, a);
    CHECK(near.contains(a));
    CHECK(near.contains(b));
    CHECK_FALSE(near.contains(far));

    const SolveContext missing = SolveContext::forComponent(doc, topology, EntityId(999));
    CHECK(missing.empty());
}

TEST_CASE("forComponents unions the components a candidate would merge") {
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 0.0);
    addSegment(doc, a, b);
    const EntityId c = addPoint(doc, 100.0, 0.0);
    const EntityId d = addPoint(doc, 110.0, 0.0);
    addSegment(doc, c, d);

    Topology topology(doc);
    REQUIRE(topology.componentCount() == 2);

    const SolveContext both = SolveContext::forComponents(doc, topology, {a, c});
    CHECK(both.contains(a));
    CHECK(both.contains(c));
    CHECK(both.members().size() == 6);

    // Naming the same component twice must not duplicate its members.
    const SolveContext once = SolveContext::forComponents(doc, topology, {a, b});
    CHECK(once.members().size() == 3);
}

TEST_CASE("an empty context solves trivially rather than failing") {
    Document doc;
    SolveContext context;
    const SolveOutcome outcome = solve(doc, context);
    CHECK(outcome.ok());
    CHECK(outcome.dof == 0);
}

TEST_CASE("the outcome carries a measured time and the generation it was asked for") {
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 3.0);
    addConstraint(doc, ConstraintKind::PointPointDistance, {a, b}, Slot(20.0));

    SolveOptions options;
    options.generation = 7;
    SolveContext context = SolveContext::forWholeDocument(doc);
    const SolveOutcome outcome = solve(doc, context, options);

    REQUIRE(outcome.ok());
    // Stale results are dropped by comparing this, never by guessing.
    CHECK(outcome.generation == 7);
    CHECK(outcome.microseconds > 0.0);
    CHECK(outcome.arenaBytes > 0);
}

TEST_CASE("skipping failure diagnosis still solves") {
    // Computing the blamed set costs roughly another solve, so the interactive
    // path opts out when only the pose matters.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 7.0);
    const EntityId segment = addSegment(doc, a, b);
    addConstraint(doc, ConstraintKind::Pin, {a});
    addConstraint(doc, ConstraintKind::Horizontal, {segment});

    SolveOptions options;
    options.diagnoseFailures = false;
    SolveContext context = SolveContext::forWholeDocument(doc);
    const SolveOutcome outcome = solve(doc, context, options);

    REQUIRE(outcome.ok());
    CHECK(context.point(b)->y == doctest::Approx(0.0));
}

TEST_CASE("a referenced horizontal is parallelism to the axis it names") {
    // The whole point of recording horizontal as axis-referenced parallelism:
    // with a reference the relation follows that axis rather than the document
    // frame, which is what makes rotate-a-subset answerable at all. Same
    // declaration, different solver primitive, chosen from the taxonomy.
    Document doc;
    // The reference axis, pinned so it cannot rotate to meet the subject.
    const EntityId r0 = addPoint(doc, 0.0, 0.0);
    const EntityId r1 = addPoint(doc, 100.0, 60.0);
    const EntityId axis = addSegment(doc, r0, r1);
    addConstraint(doc, ConstraintKind::Pin, {r0});
    addConstraint(doc, ConstraintKind::Pin, {r1});

    // The subject, drawn nowhere near parallel to it.
    const EntityId s0 = addPoint(doc, 0.0, 200.0);
    const EntityId s1 = addPoint(doc, 90.0, 190.0);
    const EntityId subject = addSegment(doc, s0, s1);
    addConstraint(doc, ConstraintKind::Pin, {s0});

    REQUIRE(addConstraint(doc, ConstraintKind::Horizontal, {subject, axis}).valid());

    SolveContext context = SolveContext::forWholeDocument(doc);
    const SolveOutcome outcome = solve(doc, context, SolveOptions());
    REQUIRE(outcome.ok());

    const Point a = *context.point(s0);
    const Point b = *context.point(s1);
    // Parallel to (100, 60), which is emphatically not horizontal.
    const double cross = (b.x - a.x) * 60.0 - (b.y - a.y) * 100.0;
    CHECK(cross == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(std::abs(b.y - a.y) > 1.0);
}

TEST_CASE("a referenced vertical is perpendicularity to the axis it names") {
    Document doc;
    const EntityId r0 = addPoint(doc, 0.0, 0.0);
    const EntityId r1 = addPoint(doc, 100.0, 60.0);
    const EntityId axis = addSegment(doc, r0, r1);
    addConstraint(doc, ConstraintKind::Pin, {r0});
    addConstraint(doc, ConstraintKind::Pin, {r1});

    const EntityId s0 = addPoint(doc, 0.0, 200.0);
    const EntityId s1 = addPoint(doc, 10.0, 280.0);
    const EntityId subject = addSegment(doc, s0, s1);
    addConstraint(doc, ConstraintKind::Pin, {s0});

    REQUIRE(addConstraint(doc, ConstraintKind::Vertical, {subject, axis}).valid());

    SolveContext context = SolveContext::forWholeDocument(doc);
    const SolveOutcome outcome = solve(doc, context, SolveOptions());
    REQUIRE(outcome.ok());

    const Point a = *context.point(s0);
    const Point b = *context.point(s1);
    // Perpendicular to (100, 60): the dot product goes to zero, not the cross.
    const double dot = (b.x - a.x) * 100.0 + (b.y - a.y) * 60.0;
    CHECK(dot == doctest::Approx(0.0).epsilon(1e-9));
}

TEST_CASE("an unreferenced horizontal still means the document frame") {
    // The default has to be exactly what it always was, or every corpus entry
    // and every saved file changes meaning under a format that gained a slot.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 90.0, 40.0);
    const EntityId segment = addSegment(doc, a, b);
    addConstraint(doc, ConstraintKind::Pin, {a});
    REQUIRE(addConstraint(doc, ConstraintKind::Horizontal, {segment}).valid());

    SolveContext context = SolveContext::forWholeDocument(doc);
    REQUIRE(solve(doc, context, SolveOptions()).ok());
    CHECK(context.point(b)->y == doctest::Approx(0.0));
}

TEST_CASE("a referenced horizontal solves inside its own component") {
    // The reference joins the component: the subject and the axis have to be
    // solved together or the relation binds two systems that never meet.
    Document doc;
    const EntityId r0 = addPoint(doc, 0.0, 0.0);
    const EntityId r1 = addPoint(doc, 100.0, 60.0);
    const EntityId axis = addSegment(doc, r0, r1);
    const EntityId s0 = addPoint(doc, 0.0, 200.0);
    const EntityId s1 = addPoint(doc, 90.0, 190.0);
    const EntityId subject = addSegment(doc, s0, s1);
    REQUIRE(addConstraint(doc, ConstraintKind::Horizontal, {subject, axis}).valid());

    Topology topology(doc);
    const SolveContext context = SolveContext::forComponent(doc, topology, subject);
    CHECK(context.contains(axis));
    CHECK(context.contains(r0));
    CHECK(context.contains(r1));
}

// A quarter arc centred on the origin, running from (50,0) counter-clockwise to
// (0,50), with every one of its points pinned so the arc itself cannot move.
// A segment hangs off the start point with its far end free, which is what the
// tangency has to steer.
namespace {

struct TangentBench {
    Document doc;
    EntityId centre, arcStart, arcEnd, arc, lineFar, segment;

    TangentBench() {
        centre = addPoint(doc, 0.0, 0.0);
        arcStart = addPoint(doc, 50.0, 0.0);
        arcEnd = addPoint(doc, 0.0, 50.0);

        EntityRecord a;
        a.kind = EntityKind::Arc;
        a.points = {centre, arcStart, arcEnd};
        arc = EntityId(doc.apply(AddRecord<EntityRecord>{a}).allocated);

        addConstraint(doc, ConstraintKind::Pin, {centre});
        addConstraint(doc, ConstraintKind::Pin, {arcStart});
        addConstraint(doc, ConstraintKind::Pin, {arcEnd});

        // Drawn at a slant, so the solve has to move it either way.
        const EntityId near = addPoint(doc, 50.0, 0.0);
        lineFar = addPoint(doc, 80.0, 20.0);
        segment = addSegment(doc, near, lineFar);
        addConstraint(doc, ConstraintKind::Coincident, {near, arcStart});
    }

    // The direction the segment settled on, as a unit vector.
    Eigen::Vector2d directionAfterSolve(uint8_t alternative) {
        ConstraintRecord t;
        t.kind = ConstraintKind::Tangent;
        t.operands[0] = arc;
        t.operands[1] = segment;
        t.alternative = alternative;
        REQUIRE(doc.apply(AddRecord<ConstraintRecord>{t}).ok());

        SolveContext context = SolveContext::forWholeDocument(doc);
        const SolveOutcome outcome = solve(doc, context, SolveOptions());
        REQUIRE(outcome.ok());

        const Point a = *context.point(doc.entities().find(segment)->points[0]);
        const Point b = *context.point(lineFar);
        Eigen::Vector2d d(b.x - a.x, b.y - a.y);
        return d.normalized();
    }
};

}  // namespace

TEST_CASE("tangency says which end of the arc it holds at") {
    // SLVS_C_ARC_LINE_TANGENT picks the arc end from Slvs_Constraint.other, and
    // zero-filling it made every tangent a tangent at the start — the other
    // form was not merely unused, it was unrepresentable. Stage 5 makes the
    // whole catalogue imposable, so it had to become sayable first.
    SUBCASE("at the start, the tangent is perpendicular to the start radius") {
        TangentBench bench;
        const Eigen::Vector2d d = bench.directionAfterSolve(0);
        // The start radius runs along +x, so its tangent is vertical.
        CHECK(std::abs(d.x()) == doctest::Approx(0.0).epsilon(1e-6));
        CHECK(std::abs(d.y()) == doctest::Approx(1.0).epsilon(1e-6));
    }

    SUBCASE("at the end, it is perpendicular to the end radius instead") {
        TangentBench bench;
        const Eigen::Vector2d d = bench.directionAfterSolve(1);
        // The end radius runs along +y, so its tangent is horizontal.
        CHECK(std::abs(d.x()) == doctest::Approx(1.0).epsilon(1e-6));
        CHECK(std::abs(d.y()) == doctest::Approx(0.0).epsilon(1e-6));
    }
}
