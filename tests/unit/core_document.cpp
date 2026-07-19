#include <doctest/doctest.h>

#include "core/document.h"
#include "support/build.h"

using namespace paroculus;
using paroculus::test::addConstraint;
using paroculus::test::addPoint;
using paroculus::test::addSegment;

TEST_CASE("geometry is born free and accretes constraints") {
    // Everything a user draws starts fully free; constraints are opportunistic
    // and most documents ship under-constrained. Nothing here requires them.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 0.0);
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    CHECK(doc.entities().size() == 2);
    CHECK(doc.constraints().empty());

    const EntityId seg = addSegment(doc, a, b);
    REQUIRE(seg.valid());
    CHECK(addConstraint(doc, ConstraintKind::Horizontal, {seg}).valid());
}

TEST_CASE("a constraint whose signature the taxonomy rejects is refused") {
    // The model and the action surface read the same table, so an action the
    // model refuses is an action no surface can offer.
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 1.0, 1.0);
    const EntityId seg = addSegment(doc, p, q);

    ConstraintRecord bad;
    bad.kind = ConstraintKind::Parallel;  // wants {segment, segment}
    bad.operands = {p, q, EntityId(), EntityId()};
    const CommandResult r = doc.apply(AddRecord<ConstraintRecord>{bad});
    CHECK(r.error == CommandError::WrongSignature);
    CHECK(doc.constraints().empty());

    // Order is significant: point-on-line is {point, segment}, not the reverse.
    CHECK_FALSE(addConstraint(doc, ConstraintKind::PointOnLine, {seg, p}).valid());
    CHECK(addConstraint(doc, ConstraintKind::PointOnLine, {p, seg}).valid());
}

TEST_CASE("a constraint naming a missing operand is refused") {
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    CHECK_FALSE(addConstraint(doc, ConstraintKind::Coincident, {p, EntityId(404)}).valid());
    CHECK(doc.constraints().empty());
}

TEST_CASE("operand slots past the kind's arity must be empty") {
    // Two records that mean the same thing have to compare equal and serialize
    // identically, so trailing junk is a refusal rather than an ignored field.
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 1.0, 0.0);

    ConstraintRecord r;
    r.kind = ConstraintKind::Coincident;
    r.operands = {p, q, p, EntityId()};
    CHECK(doc.apply(AddRecord<ConstraintRecord>{r}).error == CommandError::WrongSignature);
}

TEST_CASE("a valueless constraint may not carry a value") {
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 1.0, 0.0);
    CHECK_FALSE(addConstraint(doc, ConstraintKind::Coincident, {p, q}, Slot(5.0)).valid());
    CHECK(addConstraint(doc, ConstraintKind::PointPointDistance, {p, q}, Slot(5.0)).valid());
}

TEST_CASE("an entity whose defining point is not a point is refused") {
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 1.0, 0.0);
    const EntityId seg = addSegment(doc, p, q);

    // A segment whose endpoint is another segment is not degraded, it is
    // nonsense.
    CHECK_FALSE(addSegment(doc, seg, q).valid());
    CHECK_FALSE(addSegment(doc, p, EntityId(999)).valid());
}

TEST_CASE("a refused command leaves the document untouched") {
    Document doc;
    addPoint(doc, 1.0, 2.0);
    const Document before = doc;

    ConstraintRecord bad;
    bad.kind = ConstraintKind::Parallel;
    bad.operands = {EntityId(50), EntityId(51), EntityId(), EntityId()};
    CHECK_FALSE(doc.apply(AddRecord<ConstraintRecord>{bad}).ok());
    CHECK(doc == before);
}

TEST_CASE("every command shape inverts exactly") {
    Document doc;
    const EntityId p = addPoint(doc, 3.0, 4.0);
    const Document afterAdd = doc;

    SUBCASE("set inverts to the old record") {
        EntityRecord moved = *doc.entities().find(p);
        moved.seeds = {99.0, -1.0};
        const Command set = SetRecord<EntityRecord>{moved};

        const auto inverse = doc.invert(set);
        REQUIRE(inverse.has_value());
        REQUIRE(doc.apply(set).ok());
        CHECK(doc.entities().find(p)->seeds[0] == 99.0);
        REQUIRE(doc.apply(*inverse).ok());
        CHECK(doc == afterAdd);
    }

    SUBCASE("remove inverts to an add at the same id") {
        const Command remove = RemoveRecord<EntityRecord>{p};
        const auto inverse = doc.invert(remove);
        REQUIRE(inverse.has_value());
        REQUIRE(doc.apply(remove).ok());
        CHECK(doc.entities().empty());
        REQUIRE(doc.apply(*inverse).ok());
        CHECK(doc == afterAdd);
    }

    SUBCASE("add inverts to a remove of the id it will take") {
        EntityRecord fresh;
        fresh.kind = EntityKind::Point;
        const Command add = AddRecord<EntityRecord>{fresh};
        const auto inverse = doc.invert(add);
        REQUIRE(inverse.has_value());
        REQUIRE(doc.apply(add).ok());
        CHECK(doc.entities().size() == 2);
        REQUIRE(doc.apply(*inverse).ok());
        // Records match; the watermark legitimately moved on.
        CHECK(sameRecords(doc, afterAdd));
    }
}

TEST_CASE("inverting a command that would be refused yields nothing") {
    // A caller that inverts first and applies second never journals a no-op.
    Document doc;
    CHECK_FALSE(doc.invert(RemoveRecord<EntityRecord>{EntityId(7)}).has_value());

    EntityRecord orphan;
    orphan.kind = EntityKind::Point;
    orphan.id = EntityId(7);
    CHECK_FALSE(doc.invert(SetRecord<EntityRecord>{orphan}).has_value());
}

