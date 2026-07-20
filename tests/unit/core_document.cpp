#include <doctest/doctest.h>

#include "core/composition.h"
#include "core/document.h"
#include "core/persist.h"
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

TEST_CASE("horizontal carries a nullable reference axis") {
    // PRINCIPLES fixes horizontal and vertical as parallelism to a reference
    // axis with the document frame as the default. Null is that default and is
    // what every horizontal in the corpus means; naming a segment says the
    // relation is to that axis instead. Landed now rather than at stage 7 so
    // persist, undo, signatures and the corpus change once.
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 100.0, 0.0);
    const EntityId subject = addSegment(doc, p, q);
    const EntityId r = addPoint(doc, 0.0, 50.0);
    const EntityId s = addPoint(doc, 100.0, 110.0);
    const EntityId axis = addSegment(doc, r, s);

    SUBCASE("the reference may be absent") {
        const ConstraintId id = addConstraint(doc, ConstraintKind::Horizontal, {subject});
        REQUIRE(id.valid());
        const ConstraintRecord &c = *doc.constraints().find(id);
        CHECK(boundOperandCount(c) == 1);
        CHECK_FALSE(c.operands[1].valid());
    }

    SUBCASE("the reference may be present") {
        const ConstraintId id = addConstraint(doc, ConstraintKind::Horizontal, {subject, axis});
        REQUIRE(id.valid());
        CHECK(boundOperandCount(*doc.constraints().find(id)) == 2);
    }

    SUBCASE("a reference that is not a segment is refused") {
        ConstraintRecord bad;
        bad.kind = ConstraintKind::Horizontal;
        bad.operands[0] = subject;
        bad.operands[1] = p;  // a point names no axis
        CHECK(doc.apply(AddRecord<ConstraintRecord>{bad}).error == CommandError::WrongSignature);
    }

    SUBCASE("a reference that does not exist is refused") {
        ConstraintRecord bad;
        bad.kind = ConstraintKind::Horizontal;
        bad.operands[0] = subject;
        bad.operands[1] = EntityId(999);
        CHECK(doc.apply(AddRecord<ConstraintRecord>{bad}).error == CommandError::UnknownOperand);
    }

    SUBCASE("a gap in the operands is refused") {
        // Optional operands are a prefix. A record with slot one empty and slot
        // two filled is one no command could have produced.
        ConstraintRecord bad;
        bad.kind = ConstraintKind::Horizontal;
        bad.operands[0] = subject;
        bad.operands[2] = axis;
        CHECK(doc.apply(AddRecord<ConstraintRecord>{bad}).error == CommandError::WrongSignature);
    }

    SUBCASE("applicability still reads one segment") {
        // Selecting a single segment has to keep offering horizontal, or the
        // nullable operand would have cost the action its whole surface.
        const std::vector<EntityKind> one{EntityKind::Segment};
        CHECK(signatureMatches(ConstraintKind::Horizontal, one));
        CHECK(signatureMatches(ConstraintKind::Vertical, one));
    }
}

TEST_CASE("only a kind with alternative forms may name one") {
    // The alternative is a choice inside one relation, not a free byte. A kind
    // that has no second form has to carry the default, or two records meaning
    // the same thing would compare unequal and serialize differently.
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 100.0, 0.0);
    const EntityId segment = addSegment(doc, p, q);

    ConstraintRecord horizontal;
    horizontal.kind = ConstraintKind::Horizontal;
    horizontal.operands[0] = segment;
    horizontal.alternative = 1;
    CHECK(doc.apply(AddRecord<ConstraintRecord>{horizontal}).error ==
          CommandError::WrongSignature);

    // Tangency has two forms, so it may name either.
    const EntityId centre = addPoint(doc, 0.0, 200.0);
    const EntityId arcStart = addPoint(doc, 50.0, 200.0);
    const EntityId arcEnd = addPoint(doc, 0.0, 250.0);
    EntityRecord a;
    a.kind = EntityKind::Arc;
    a.points = {centre, arcStart, arcEnd};
    const EntityId arc(doc.apply(AddRecord<EntityRecord>{a}).allocated);

    for(uint8_t form : {uint8_t{0}, uint8_t{1}}) {
        CAPTURE(int(form));
        ConstraintRecord t;
        t.kind = ConstraintKind::Tangent;
        t.operands[0] = arc;
        t.operands[1] = segment;
        t.alternative = form;
        CHECK(doc.apply(AddRecord<ConstraintRecord>{t}).ok());
    }

    // But not a third one it does not have.
    ConstraintRecord tooMany;
    tooMany.kind = ConstraintKind::Tangent;
    tooMany.operands[0] = arc;
    tooMany.operands[1] = segment;
    tooMany.alternative = 2;
    CHECK(doc.apply(AddRecord<ConstraintRecord>{tooMany}).error == CommandError::WrongSignature);
}

