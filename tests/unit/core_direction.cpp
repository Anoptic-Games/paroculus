// Direction classes: the declared-parallelism closure over segments.
//
// The count reads the declarations, not the pixels — two segments that only look
// parallel count twice — so these cases pin exactly which relations close a class
// and which do not, and that the output order is deterministic.
#include <doctest/doctest.h>

#include "core/direction.h"
#include "support/build.h"

using namespace paroculus;

namespace {

// A segment from (x0,y0) to (x1,y1), returning its id.
EntityId seg(Document &doc, double x0, double y0, double x1, double y1) {
    const EntityId a = test::addPoint(doc, x0, y0);
    const EntityId b = test::addPoint(doc, x1, y1);
    return test::addSegment(doc, a, b);
}

}  // namespace

TEST_CASE("two unrelated segments are two classes") {
    Document doc;
    seg(doc, 0.0, 0.0, 10.0, 0.0);
    seg(doc, 0.0, 5.0, 10.0, 7.0);
    const DirectionClasses classes = directionClasses(doc);
    CHECK(classes.count() == 2);
}

TEST_CASE("segments that merely look parallel still count twice") {
    // The diagnostic: no declaration, so no shared class, however parallel they
    // are drawn.
    Document doc;
    seg(doc, 0.0, 0.0, 10.0, 0.0);
    seg(doc, 0.0, 5.0, 10.0, 5.0);
    CHECK(directionClasses(doc).count() == 2);
}

TEST_CASE("a parallel constraint unites two segments into one class") {
    Document doc;
    const EntityId a = seg(doc, 0.0, 0.0, 10.0, 0.0);
    const EntityId b = seg(doc, 0.0, 5.0, 10.0, 7.0);
    test::addConstraint(doc, ConstraintKind::Parallel, {a, b});
    const DirectionClasses classes = directionClasses(doc);
    CHECK(classes.count() == 1);
    CHECK(classes.classOf.at(a) == classes.classOf.at(b));
}

TEST_CASE("null-reference horizontals share one class, verticals another") {
    Document doc;
    const EntityId h1 = seg(doc, 0.0, 0.0, 10.0, 0.0);
    const EntityId h2 = seg(doc, 0.0, 5.0, 10.0, 5.0);
    const EntityId v1 = seg(doc, 0.0, 0.0, 0.0, 10.0);
    test::addConstraint(doc, ConstraintKind::Horizontal, {h1});
    test::addConstraint(doc, ConstraintKind::Horizontal, {h2});
    test::addConstraint(doc, ConstraintKind::Vertical, {v1});
    const DirectionClasses classes = directionClasses(doc);
    // Two horizontals in one class, one vertical in another: two classes, and the
    // vertical is not folded into the horizontal one — perpendicular is a
    // different direction.
    CHECK(classes.count() == 2);
    CHECK(classes.classOf.at(h1) == classes.classOf.at(h2));
    CHECK(classes.classOf.at(h1) != classes.classOf.at(v1));
}

TEST_CASE("a horizontal with a named reference joins the reference's class") {
    Document doc;
    const EntityId ref = seg(doc, 0.0, 0.0, 10.0, 3.0);
    const EntityId s = seg(doc, 0.0, 20.0, 10.0, 20.0);
    // Horizontal(s) about ref means s is parallel to ref.
    test::addConstraint(doc, ConstraintKind::Horizontal, {s, ref});
    const DirectionClasses classes = directionClasses(doc);
    CHECK(classes.count() == 1);
    CHECK(classes.classOf.at(s) == classes.classOf.at(ref));
}

TEST_CASE("a vertical with a named reference does not join the reference's class") {
    Document doc;
    const EntityId ref = seg(doc, 0.0, 0.0, 10.0, 3.0);
    const EntityId s = seg(doc, 0.0, 20.0, 3.0, 30.0);
    // Vertical(s) about ref means s is perpendicular to ref — a different
    // direction, so a different class.
    test::addConstraint(doc, ConstraintKind::Vertical, {s, ref});
    const DirectionClasses classes = directionClasses(doc);
    CHECK(classes.count() == 2);
    CHECK(classes.classOf.at(s) != classes.classOf.at(ref));
}

TEST_CASE("two verticals about the same reference share a class") {
    Document doc;
    const EntityId ref = seg(doc, 0.0, 0.0, 10.0, 3.0);
    const EntityId s1 = seg(doc, 0.0, 20.0, 3.0, 30.0);
    const EntityId s2 = seg(doc, 5.0, 20.0, 8.0, 30.0);
    test::addConstraint(doc, ConstraintKind::Vertical, {s1, ref});
    test::addConstraint(doc, ConstraintKind::Vertical, {s2, ref});
    const DirectionClasses classes = directionClasses(doc);
    // ref, and the two verticals perpendicular to it: two classes.
    CHECK(classes.count() == 2);
    CHECK(classes.classOf.at(s1) == classes.classOf.at(s2));
    CHECK(classes.classOf.at(s1) != classes.classOf.at(ref));
}

TEST_CASE("the output order is deterministic and members are id-ordered") {
    Document doc;
    const EntityId a = seg(doc, 0.0, 0.0, 10.0, 0.0);
    const EntityId b = seg(doc, 0.0, 5.0, 10.0, 7.0);
    test::addConstraint(doc, ConstraintKind::Parallel, {a, b});
    seg(doc, 100.0, 0.0, 100.0, 10.0);  // an unrelated third

    const DirectionClasses first = directionClasses(doc);
    const DirectionClasses second = directionClasses(doc);
    REQUIRE(first.members.size() == second.members.size());
    for(size_t i = 0; i < first.members.size(); i++) {
        CHECK(first.members[i] == second.members[i]);
    }
    // Classes are ordered by smallest member; a's class comes first and holds
    // both a and b in id order.
    REQUIRE(first.members.size() == 2);
    CHECK(first.members[0].size() == 2);
    CHECK(first.members[0][0] == a);
    CHECK(first.members[0][1] == b);
    CHECK(first.classMembers(b).size() == 2);
    CHECK(first.classMembers(EntityId()).empty());
}