TEST_CASE("parameter cycles are refused at the command layer") {
    Document doc;
    ParameterRecord gutter;
    gutter.name = "gutter";
    gutter.value = Slot(8.0);
    const auto added = doc.apply(AddRecord<ParameterRecord>{gutter});
    REQUIRE(added.ok());
    const ParameterId id(added.allocated);

    ParameterRecord selfRef;
    selfRef.id = id;
    selfRef.name = "gutter";
    selfRef.value = Slot::parameter(id);
    CHECK(doc.apply(SetRecord<ParameterRecord>{selfRef}).error ==
          CommandError::CyclicParameter);
    CHECK(*doc.evaluate(Slot::parameter(id)) == doctest::Approx(8.0));
}

TEST_CASE("a slot referencing a missing parameter is refused") {
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 1.0, 0.0);
    CHECK_FALSE(addConstraint(doc, ConstraintKind::PointPointDistance, {p, q},
                              Slot::parameter(ParameterId(42)))
                    .valid());
}

TEST_CASE("regions reference edges, never points") {
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 1.0, 0.0);
    const EntityId seg = addSegment(doc, p, q);

    RegionRecord bad;
    bad.boundary = {p};
    CHECK(doc.apply(AddRecord<RegionRecord>{bad}).error == CommandError::UnknownOperand);

    RegionRecord good;
    good.boundary = {seg};
    CHECK(doc.apply(AddRecord<RegionRecord>{good}).ok());

    RegionRecord empty;
    CHECK_FALSE(doc.apply(AddRecord<RegionRecord>{empty}).ok());
}

TEST_CASE("deletion dependents are reported, never hidden") {
    // There is no "cannot delete: in use" anywhere in the tool. Relations that
    // reference a deleted operand die with it, and the removal is counted.
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 1.0, 0.0);
    const EntityId seg = addSegment(doc, p, q);
    addConstraint(doc, ConstraintKind::Horizontal, {seg});
    addConstraint(doc, ConstraintKind::Coincident, {p, q});

    RegionRecord region;
    region.boundary = {seg};
    REQUIRE(doc.apply(AddRecord<RegionRecord>{region}).ok());

    const Dependents onP = dependentsOf(doc, p);
    CHECK(onP.constraints.size() == 1);
    CHECK(onP.regions.empty());

    const Dependents onSeg = dependentsOf(doc, seg);
    CHECK(onSeg.constraints.size() == 1);
    CHECK(onSeg.regions.size() == 1);
    // The point is also depended on by the segment built from it.
    CHECK(onP.entities.size() == 1);
}

TEST_CASE("a removal that would dangle is refused, and the cascade is available") {
    // Referential integrity is a model invariant. The refusal never reaches the
    // user because deletionStep always exists: the answer to a deletion is a
    // bigger deletion, reported by count, never a "cannot delete" dialog.
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 1.0, 0.0);
    const EntityId seg = addSegment(doc, p, q);
    addConstraint(doc, ConstraintKind::Horizontal, {seg});
    RegionRecord region;
    region.boundary = {seg};
    REQUIRE(doc.apply(AddRecord<RegionRecord>{region}).ok());

    CHECK(doc.apply(RemoveRecord<EntityRecord>{p}).error == CommandError::HasDependents);
    CHECK(doc.entities().contains(p));

    const std::vector<Command> cascade = deletionStep(doc, p);
    for(const Command &c : cascade) REQUIRE(doc.apply(c).ok());

    CHECK_FALSE(doc.entities().contains(p));
    CHECK_FALSE(doc.entities().contains(seg));
    CHECK(doc.constraints().empty());
    CHECK(doc.regions().empty());
    // q was not part of the cascade: only what would have dangled goes.
    CHECK(doc.entities().contains(q));
}

TEST_CASE("a leaf entity deletes on its own") {
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    CHECK(deletionStep(doc, p).size() == 1);
    CHECK(doc.apply(RemoveRecord<EntityRecord>{p}).ok());
}

TEST_CASE("layers and styles are organization, not semantics") {
    Document doc;
    LayerRecord layer;
    layer.name = "guides";
    layer.visible = false;
    layer.locked = true;
    const auto added = doc.apply(AddRecord<LayerRecord>{layer});
    REQUIRE(added.ok());
    const LayerId lid(added.allocated);

    // Geometry may name a layer, and a missing one is refused.
    EntityRecord p;
    p.kind = EntityKind::Point;
    p.layer = lid;
    CHECK(doc.apply(AddRecord<EntityRecord>{p}).ok());

    EntityRecord orphan;
    orphan.kind = EntityKind::Point;
    orphan.layer = LayerId(77);
    CHECK(doc.apply(AddRecord<EntityRecord>{orphan}).error == CommandError::UnknownOperand);
}

TEST_CASE("construction is a role, not a type") {
    // A guide is an ordinary line with a render role, which is why every guide
    // capability falls out of not having a guide type.
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 1.0, 0.0);
    const EntityId seg = addSegment(doc, p, q);

    EntityRecord asGuide = *doc.entities().find(seg);
    asGuide.role = Role::Construction;
    REQUIRE(doc.apply(SetRecord<EntityRecord>{asGuide}).ok());

    // It still takes constraints identically.
    CHECK(addConstraint(doc, ConstraintKind::Horizontal, {seg}).valid());
}