TEST_CASE("the two tangent forms are different declarations") {
    // Two records over the same pair that differ only in which end they hold
    // at are not duplicates of each other, and comparing them must say so or
    // undo would restore the wrong one.
    ConstraintRecord atStart;
    atStart.id = ConstraintId(1);
    atStart.kind = ConstraintKind::Tangent;
    atStart.operands[0] = EntityId(1);
    atStart.operands[1] = EntityId(2);

    ConstraintRecord atEnd = atStart;
    atEnd.alternative = 1;
    CHECK(atStart != atEnd);
}

TEST_CASE("a reference axis is depended on like any other operand") {
    // Deleting the axis a relation is measured against has to take the relation
    // with it, or the document keeps a horizontal pointing at nothing.
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 100.0, 0.0);
    const EntityId subject = addSegment(doc, p, q);
    const EntityId r = addPoint(doc, 0.0, 50.0);
    const EntityId s = addPoint(doc, 100.0, 110.0);
    const EntityId axis = addSegment(doc, r, s);
    REQUIRE(addConstraint(doc, ConstraintKind::Horizontal, {subject, axis}).valid());

    CHECK(dependentsOf(doc, axis).constraints.size() == 1);
    CHECK(doc.apply(RemoveRecord<EntityRecord>{axis}).error == CommandError::HasDependents);

    for(const Command &c : deletionStep(doc, axis)) REQUIRE(doc.apply(c).ok());
    CHECK(doc.constraints().empty());
    CHECK(doc.entities().contains(subject));
}

TEST_CASE("a parameter still being read cannot simply vanish") {
    // A dangling slot is worse than a dangling operand: it evaluates to nullopt
    // and the solver translation reads that as zero, so the dimension drives to
    // nothing with no refusal anywhere. The deletion freezes instead.
    Document doc;
    ParameterRecord width;
    width.name = "width";
    width.value = Slot(40.0);
    const ParameterId wid(doc.apply(AddRecord<ParameterRecord>{width}).allocated);

    ParameterRecord half;
    half.name = "half";
    half.value = Slot::binary(ExprOp::Divide, Slot::parameter(wid), Slot(2.0));
    const ParameterId hid(doc.apply(AddRecord<ParameterRecord>{half}).allocated);

    StyleRecord style;
    style.name = "hairline";
    style.strokeWidth = Slot::parameter(hid);
    const StyleId sid(doc.apply(AddRecord<StyleRecord>{style}).allocated);

    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 40.0, 0.0);
    const ConstraintId dim =
        addConstraint(doc, ConstraintKind::PointPointDistance, {p, q}, Slot::parameter(wid));
    REQUIRE(dim.valid());

    const ParameterDependents deps = dependentsOf(doc, wid);
    CHECK(deps.constraints.size() == 1);
    CHECK(deps.parameters.size() == 1);
    CHECK(deps.styles.empty());  // the style reads half, not width
    CHECK(deps.count() == 2);

    CHECK(doc.apply(RemoveRecord<ParameterRecord>{wid}).error == CommandError::HasDependents);
    CHECK(doc.parameters().contains(wid));

    for(const Command &c : deletionStep(doc, wid)) REQUIRE(doc.apply(c).ok());

    CHECK_FALSE(doc.parameters().contains(wid));
    // Every value the drawing had, it still has — only the name is gone.
    CHECK(*doc.evaluate(doc.constraints().find(dim)->value) == doctest::Approx(40.0));
    CHECK(*doc.evaluate(doc.parameters().find(hid)->value) == doctest::Approx(20.0));
    CHECK(*doc.evaluate(doc.styles().find(sid)->strokeWidth) == doctest::Approx(20.0));
    // A frozen slot reads nothing, so half is now deletable on its own.
    CHECK(dependentsOf(doc, hid).count() == 1);
}

TEST_CASE("an unread parameter deletes on its own") {
    Document doc;
    ParameterRecord gutter;
    gutter.name = "gutter";
    gutter.value = Slot(8.0);
    const ParameterId id(doc.apply(AddRecord<ParameterRecord>{gutter}).allocated);

    CHECK(deletionStep(doc, id).size() == 1);
    CHECK(doc.apply(RemoveRecord<ParameterRecord>{id}).ok());
}

