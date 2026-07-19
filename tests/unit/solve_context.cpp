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
