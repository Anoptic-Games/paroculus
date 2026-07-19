#include <doctest/doctest.h>

#include "core/pose.h"
#include "support/build.h"

using namespace paroculus;
using paroculus::test::addPoint;
using paroculus::test::addSegment;

TEST_CASE("a pose reads the document's committed seeds") {
    Document doc;
    const EntityId a = addPoint(doc, 1.0, 2.0);
    const EntityId b = addPoint(doc, 5.0, 6.0);
    const EntityId segment = addSegment(doc, a, b);

    const Pose pose(doc);
    CHECK(pose.point(a)->x == 1.0);
    CHECK(pose.point(b)->y == 6.0);

    const auto ends = pose.segment(segment);
    REQUIRE(ends.has_value());
    CHECK(ends->first.x == 1.0);
    CHECK(ends->second.y == 6.0);

    // A segment owns no position of its own.
    CHECK_FALSE(pose.point(segment).has_value());
    CHECK_FALSE(pose.point(EntityId(999)).has_value());
}

TEST_CASE("an overlay supersedes the committed values") {
    // A drag in flight has newer values that have not been committed and may
    // never be. Rendering and hit testing must both see them, or the user
    // picks one thing and selects another.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 0.0);
    const EntityId segment = addSegment(doc, a, b);

    Pose pose(doc);
    SeedSpan moved;
    moved.entity = b;
    moved.seeds = {40.0, 25.0};
    pose.overlay(std::vector<SeedSpan>{moved});

    CHECK(pose.point(b)->x == 40.0);
    CHECK(pose.point(b)->y == 25.0);
    // Untouched entities keep their committed values.
    CHECK(pose.point(a)->x == 0.0);
    // And derived geometry follows the overlay.
    CHECK(pose.segment(segment)->second.x == 40.0);
    // The document itself is untouched.
    CHECK(doc.entities().find(b)->seeds[0] == 10.0);
}

TEST_CASE("clearing the overlay returns to the committed pose") {
    Document doc;
    const EntityId a = addPoint(doc, 3.0, 4.0);

    Pose pose(doc);
    SeedSpan moved;
    moved.entity = a;
    moved.seeds = {99.0, 99.0};
    pose.overlay(std::vector<SeedSpan>{moved});
    REQUIRE(pose.point(a)->x == 99.0);

    pose.overlay({});
    CHECK(pose.point(a)->x == 3.0);
}

TEST_CASE("a circle's radius comes from the pose, not from a constraint") {
    Document doc;
    const EntityId centre = addPoint(doc, 0.0, 0.0);
    EntityRecord circle;
    circle.kind = EntityKind::Circle;
    circle.points = {centre, EntityId(), EntityId()};
    circle.seeds = {7.5, 0.0};
    const EntityId id(doc.apply(AddRecord<EntityRecord>{circle}).allocated);

    const Pose pose(doc);
    CHECK(pose.radius(id) == doctest::Approx(7.5));
    CHECK_FALSE(pose.radius(centre).has_value());
}