TEST_CASE("a style width is a slot, and is checked like one") {
    Document doc;
    StyleRecord style;
    style.name = "hairline";
    style.strokeWidth = Slot::parameter(ParameterId(42));
    CHECK(doc.apply(AddRecord<StyleRecord>{style}).error == CommandError::UnknownOperand);
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

    // An empty outline is accepted, and only looks like a hole in the
    // validation. It is the state a region lands in when the edges it bounded
    // were deleted, and refusing it would mean a deletion could only proceed by
    // taking the fill with it. Whether a region encloses anything is asked in
    // one place, and that place is not the validator.
    RegionRecord empty;
    CHECK(doc.apply(AddRecord<RegionRecord>{empty}).ok());
    CHECK(regionState(doc, doc.regions().records().back()) == RegionState::Broken);
}

TEST_CASE("a region gets its area exactly one way") {
    // Outline names edges, composite names regions, and never both: a record
    // populating each would leave "what is this region's area" with two answers
    // and no rule for which wins.
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 1.0, 0.0);
    const EntityId seg = addSegment(doc, p, q);

    RegionRecord outline;
    outline.boundary = {seg};
    REQUIRE(doc.apply(AddRecord<RegionRecord>{outline}).ok());
    const RegionId first = doc.regions().records().back().id;

    RegionRecord both;
    both.op = CompositeOp::Union;
    both.boundary = {seg};
    both.operands = {first};
    CHECK(doc.apply(AddRecord<RegionRecord>{both}).error == CommandError::WrongSignature);

    RegionRecord composite;
    composite.op = CompositeOp::Union;
    composite.operands = {first};
    REQUIRE(doc.apply(AddRecord<RegionRecord>{composite}).ok());
    const RegionId second = doc.regions().records().back().id;

    // An operand belongs to at most one composite: two would draw it twice and
    // give lifting it back out two answers.
    RegionRecord rival;
    rival.op = CompositeOp::Intersect;
    rival.operands = {first};
    CHECK(doc.apply(AddRecord<RegionRecord>{rival}).error == CommandError::HasDependents);

    // And a composite may not reach itself. The per-frame path walk follows the
    // same edges, so a cycle is not merely nonsense, it does not terminate.
    RegionRecord loop = *doc.regions().find(first);
    loop.op = CompositeOp::Union;
    loop.boundary.clear();
    loop.operands = {second};
    CHECK(doc.apply(SetRecord<RegionRecord>{loop}).error == CommandError::CyclicParameter);
}

TEST_CASE("a composite is dismantled, never consumed") {
    // Nothing was taken to make it, so nothing is stranded by taking it apart.
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 1.0, 0.0);
    const EntityId a = addSegment(doc, p, q);
    const EntityId b = addSegment(doc, q, p);

    RegionRecord one;
    one.boundary = {a};
    REQUIRE(doc.apply(AddRecord<RegionRecord>{one}).ok());
    const RegionId first = doc.regions().records().back().id;
    RegionRecord two;
    two.boundary = {b};
    REQUIRE(doc.apply(AddRecord<RegionRecord>{two}).ok());
    const RegionId second = doc.regions().records().back().id;

    RegionRecord composite;
    composite.op = CompositeOp::Subtract;
    composite.operands = {first, second};
    REQUIRE(doc.apply(AddRecord<RegionRecord>{composite}).ok());
    const RegionId both = doc.regions().records().back().id;

    // The operands are no longer drawn in their own right, but they are still
    // there — which is what makes lifting a real inverse.
    CHECK_FALSE(isTopLevelRegion(doc, first));
    CHECK(isTopLevelRegion(doc, both));

    REQUIRE(doc.apply(RemoveRecord<RegionRecord>{both}).ok());
    CHECK(doc.regions().contains(first));
    CHECK(doc.regions().contains(second));
    CHECK(isTopLevelRegion(doc, first));

    // Removing an operand out from under a composite is refused, and the
    // deletion step lifts it out first.
    RegionRecord again;
    again.op = CompositeOp::Subtract;
    again.operands = {first, second};
    REQUIRE(doc.apply(AddRecord<RegionRecord>{again}).ok());
    const RegionId rebuilt = doc.regions().records().back().id;

    CHECK(doc.apply(RemoveRecord<RegionRecord>{first}).error == CommandError::HasDependents);
    for(const Command &c : deletionStep(doc, first)) REQUIRE(doc.apply(c).ok());
    CHECK_FALSE(doc.regions().contains(first));
    CHECK(doc.regions().find(rebuilt)->operands == std::vector<RegionId>{second});
    // And a composite thinned past combining anything says so rather than
    // vanishing.
    CHECK(regionState(doc, rebuilt) == RegionState::Broken);
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
    // The region shrank rather than dying. Nothing higher-order is silently
    // discarded: the fill the user asked for is still declared, it renders as a
    // diagnostic because it no longer encloses anything, and one undo has it
    // back whole.
    REQUIRE(doc.regions().size() == 1);
    CHECK(doc.regions().records().front().boundary.empty());
    CHECK(regionState(doc, doc.regions().records().front()) == RegionState::Broken);
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

