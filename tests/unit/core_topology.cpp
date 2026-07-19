#include <doctest/doctest.h>

#include "core/topology.h"
#include "support/build.h"

using namespace paroculus;
using paroculus::test::Rng;
using paroculus::test::addConstraint;
using paroculus::test::addPoint;
using paroculus::test::addSegment;

namespace {

// Four segments whose corners are coincident: the flagship outline.
struct Quad {
    std::array<EntityId, 8> corners{};  // two points per joint, pre-coincidence
    std::array<EntityId, 4> edges{};
};

Quad buildQuad(Document &doc, bool close) {
    Quad q;
    const double xs[4] = {0.0, 10.0, 10.0, 0.0};
    const double ys[4] = {0.0, 0.0, 10.0, 10.0};
    for(int i = 0; i < 4; i++) {
        const int j = (i + 1) % 4;
        q.corners[i * 2] = addPoint(doc, xs[i], ys[i]);
        q.corners[i * 2 + 1] = addPoint(doc, xs[j], ys[j]);
        q.edges[i] = addSegment(doc, q.corners[i * 2], q.corners[i * 2 + 1]);
    }
    if(close) {
        // Join each edge's end to the next edge's start.
        for(int i = 0; i < 4; i++) {
            const int j = (i + 1) % 4;
            addConstraint(doc, ConstraintKind::Coincident,
                          {q.corners[i * 2 + 1], q.corners[j * 2]});
        }
    }
    return q;
}

}  // namespace

TEST_CASE("a segment always shares a component with its endpoints") {
    // Without ownership edges an unconstrained segment would partition away
    // from the very points that define it.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 1.0, 0.0);
    const EntityId seg = addSegment(doc, a, b);

    Topology t(doc);
    CHECK(t.sameComponent(seg, a));
    CHECK(t.sameComponent(seg, b));
    CHECK(t.componentCount() == 1);
}

TEST_CASE("unconnected geometry partitions apart") {
    // This is what makes a drag local: geometry that is not connected cannot
    // move, so a solve is scoped to one component.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 1.0, 0.0);
    addSegment(doc, a, b);
    const EntityId far = addPoint(doc, 500.0, 500.0);

    Topology t(doc);
    CHECK(t.componentCount() == 2);
    CHECK_FALSE(t.sameComponent(a, far));
}

TEST_CASE("a constraint merges the components of its operands") {
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 1.0, 0.0);
    const EntityId segA = addSegment(doc, a, b);
    const EntityId c = addPoint(doc, 0.0, 5.0);
    const EntityId d = addPoint(doc, 1.0, 5.0);
    const EntityId segB = addSegment(doc, c, d);

    Topology t(doc);
    REQUIRE(t.componentCount() == 2);

    const ConstraintId par = addConstraint(doc, ConstraintKind::Parallel, {segA, segB});
    REQUIRE(par.valid());
    t.markDirty();
    CHECK(t.componentCount() == 1);
    CHECK(t.sameComponent(a, d));
}

TEST_CASE("coincidence is a narrower relation than component membership") {
    // A parallel constraint puts two segments in one component without making
    // any of their endpoints the same vertex.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 1.0, 0.0);
    const EntityId segA = addSegment(doc, a, b);
    const EntityId c = addPoint(doc, 0.0, 5.0);
    const EntityId d = addPoint(doc, 1.0, 5.0);
    const EntityId segB = addSegment(doc, c, d);
    addConstraint(doc, ConstraintKind::Parallel, {segA, segB});

    Topology t(doc);
    CHECK(t.sameComponent(a, c));
    CHECK_FALSE(t.coincident(a, c));

    addConstraint(doc, ConstraintKind::Coincident, {b, c});
    t.markDirty();
    CHECK(t.coincident(b, c));
    CHECK(t.coincidentRun(b).size() == 2);
}

TEST_CASE("closure is topological, not visual") {
    // Endpoints that merely look close are an open outline. This is exactly the
    // gap heal-and-fill has to bridge by imposing coincidences.
    Document doc;
    const Quad open = buildQuad(doc, false);
    Topology t(doc);
    CHECK_FALSE(findBoundaryCycle(doc, t, open.edges).has_value());

    Document closedDoc;
    const Quad closed = buildQuad(closedDoc, true);
    Topology ct(closedDoc);
    const auto cycle = findBoundaryCycle(closedDoc, ct, closed.edges);
    REQUIRE(cycle.has_value());
    CHECK(cycle->size() == 4);
}

TEST_CASE("the reported cycle is a permutation of its edges") {
    Document doc;
    const Quad q = buildQuad(doc, true);
    Topology t(doc);

    const auto cycle = findBoundaryCycle(doc, t, q.edges);
    REQUIRE(cycle.has_value());
    std::vector<EntityId> sorted = *cycle;
    std::sort(sorted.begin(), sorted.end());
    std::vector<EntityId> expected(q.edges.begin(), q.edges.end());
    std::sort(expected.begin(), expected.end());
    CHECK(sorted == expected);
}

TEST_CASE("cycle detection does not depend on the caller's argument order") {
    // Determinism again: the boundary a region records must be a function of
    // the document, not of how the selection happened to be ordered.
    Document doc;
    const Quad q = buildQuad(doc, true);
    Topology t(doc);

    std::vector<EntityId> forward(q.edges.begin(), q.edges.end());
    std::vector<EntityId> reversed(q.edges.rbegin(), q.edges.rend());
    const auto a = findBoundaryCycle(doc, t, forward);
    const auto b = findBoundaryCycle(doc, t, reversed);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(*a == *b);
}

