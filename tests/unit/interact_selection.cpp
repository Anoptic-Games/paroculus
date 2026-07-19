#include <doctest/doctest.h>

#include "interact/selection.h"
#include "support/build.h"

using namespace paroculus;
using paroculus::test::addConstraint;
using paroculus::test::addPoint;
using paroculus::test::addSegment;

namespace {

struct TwoSegments {
    Document doc;
    EntityId a, b, c, d, first, second;
};

// Two segments joined end to end by a coincidence: a connected run.
TwoSegments buildRun() {
    TwoSegments f;
    f.a = addPoint(f.doc, 0.0, 0.0);
    f.b = addPoint(f.doc, 10.0, 0.0);
    f.c = addPoint(f.doc, 10.0, 0.0);
    f.d = addPoint(f.doc, 20.0, 0.0);
    f.first = addSegment(f.doc, f.a, f.b);
    f.second = addSegment(f.doc, f.c, f.d);
    addConstraint(f.doc, ConstraintKind::Coincident, {f.b, f.c});
    return f;
}

}  // namespace

TEST_CASE("selection contents stay id-ordered and free of duplicates") {
    // Two selections built by different routes must compare and serialize alike.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 1.0, 0.0);
    const EntityId c = addPoint(doc, 2.0, 0.0);

    Selection selection;
    selection.add(c);
    selection.add(a);
    selection.add(b);
    selection.add(a);  // again

    REQUIRE(selection.size() == 3);
    CHECK(selection.items()[0] == a);
    CHECK(selection.items()[1] == b);
    CHECK(selection.items()[2] == c);
}

TEST_CASE("toggle is in when out and out when in") {
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);

    Selection selection;
    selection.toggle(a);
    CHECK(selection.contains(a));
    selection.toggle(a);
    CHECK_FALSE(selection.contains(a));
    // A null id is not selectable.
    selection.toggle(EntityId());
    CHECK(selection.empty());
}

TEST_CASE("a connected run follows shared geometry, not constraints") {
    // Two segments held parallel are related but are not one shape. The
    // constraint graph is what the solver groups by; the coincidence graph is
    // what the user sees as joined.
    TwoSegments f = buildRun();
    Topology topology(f.doc);

    const std::vector<EntityId> run = connectedRun(f.doc, topology, f.first);
    CHECK(std::find(run.begin(), run.end(), f.second) != run.end());

    Document separate;
    const EntityId p = addPoint(separate, 0.0, 0.0);
    const EntityId q = addPoint(separate, 10.0, 0.0);
    const EntityId r = addPoint(separate, 0.0, 5.0);
    const EntityId s = addPoint(separate, 10.0, 5.0);
    const EntityId one = addSegment(separate, p, q);
    const EntityId two = addSegment(separate, r, s);
    addConstraint(separate, ConstraintKind::Parallel, {one, two});
    Topology separateTopology(separate);

    const std::vector<EntityId> parallelRun = connectedRun(separate, separateTopology, one);
    CHECK(std::find(parallelRun.begin(), parallelRun.end(), two) == parallelRun.end());
}

TEST_CASE("descent goes to the parts and ascent comes back") {
    TwoSegments f = buildRun();
    Topology topology(f.doc);

    Selection selection;
    selection.set(connectedRun(f.doc, topology, f.first));
    const size_t whole = selection.size();
    CHECK(selection.depth() == 0);

    REQUIRE(selection.descend(f.doc, topology));
    CHECK(selection.depth() == 1);
    // Descending a run of segments leaves the points that define them.
    CHECK(selection.contains(f.a));
    CHECK(selection.contains(f.b));
    CHECK_FALSE(selection.contains(f.first));

    REQUIRE(selection.ascend(f.doc, topology));
    CHECK(selection.depth() == 0);
    CHECK(selection.contains(f.first));
    CHECK(selection.size() == whole);
}

TEST_CASE("ascent refuses at the home state so Esc can clear instead") {
    TwoSegments f = buildRun();
    Topology topology(f.doc);

    Selection selection;
    selection.set(f.first);
    CHECK_FALSE(selection.ascend(f.doc, topology));
}

TEST_CASE("descent bottoms out rather than looping") {
    TwoSegments f = buildRun();
    Topology topology(f.doc);

    Selection selection;
    selection.set(f.first);
    REQUIRE(selection.descend(f.doc, topology));
    // Points have no parts, so there is nothing further to descend into.
    CHECK_FALSE(selection.descend(f.doc, topology));
}

TEST_CASE("mixed-depth selections are legal and produce honest signatures") {
    // {this shape, that one vertex} is a thing a user can express, so it has to
    // be a thing the signature can describe.
    TwoSegments f = buildRun();

    Selection selection;
    selection.add(f.first);
    selection.add(f.a);

    const Signature signature = selection.signature(f.doc);
    CHECK(signature.size() == 2);
    CHECK(signature.describe() == "{point, segment}");
}

TEST_CASE("the signature is the sorted typed multiset") {
    // Canonical because the surface asks what can apply to a set, not to a
    // click order. Two segments picked either way round are one question.
    TwoSegments f = buildRun();

    Selection forward;
    forward.add(f.first);
    forward.add(f.second);

    Selection backward;
    backward.add(f.second);
    backward.add(f.first);

    CHECK(forward.signature(f.doc) == backward.signature(f.doc));
    CHECK(forward.signature(f.doc).describe() == "{segment, segment}");

    Selection empty;
    CHECK(empty.signature(f.doc).empty());
    CHECK(empty.signature(f.doc).describe() == "{}");
}

TEST_CASE("a signature counts duplicates rather than collapsing them") {
    TwoSegments f = buildRun();
    Selection selection;
    selection.add(f.a);
    selection.add(f.b);
    // Two points is a different question from one point.
    CHECK(selection.signature(f.doc).describe() == "{point, point}");
}