TEST_CASE("a layer still in use is emptied before it goes, never left dangling") {
    // Load validation checks layer references, so an entity left naming a layer
    // that is gone is a document that serializes and will not deserialize —
    // saved once and unopenable after. Nothing deletes a layer today; the
    // palette will grow one, and it will find the hole already closed.
    Document doc;
    LayerRecord layer;
    layer.name = "guides";
    const auto added = doc.apply(AddRecord<LayerRecord>{layer});
    REQUIRE(added.ok());
    const LayerId lid(added.allocated);

    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 0.0);
    const EntityId edge = addSegment(doc, a, b);
    for(EntityId id : {a, b, edge}) {
        EntityRecord r = *doc.entities().find(id);
        r.layer = lid;
        REQUIRE(doc.apply(SetRecord<EntityRecord>{r}).ok());
    }
    RegionRecord region;
    region.boundary = {edge};
    region.layer = lid;
    const auto regionAdded = doc.apply(AddRecord<RegionRecord>{region});
    REQUIRE(regionAdded.ok());
    const RegionId rid(regionAdded.allocated);

    CHECK(dependentsOf(doc, lid).count() == 4);
    CHECK(doc.apply(RemoveRecord<LayerRecord>{lid}).error == CommandError::HasDependents);

    // The freeze-shaped answer: only the organization the user deleted is lost.
    // Everything moves to the base layer, which every document has without
    // anyone creating one, so the refusal never reaches the user as a refusal.
    for(const Command &command : deletionStep(doc, lid)) REQUIRE(doc.apply(command).ok());
    CHECK_FALSE(doc.layers().contains(lid));
    CHECK(doc.entities().find(edge)->layer == LayerId());
    CHECK(doc.regions().find(rid)->layer == LayerId());
    CHECK(doc.entities().size() == 3);
    CHECK(doc.regions().size() == 1);

    // And the state it leaves is one the loader accepts, which is the whole
    // point: the failure this closes was a file that wrote cleanly and refused
    // to come back.
    Document reloaded;
    REQUIRE(deserialize(serialize(doc), reloaded).ok);
    CHECK(sameRecords(doc, reloaded));
}