TEST_CASE("an open run is not a cycle") {
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 1.0, 0.0);
    const EntityId c = addPoint(doc, 1.0, 0.0);
    const EntityId d = addPoint(doc, 2.0, 0.0);
    const EntityId s1 = addSegment(doc, a, b);
    const EntityId s2 = addSegment(doc, c, d);
    addConstraint(doc, ConstraintKind::Coincident, {b, c});

    Topology t(doc);
    const std::array<EntityId, 2> edges = {s1, s2};
    CHECK_FALSE(findBoundaryCycle(doc, t, edges).has_value());
}

TEST_CASE("two disjoint loops are not one cycle") {
    // The degree test alone would accept this; connectivity is what rejects it.
    Document doc;
    const Quad first = buildQuad(doc, true);
    const Quad second = buildQuad(doc, true);

    std::vector<EntityId> both(first.edges.begin(), first.edges.end());
    both.insert(both.end(), second.edges.begin(), second.edges.end());

    Topology t(doc);
    CHECK_FALSE(findBoundaryCycle(doc, t, both).has_value());
    // Each on its own is still a loop.
    CHECK(findBoundaryCycle(doc, t, first.edges).has_value());
    CHECK(findBoundaryCycle(doc, t, second.edges).has_value());
}

TEST_CASE("a vertex touched three times is not a cycle") {
    Document doc;
    const Quad q = buildQuad(doc, true);
    // A spur off one corner.
    const EntityId tip = addPoint(doc, -5.0, 0.0);
    const EntityId spurRoot = addPoint(doc, 0.0, 0.0);
    const EntityId spur = addSegment(doc, spurRoot, tip);
    addConstraint(doc, ConstraintKind::Coincident, {spurRoot, q.corners[0]});

    std::vector<EntityId> edges(q.edges.begin(), q.edges.end());
    edges.push_back(spur);

    Topology t(doc);
    CHECK_FALSE(findBoundaryCycle(doc, t, edges).has_value());
}

TEST_CASE("points and missing entities cannot bound an area") {
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 1.0, 0.0);
    const EntityId seg = addSegment(doc, a, b);

    Topology t(doc);
    const std::array<EntityId, 2> withPoint = {seg, a};
    CHECK_FALSE(findBoundaryCycle(doc, t, withPoint).has_value());
    const std::array<EntityId, 2> withGhost = {seg, EntityId(999)};
    CHECK_FALSE(findBoundaryCycle(doc, t, withGhost).has_value());
}

TEST_CASE("property: a marked partition equals a rebuild after every step") {
    // A Topology is a derived index and the document is the authority, so a
    // topology told that something changed has to answer exactly as one built
    // from scratch would — after additions, after removals, and at every point
    // in between rather than only at the end.
    //
    // Driven through markDirty because that is what the product calls. The
    // incremental union-in-place path this once exercised was unreachable, and
    // a property proved on code nothing runs is not coverage.
    for(uint64_t seed = 1; seed <= 30; seed++) {
        Rng rng(seed * 6151u);
        Document doc;
        Topology incremental(doc);

        std::vector<EntityId> points, segments;

        for(int step = 0; step < 40; step++) {
            const uint32_t choice = rng.below(10);

            if(choice < 4 || points.size() < 2) {
                const EntityId p = addPoint(doc, rng.real(-50, 50), rng.real(-50, 50));
                if(p.valid()) {
                    points.push_back(p);
                    incremental.markDirty();
                }
            } else if(choice < 6) {
                const EntityId a = points[rng.below(points.size())];
                const EntityId b = points[rng.below(points.size())];
                if(a != b) {
                    const EntityId s = addSegment(doc, a, b);
                    if(s.valid()) {
                        segments.push_back(s);
                        incremental.markDirty();
                    }
                }
            } else if(choice < 8) {
                const EntityId a = points[rng.below(points.size())];
                const EntityId b = points[rng.below(points.size())];
                if(a != b) {
                    const ConstraintId c =
                        addConstraint(doc, ConstraintKind::Coincident, {a, b});
                    if(c.valid()) incremental.markDirty();
                }
            } else if(choice < 9 && segments.size() >= 2) {
                const EntityId a = segments[rng.below(segments.size())];
                const EntityId b = segments[rng.below(segments.size())];
                if(a != b) {
                    const ConstraintId c = addConstraint(doc, ConstraintKind::Parallel, {a, b});
                    if(c.valid()) incremental.markDirty();
                }
            } else if(!doc.constraints().empty()) {
                // A removal, which the union-find cannot absorb.
                const auto &all = doc.constraints().records();
                const ConstraintId victim = all[rng.below(all.size())].id;
                REQUIRE(doc.apply(RemoveRecord<ConstraintRecord>{victim}).ok());
                incremental.noteRemoved();
            }

            // A freshly built Topology is the from-scratch answer by
            // construction; the maintained one must agree.
            const Topology scratch(doc);
            REQUIRE_MESSAGE(incremental.componentCount() == scratch.componentCount(),
                            "seed ", seed, " step ", step);
            for(const EntityRecord &e : doc.entities().records()) {
                for(const EntityRecord &f : doc.entities().records()) {
                    REQUIRE_MESSAGE(
                        incremental.sameComponent(e.id, f.id) ==
                            scratch.sameComponent(e.id, f.id),
                        "seed ", seed, " step ", step);
                }
                REQUIRE(incremental.coincidentRun(e.id) == scratch.coincidentRun(e.id));
            }
        }
    }
}