TEST_CASE("a style still in use is unhooked before it goes") {
    // Same hole, same shape, and the same answer: the references are nulled and
    // the geometry falls back to the default look. Nothing else about the
    // records changes, because a style is hung off a record rather than
    // something the record is built from.
    Document doc;
    StyleRecord style;
    style.name = "ink";
    style.strokeWidth = Slot(3.0);
    const auto added = doc.apply(AddRecord<StyleRecord>{style});
    REQUIRE(added.ok());
    const StyleId sid(added.allocated);

    const EntityId a = addPoint(doc, 0.0, 0.0);
    EntityRecord styled = *doc.entities().find(a);
    styled.style = sid;
    REQUIRE(doc.apply(SetRecord<EntityRecord>{styled}).ok());

    CHECK(dependentsOf(doc, sid).count() == 1);
    CHECK(doc.apply(RemoveRecord<StyleRecord>{sid}).error == CommandError::HasDependents);

    for(const Command &command : deletionStep(doc, sid)) REQUIRE(doc.apply(command).ok());
    CHECK_FALSE(doc.styles().contains(sid));
    CHECK_FALSE(doc.entities().find(a)->style.valid());
    CHECK(doc.entities().find(a)->seeds[0] == 0.0);

    Document reloaded;
    REQUIRE(deserialize(serialize(doc), reloaded).ok);
    CHECK(sameRecords(doc, reloaded));
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

TEST_CASE("deleting a member shrinks its group rather than dissolving it") {
    // Membership is organization, not structure: a group that lost one entity
    // still names the others correctly. Removing the whole record took the
    // grouping of everything the user did not delete with it.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 0.0);
    const EntityId c = addPoint(doc, 10.0, 10.0);
    const EntityId edge = addSegment(doc, a, b);

    GroupRecord group;
    group.name = "frame";
    group.members = {a, b, c, edge};
    const CommandResult added = doc.apply(AddRecord<GroupRecord>{group});
    REQUIRE(added.ok());
    const GroupId id(added.allocated);

    // Deleting a takes the segment built on it, so two members leave at once —
    // and in one record. One per victim would each drop a different member,
    // and the last applied would restore the ones before it.
    const std::vector<Command> cascade = deletionStep(doc, a);
    size_t shrinks = 0;
    for(const Command &command : cascade) {
        CHECK_FALSE(std::holds_alternative<RemoveRecord<GroupRecord>>(command));
        if(std::holds_alternative<SetRecord<GroupRecord>>(command)) shrinks++;
    }
    CHECK(shrinks == 1);

    for(const Command &command : cascade) REQUIRE(doc.apply(command).ok());

    const GroupRecord *after = doc.groups().find(id);
    REQUIRE(after != nullptr);
    CHECK(after->name == "frame");
    CHECK(after->members == std::vector<EntityId>{b, c});
}

TEST_CASE("a relation a tag is built on shrinks the tag rather than dangling") {
    // The model gave three answers about one state: tagState called a dangling
    // tag→constraint reference broken-but-legal, validate refused it, and the
    // loader refused the file it came from — a document that saves and will not
    // open. Nothing creates tags yet, and stage 7's rectangle does; its defining
    // relations are exactly what decline and delete remove.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 0.0);
    const EntityId edge = addSegment(doc, a, b);
    const ConstraintId level = addConstraint(doc, ConstraintKind::Horizontal, {edge});
    const ConstraintId length =
        addConstraint(doc, ConstraintKind::PointPointDistance, {a, b}, Slot(10.0));

    TagRecord tag;
    tag.kind = TagKind::Rectangle;
    tag.entities = {edge};
    tag.constraints = {level, length};
    const CommandResult added = doc.apply(AddRecord<TagRecord>{tag});
    REQUIRE(added.ok());
    const TagId id(added.allocated);

    // Refused on its own, exactly as an entity removal is, so no path can forget
    // to shrink.
    CHECK(doc.apply(RemoveRecord<ConstraintRecord>{level}).error ==
          CommandError::HasDependents);

    const ConstraintId doomed[] = {level};
    for(const Command &command : deletionStep(doc, doomed)) REQUIRE(doc.apply(command).ok());

    const TagRecord *after = doc.tags().find(id);
    REQUIRE(after != nullptr);
    CHECK(after->constraints == std::vector<ConstraintId>{length});
    CHECK(after->entities == std::vector<EntityId>{edge});
    // Broken, because a rectangle is not one relation — and legal, which is what
    // makes the state one undo restores rather than one the loader refuses.
    CHECK(doc.validate(*after) == CommandError::None);
}

TEST_CASE("geometry and relations delete in one cascade") {
    // A selection reaching both is one gesture, so it is one cascade: a tag
    // losing a relation to the geometry's own dependents and another to the
    // user's naming can only be set once, and two passes would each compute
    // their shrink from the unshrunk record.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 0.0);
    const EntityId edge = addSegment(doc, a, b);
    const ConstraintId level = addConstraint(doc, ConstraintKind::Horizontal, {edge});
    const ConstraintId length =
        addConstraint(doc, ConstraintKind::PointPointDistance, {a, b}, Slot(10.0));

    TagRecord tag;
    tag.kind = TagKind::Rectangle;
    tag.entities = {edge};
    tag.constraints = {level, length};
    const CommandResult added = doc.apply(AddRecord<TagRecord>{tag});
    REQUIRE(added.ok());
    const TagId id(added.allocated);

    // Delete the edge and, in the same breath, name a relation that the edge's
    // own cascade would not have reached.
    const EntityId geometry[] = {edge};
    const ConstraintId relations[] = {length};
    const std::vector<Command> cascade = deletionStep(doc, geometry, relations);
    size_t shrinks = 0;
    for(const Command &command : cascade) {
        if(std::holds_alternative<SetRecord<TagRecord>>(command)) shrinks++;
    }
    CHECK(shrinks == 1);
    for(const Command &command : cascade) REQUIRE(doc.apply(command).ok());

    const TagRecord *after = doc.tags().find(id);
    REQUIRE(after != nullptr);
    CHECK(after->constraints.empty());
    CHECK(after->entities.empty());
}

TEST_CASE("a group survives losing its last member") {
    // An empty group is a named container the user can still add to. Deciding
    // it has outlived its purpose is their call, and deleting it is an action
    // they already have.
    Document doc;
    const EntityId only = addPoint(doc, 0.0, 0.0);

    GroupRecord group;
    group.name = "spares";
    group.members = {only};
    const CommandResult added = doc.apply(AddRecord<GroupRecord>{group});
    REQUIRE(added.ok());
    const GroupId id(added.allocated);

    for(const Command &command : deletionStep(doc, only)) REQUIRE(doc.apply(command).ok());

    const GroupRecord *after = doc.groups().find(id);
    REQUIRE(after != nullptr);
    CHECK(after->name == "spares");
    CHECK(after->members.empty());
}

TEST_CASE("a group shrink inverts exactly, like every other command") {
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 0.0);

    GroupRecord group;
    group.name = "pair";
    group.members = {a, b};
    const CommandResult added = doc.apply(AddRecord<GroupRecord>{group});
    REQUIRE(added.ok());
    const GroupId id(added.allocated);
    const GroupRecord before = *doc.groups().find(id);

    const std::vector<Command> cascade = deletionStep(doc, a);
    std::vector<Command> inverses;
    for(const Command &command : cascade) {
        const std::optional<Command> inverse = doc.invert(command);
        REQUIRE(inverse.has_value());
        inverses.push_back(*inverse);
        REQUIRE(doc.apply(command).ok());
    }
    for(size_t i = inverses.size(); i-- > 0;) REQUIRE(doc.apply(inverses[i]).ok());

    CHECK(*doc.groups().find(id) == before);
    CHECK(doc.entities().contains(a));
}

TEST_CASE("unused seed slots are canonical, like unused point slots") {
    // An unused seed slot is not scratch space. Junk left in one survives the
    // commit and serializes, so two records meaning the same thing compare
    // unequal and a round-trip quietly loses the difference.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 0.0);

    // A segment owns no parameters at all.
    EntityRecord segment;
    segment.kind = EntityKind::Segment;
    segment.points = {a, b, EntityId()};
    segment.seeds = {3.0, 0.0};
    CHECK(doc.apply(AddRecord<EntityRecord>{segment}).error == CommandError::WrongSignature);

    // A circle owns exactly one, so the second slot must be zero.
    EntityRecord circle;
    circle.kind = EntityKind::Circle;
    circle.points = {a, EntityId(), EntityId()};
    circle.seeds = {20.0, 7.0};
    CHECK(doc.apply(AddRecord<EntityRecord>{circle}).error == CommandError::WrongSignature);

    circle.seeds = {20.0, 0.0};
    CHECK(doc.apply(AddRecord<EntityRecord>{circle}).ok());
}

TEST_CASE("a watermark on load raises and never lowers") {
    // Records reserve above themselves as they load, and nothing orders the
    // watermark line before them. A file whose watermark sits below what its
    // records hold would otherwise reissue an ID one of them already has, and
    // every reference to that record would silently rebind to the new one.
    Document doc;
    const EntityId first = addPoint(doc, 0.0, 0.0);
    const EntityId second = addPoint(doc, 10.0, 0.0);
    REQUIRE(second.value() > first.value());

    std::string text = serialize(doc);
    const size_t at = text.find("watermark ");
    REQUIRE(at != std::string::npos);
    const size_t lineEnd = text.find('\n', at);
    REQUIRE(lineEnd != std::string::npos);
    std::string watermark = text.substr(at, lineEnd - at + 1);
    text.erase(at, lineEnd - at + 1);

    // As a hand edit might leave it: a value below what the records hold, and
    // ordered after them, so the records have already reserved above themselves
    // by the time the line is read. A watermark ahead of the records is
    // harmless whatever it says, since the reservations follow it.
    const size_t valueAt = watermark.find("entity=") + std::string("entity=").size();
    watermark.replace(valueAt, watermark.find(' ', valueAt) - valueAt, "1");
    text += watermark;

    Document loaded;
    REQUIRE(deserialize(text, loaded).ok);
    CHECK(loaded.entities().contains(first));
    CHECK(loaded.entities().contains(second));

    // The next ID issued clears everything the file held, rather than colliding.
    const EntityId fresh = addPoint(loaded, 1.0, 1.0);
    REQUIRE(fresh.valid());
    CHECK(fresh.value() > second.value());
    CHECK(loaded.entities().size() == 3);
}
