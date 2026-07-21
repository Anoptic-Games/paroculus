// Structure operations, as properties rather than as examples.
//
// The three claims stage 7 makes about transforms are all of the form "this
// operation and this family of relations commute", and each is checkable by
// measuring residuals before anything re-solves. That is the point of doing the
// rewrite exactly: if a rotation had gone through the solver, "the residuals are
// still zero" would be a statement about Newton's tolerance rather than about
// the transform.
#include <doctest/doctest.h>

#include <cmath>

#include "core/composition.h"
#include "core/compound.h"
#include "core/copy.h"
#include "core/measure.h"
#include "core/persist.h"
#include "core/pose.h"
#include "core/topology.h"
#include "core/transform.h"
#include "solve/solve.h"
#include "support/build.h"

using namespace paroculus;
using namespace paroculus::test;

namespace {

// A cluster with nothing axis-referenced in it: a triangle whose shape is held
// by two equal sides and a fixed base. Every relation on it is rigid-motion
// invariant, so a rotation must leave all three residuals where they were.
struct Triangle {
    Document doc;
    EntityId a, b, c;
    EntityId ab, bc, ca;
    std::vector<EntityId> all;
};

Triangle rigidTriangle() {
    Triangle t;
    t.a = addPoint(t.doc, 0.0, 0.0);
    t.b = addPoint(t.doc, 40.0, 0.0);
    t.c = addPoint(t.doc, 20.0, 30.0);
    t.ab = addSegment(t.doc, t.a, t.b);
    t.bc = addSegment(t.doc, t.b, t.c);
    t.ca = addSegment(t.doc, t.c, t.a);
    addConstraint(t.doc, ConstraintKind::EqualLength, {t.bc, t.ca});
    addConstraint(t.doc, ConstraintKind::PointPointDistance, {t.a, t.b}, Slot(40.0));
    t.all = {t.a, t.b, t.c, t.ab, t.bc, t.ca};
    return t;
}

double worstResidual(const Document &doc) {
    const Pose pose(doc);
    double worst = 0.0;
    for(const ConstraintRecord &c : doc.constraints().records()) {
        if(const std::optional<double> r = residual(pose, c)) {
            worst = std::max(worst, std::abs(*r));
        }
    }
    return worst;
}

// Solves the whole document and reports how far it moved anything. Zero means
// the transform landed on a solution and the re-solve was the identity.
double resolveDrift(Document &doc) {
    SolveContext context = SolveContext::forWholeDocument(doc);
    const std::vector<SeedSpan> before = context.params();
    SolveOptions options;
    options.diagnoseFailures = false;
    if(!solve(doc, context, options).ok()) return std::numeric_limits<double>::infinity();

    double worst = 0.0;
    for(const SeedSpan &after : context.params()) {
        for(const SeedSpan &was : before) {
            if(was.entity != after.entity) continue;
            for(size_t i = 0; i < MAX_ENTITY_PARAMS; i++) {
                worst = std::max(worst, std::abs(after.seeds[i] - was.seeds[i]));
            }
        }
    }
    return worst;
}

bool applyAll(Document &doc, const std::vector<Command> &commands) {
    for(const Command &c : commands) {
        if(!doc.apply(c).ok()) return false;
    }
    return true;
}

}  // namespace

TEST_CASE("rotating a rigid cluster is an isometry, and re-solving is the identity") {
    Triangle t = rigidTriangle();
    // Already satisfied where it was drawn, so any residual afterwards is the
    // rotation's doing rather than the fixture's.
    REQUIRE(worstResidual(t.doc) < 1e-9);

    RotateOptions options;
    options.centre = Point{7.0, -3.0};  // deliberately not the centroid
    options.angle = 0.7;
    const TransformStep step = rotateStep(t.doc, t.all, options);
    REQUIRE(step.ok());
    // Three points moved; the segments own no parameters and are carried by
    // their ends.
    CHECK(step.moved == 3);
    CHECK(step.retargeted == 0);
    REQUIRE(applyAll(t.doc, step.commands));

    // The whole claim: every internal residual is still zero, before anything
    // has re-solved. A rotation that had gone through the solver could only ever
    // report Newton's tolerance here.
    CHECK(worstResidual(t.doc) < 1e-12);
    CHECK(resolveDrift(t.doc) < 1e-9);
}

TEST_CASE("rotation preserves lengths exactly") {
    Triangle t = rigidTriangle();
    const Pose before(t.doc);
    const EntityId ends[] = {t.a, t.b};
    const double baseline = *measure(before, ConstraintKind::PointPointDistance, ends);

    RotateOptions options;
    options.centre = Point{-11.0, 4.0};
    options.angle = -2.3;
    REQUIRE(applyAll(t.doc, rotateStep(t.doc, t.all, options).commands));

    const Pose after(t.doc);
    CHECK(*measure(after, ConstraintKind::PointPointDistance, ends) ==
          doctest::Approx(baseline).epsilon(1e-12));
}

TEST_CASE("a kept-axes rotation resists and a retargeted one does not") {
    // The axis question, both answers, on the geometry that raises it. Two
    // segments squared against the document frame: rotating them is exactly the
    // case PRINCIPLES says has two honest answers.
    auto build = []() {
        Document doc;
        const EntityId a = addPoint(doc, 0.0, 0.0);
        const EntityId b = addPoint(doc, 50.0, 0.0);
        const EntityId c = addPoint(doc, 50.0, 30.0);
        const EntityId across = addSegment(doc, a, b);
        const EntityId up = addSegment(doc, b, c);
        addConstraint(doc, ConstraintKind::Horizontal, {across});
        addConstraint(doc, ConstraintKind::Vertical, {up});
        return std::make_tuple(std::move(doc), std::vector<EntityId>{a, b, c, across, up});
    };

    RotateOptions options;
    options.centre = Point{0.0, 0.0};
    options.angle = 0.4;

    SUBCASE("the question exists and is counted before it is answered") {
        auto [doc, selection] = build();
        const std::vector<EntityId> moved = transformClosure(doc, selection);
        CHECK(axisReferencedIn(doc, moved).size() == 2);
    }

    SUBCASE("keeping the document axes lets the solver fight the rotation") {
        auto [doc, selection] = build();
        options.axes = AxisAnswer::KeepDocumentAxes;
        const TransformStep step = rotateStep(doc, selection, options);
        REQUIRE(step.ok());
        CHECK(step.retargeted == 0);
        CHECK(!step.frame.valid());
        REQUIRE(applyAll(doc, step.commands));

        // The seeds are rotated, so the axis relations are violated by exactly
        // the rotation — which is the resistance, stated as a number.
        CHECK(worstResidual(doc) > 1e-3);
        // And the re-solve pulls it back. Not the identity: that is the whole
        // difference between the two answers.
        CHECK(resolveDrift(doc) > 1e-3);
    }

    SUBCASE("retargeting to a cluster frame carries the axes with it") {
        auto [doc, selection] = build();
        options.axes = AxisAnswer::RetargetToClusterFrame;
        const TransformStep step = rotateStep(doc, selection, options);
        REQUIRE(step.ok());
        CHECK(step.retargeted == 2);
        REQUIRE(step.frame.valid());
        REQUIRE(applyAll(doc, step.commands));

        // The frame is construction geometry, so it constrains and never draws
        // as part of the sketch or attracts a snap.
        const EntityRecord *frame = doc.entities().find(step.frame);
        REQUIRE(frame != nullptr);
        CHECK(frame->kind == EntityKind::Segment);
        CHECK(frame->role == Role::Construction);
        CHECK(doc.entities().find(frame->points[0])->role == Role::Construction);

        // Both relations now name it, and both are still the kind the user
        // declared: the same declaration against a different reference.
        size_t referenced = 0;
        for(const ConstraintRecord &c : doc.constraints().records()) {
            if(c.kind != ConstraintKind::Horizontal && c.kind != ConstraintKind::Vertical) {
                continue;
            }
            CHECK(boundOperandCount(c) == 2);
            CHECK(c.operands[1] == step.frame);
            referenced++;
        }
        CHECK(referenced == 2);

        // Nothing is violated and nothing moves. The cluster's horizontal tilted
        // with it, which is what rigid-body style means.
        //
        // The tolerance is looser here than on the distance residuals above and
        // has to be: an axis residual is an angle recovered through an arc
        // cosine, which loses half its significant digits as the angle
        // approaches zero — precisely the case a satisfied parallel is. What is
        // being asserted is that the relation holds; asserting it to 1e-12 would
        // be asserting something about acos.
        CHECK(worstResidual(doc) < 1e-5);
        CHECK(resolveDrift(doc) < 1e-6);
    }
}

TEST_CASE("uniform scale rescales absolutes only when asked, and never ratios") {
    // The asymmetry that is the project thesis: a proportion-built drawing
    // rescales cleanly and an absolute dimension pins physical size.
    auto build = []() {
        Document doc;
        const EntityId a = addPoint(doc, 0.0, 0.0);
        const EntityId b = addPoint(doc, 40.0, 0.0);
        const EntityId c = addPoint(doc, 40.0, 20.0);
        const EntityId first = addSegment(doc, a, b);
        const EntityId second = addSegment(doc, b, c);
        addConstraint(doc, ConstraintKind::LengthRatio, {first, second}, Slot(2.0));
        addConstraint(doc, ConstraintKind::PointPointDistance, {a, b}, Slot(40.0));
        return std::make_tuple(std::move(doc),
                               std::vector<EntityId>{a, b, c, first, second});
    };

    ScaleOptions options;
    options.centre = Point{0.0, 0.0};
    options.factor = 3.0;

    SUBCASE("the taxonomy decides which family is the question") {
        auto [doc, selection] = build();
        const std::vector<EntityId> moved = transformClosure(doc, selection);
        // The ratio is scale-invariant and is not offered as a question; the
        // distance is the whole of it.
        const std::vector<ConstraintId> absolutes = absoluteValuedIn(doc, moved);
        REQUIRE(absolutes.size() == 1);
        CHECK(doc.constraints().find(absolutes.front())->kind ==
              ConstraintKind::PointPointDistance);
    }

    SUBCASE("let them resist") {
        auto [doc, selection] = build();
        options.values = ValueAnswer::LetThemResist;
        const TransformStep step = scaleStep(doc, selection, options);
        REQUIRE(step.ok());
        CHECK(step.rescaled == 0);
        REQUIRE(applyAll(doc, step.commands));

        // The ratio still holds — it is scale-invariant, so tripling everything
        // left it exactly satisfied — while the distance does not.
        const Pose pose(doc);
        for(const ConstraintRecord &c : doc.constraints().records()) {
            const double r = std::abs(*residual(pose, c));
            if(c.kind == ConstraintKind::LengthRatio) {
                CHECK(r < 1e-12);
            } else {
                CHECK(r > 1.0);
            }
        }
    }

    SUBCASE("scale the values") {
        auto [doc, selection] = build();
        options.values = ValueAnswer::ScaleTheValues;
        const TransformStep step = scaleStep(doc, selection, options);
        REQUIRE(step.ok());
        CHECK(step.rescaled == 1);
        CHECK(step.straddling == 0);
        REQUIRE(applyAll(doc, step.commands));

        // The distance was multiplied by the factor and the ratio was not
        // touched, so the whole drawing is satisfied where it stands and the
        // re-solve is the identity.
        for(const ConstraintRecord &c : doc.constraints().records()) {
            if(c.kind == ConstraintKind::PointPointDistance) {
                CHECK(c.value.constant() == doctest::Approx(120.0));
            }
            if(c.kind == ConstraintKind::LengthRatio) {
                CHECK(c.value.constant() == doctest::Approx(2.0));
            }
        }
        CHECK(worstResidual(doc) < 1e-9);
        CHECK(resolveDrift(doc) < 1e-9);
    }
}

TEST_CASE("a dimension reaching outside what moved resists either way, and says so") {
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 40.0, 0.0);
    const EntityId outside = addPoint(doc, 100.0, 0.0);
    const EntityId edge = addSegment(doc, a, b);
    addConstraint(doc, ConstraintKind::PointPointDistance, {b, outside}, Slot(60.0));

    ScaleOptions options;
    options.centre = Point{0.0, 0.0};
    options.factor = 2.0;
    options.values = ValueAnswer::ScaleTheValues;
    const std::vector<EntityId> selection{a, b, edge};
    const TransformStep step = scaleStep(doc, selection, options);
    REQUIRE(step.ok());
    // Not rewritten: it says where this geometry sits relative to something that
    // did not move, and multiplying it would assert something about the point
    // the user did not select.
    CHECK(step.rescaled == 0);
    CHECK(step.straddling == 1);
}

TEST_CASE("a scale of zero is refused, and non-uniform scale is refused always") {
    Triangle t = rigidTriangle();
    ScaleOptions options;
    options.factor = 0.0;
    CHECK(scaleStep(t.doc, t.all, options).error == TransformError::Degenerate);

    // Even when the two factors agree. An operation that sometimes does the
    // thing and sometimes refuses teaches nothing about the model.
    CHECK(nonUniformScaleStep(t.doc, t.all, 2.0, 2.0).error == TransformError::NonUniform);
    CHECK(nonUniformScaleStep(t.doc, t.all, 2.0, 3.0).error == TransformError::NonUniform);
}

TEST_CASE("a transform refuses to write through a lock") {
    Document doc;
    LayerRecord layer;
    layer.name = "frozen";
    layer.locked = true;
    const uint32_t id = doc.apply(AddRecord<LayerRecord>{layer}).allocated;
    REQUIRE(id != 0);

    EntityRecord point;
    point.kind = EntityKind::Point;
    point.layer = LayerId(id);
    const EntityId a(doc.apply(AddRecord<EntityRecord>{point}).allocated);
    REQUIRE(a.valid());

    RotateOptions options;
    options.angle = 1.0;
    const std::vector<EntityId> selection{a};
    // Whole, not partial. A rotation that turned the half of a cluster it was
    // allowed to touch is worse than one that reports it cannot turn this one.
    CHECK(rotateStep(doc, selection, options).error == TransformError::Locked);
}

TEST_CASE("copy is a kind-preserving bijection onto fresh IDs") {
    Triangle t = rigidTriangle();
    const size_t entitiesBefore = t.doc.entities().size();
    const size_t constraintsBefore = t.doc.constraints().size();

    const CopyStep copy = copyStep(t.doc, t.all, 200.0, 0.0);
    REQUIRE(!copy.empty());
    // Everything in the closure has exactly one image, and every internal
    // relation came with it.
    CHECK(copy.entities.size() == entitiesBefore);
    CHECK(copy.constraints.size() == constraintsBefore);
    CHECK(copy.droppedConstraints == 0);

    for(const auto &[from, to] : copy.entities) {
        // Disjoint, and kind-preserving.
        CHECK(from != to);
        CHECK(copy.entities.count(to) == 0);
    }
    REQUIRE(applyAll(t.doc, copy.commands));

    for(const auto &[from, to] : copy.entities) {
        const EntityRecord *original = t.doc.entities().find(from);
        const EntityRecord *image = t.doc.entities().find(to);
        REQUIRE(original != nullptr);
        REQUIRE(image != nullptr);
        CHECK(original->kind == image->kind);
        CHECK(original->role == image->role);
        // Points moved by the offset; everything else is defined by them.
        if(image->kind == EntityKind::Point) {
            CHECK(image->seeds[0] == doctest::Approx(original->seeds[0] + 200.0));
            CHECK(image->seeds[1] == doctest::Approx(original->seeds[1]));
        }
    }
    for(const auto &[from, to] : copy.constraints) {
        const ConstraintRecord *original = t.doc.constraints().find(from);
        const ConstraintRecord *image = t.doc.constraints().find(to);
        REQUIRE(original != nullptr);
        REQUIRE(image != nullptr);
        CHECK(original->kind == image->kind);
        CHECK(original->value == image->value);
        // Every operand rebound to the image of what it named. A relation still
        // pointing at an original would make the copy a second view of the same
        // geometry rather than a copy of it.
        for(size_t i = 0; i < boundOperandCount(*original); i++) {
            CHECK(image->operands[i] == copy.entities.at(original->operands[i]));
        }
    }

    // The copy holds together on its own: no residual, nothing to re-solve.
    CHECK(worstResidual(t.doc) < 1e-9);
}

TEST_CASE("a copy drops the relations that straddle its boundary, and counts them") {
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 40.0, 0.0);
    const EntityId outside = addPoint(doc, 0.0, 90.0);
    const EntityId edge = addSegment(doc, a, b);
    addConstraint(doc, ConstraintKind::PointPointDistance, {a, b}, Slot(40.0));
    addConstraint(doc, ConstraintKind::PointPointDistance, {b, outside}, Slot(90.0));

    const std::vector<EntityId> selection{edge};
    const CopyStep copy = copyStep(doc, selection, 10.0, 10.0);
    // The internal one came; the one reaching out did not, and the drop is a
    // number the surface reports rather than a silence.
    CHECK(copy.constraints.size() == 1);
    CHECK(copy.droppedConstraints == 1);
    CHECK(copy.entities.size() == 3);  // the segment and its two ends
}

TEST_CASE("distribute expands to the primitive set a hand build would produce") {
    Document doc;
    const EntityId p0 = addPoint(doc, 0.0, 0.0);
    const EntityId p1 = addPoint(doc, 13.0, 2.0);
    const EntityId p2 = addPoint(doc, 27.0, -1.0);
    const EntityId p3 = addPoint(doc, 40.0, 0.0);
    const std::vector<EntityId> selection{p0, p1, p2, p3};

    const CompoundStep step = distributeStep(doc, selection);
    REQUIRE(step.ok());
    // One span plus three gaps; two collinearities plus two equal spacings.
    CHECK(step.entities.size() == 4);
    CHECK(step.constraints.size() == 4);
    REQUIRE(applyAll(doc, step.commands));

    // Every emitted entity is construction geometry: the mechanism is visible
    // and deletable rather than hidden.
    for(EntityId id : step.entities) {
        const EntityRecord *e = doc.entities().find(id);
        REQUIRE(e != nullptr);
        CHECK(e->kind == EntityKind::Segment);
        CHECK(e->role == Role::Construction);
    }

    size_t onLine = 0, equal = 0;
    for(ConstraintId id : step.constraints) {
        const ConstraintRecord *c = doc.constraints().find(id);
        REQUIRE(c != nullptr);
        if(c->kind == ConstraintKind::PointOnLine) onLine++;
        if(c->kind == ConstraintKind::EqualLength) equal++;
    }
    CHECK(onLine == 2);
    CHECK(equal == 2);

    // The tag names what it is about and nothing it owns.
    const TagRecord *tag = doc.tags().find(step.tag);
    REQUIRE(tag != nullptr);
    CHECK(tag->kind == TagKind::Distribution);
    CHECK(tagState(doc, *tag) == TagState::Whole);

    // And it does what it says: solving spaces them evenly along the span.
    SolveContext context = SolveContext::forWholeDocument(doc);
    SolveOptions options;
    options.diagnoseFailures = false;
    REQUIRE(solve(doc, context, options).ok());
    Pose pose(doc);
    pose.overlay(context.params());
    const Point a = *pose.point(p0);
    const Point b = *pose.point(p1);
    const Point c = *pose.point(p2);
    const Point d = *pose.point(p3);
    const double first = std::hypot(b.x - a.x, b.y - a.y);
    const double second = std::hypot(c.x - b.x, c.y - b.y);
    const double third = std::hypot(d.x - c.x, d.y - c.y);
    CHECK(second == doctest::Approx(first).epsilon(1e-6));
    CHECK(third == doctest::Approx(first).epsilon(1e-6));
}

TEST_CASE("distribute needs three things to be a rhythm over") {
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 0.0);
    const std::vector<EntityId> two{a, b};
    // Two points are always evenly distributed, so the compound declares nothing
    // and the tag would be an affordance for a shape that is not there.
    CHECK(distributeStep(doc, two).error == CompoundError::TooFew);
}

TEST_CASE("mirror is a copy plus symmetric relations plus a tag") {
    Document doc;
    const EntityId axisFrom = addPoint(doc, 0.0, -50.0);
    const EntityId axisTo = addPoint(doc, 0.0, 50.0);
    const EntityId axis = addSegment(doc, axisFrom, axisTo);
    // An axis is construction geometry, which is also how the selection tells it
    // apart from the bar it is reflecting.
    EntityRecord guide = *doc.entities().find(axis);
    guide.role = Role::Construction;
    REQUIRE(doc.apply(SetRecord<EntityRecord>{guide}).ok());

    const EntityId a = addPoint(doc, 20.0, 10.0);
    const EntityId b = addPoint(doc, 35.0, 30.0);
    const EntityId bar = addSegment(doc, a, b);

    const std::vector<EntityId> selection{axis, bar};
    CHECK(mirrorAxisIn(doc, selection) == axis);

    const CompoundStep step = mirrorStep(doc, selection, axis);
    REQUIRE(step.ok());
    CHECK(step.entities.size() == 3);     // the bar and its two ends
    CHECK(step.constraints.size() == 2);  // one per copied point
    REQUIRE(applyAll(doc, step.commands));

    for(ConstraintId id : step.constraints) {
        const ConstraintRecord *c = doc.constraints().find(id);
        REQUIRE(c != nullptr);
        CHECK(c->kind == ConstraintKind::SymmetricAboutLine);
        CHECK(c->operands[2] == axis);
    }

    // The images landed reflected, exactly, before anything solved — and the
    // relations that say so are already satisfied there.
    CHECK(worstResidual(doc) < 1e-9);

    const TagRecord *tag = doc.tags().find(step.tag);
    REQUIRE(tag != nullptr);
    CHECK(tag->kind == TagKind::Mirror);
    CHECK(tagState(doc, *tag) == TagState::Whole);
}

TEST_CASE("the mirror axis is the construction segment, or the only segment") {
    Document doc;
    const EntityId from = addPoint(doc, 0.0, -20.0);
    const EntityId to = addPoint(doc, 0.0, 20.0);
    const EntityId edge = addSegment(doc, from, to);
    const EntityId loose = addPoint(doc, 15.0, 0.0);

    // One segment and some points: no ambiguity, and the segment is the axis.
    const std::vector<EntityId> points{edge, loose};
    CHECK(mirrorAxisIn(doc, points) == edge);

    // Two ordinary segments: which is the axis is a question, and guessing is
    // the silent choice the surface exists to ask about.
    const EntityId other = addSegment(doc, loose, to);
    const std::vector<EntityId> two{edge, other};
    CHECK(!mirrorAxisIn(doc, two).valid());

    // Until one of them is a guide.
    EntityRecord guide = *doc.entities().find(edge);
    guide.role = Role::Construction;
    REQUIRE(doc.apply(SetRecord<EntityRecord>{guide}).ok());
    CHECK(mirrorAxisIn(doc, two) == edge);
}

TEST_CASE("mirror refuses an axis that is part of what it would mirror") {
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 0.0, 40.0);
    const EntityId axis = addSegment(doc, a, b);
    const EntityId c = addPoint(doc, 20.0, 20.0);
    const EntityId spoke = addSegment(doc, b, c);

    // The spoke shares an endpoint with the axis, so mirroring it would reflect
    // one of the two points the reflection is defined by.
    const std::vector<EntityId> selection{axis, spoke};
    CHECK(mirrorStep(doc, selection, axis).error == CompoundError::AxisInside);
}

TEST_CASE("dissolving a tag leaves a byte-stable remainder") {
    // Nothing is lost at dissolution because the tag never owned anything. The
    // strongest way to say that is bytes: serialize, drop the tag, serialize the
    // same document built without one, and compare.
    Document tagged;
    const EntityId a = addPoint(tagged, 0.0, 0.0);
    const EntityId b = addPoint(tagged, 40.0, 0.0);
    const EntityId c = addPoint(tagged, 40.0, 25.0);
    const EntityId first = addSegment(tagged, a, b);
    const EntityId second = addSegment(tagged, b, c);
    const ConstraintId across = addConstraint(tagged, ConstraintKind::Horizontal, {first});
    addConstraint(tagged, ConstraintKind::Vertical, {second});

    TagRecord tag;
    tag.kind = TagKind::Rectangle;
    tag.entities = {first, second};
    tag.constraints = {across};
    const uint32_t tagId = tagged.apply(AddRecord<TagRecord>{tag}).allocated;
    REQUIRE(tagId != 0);

    const std::string withTag = serialize(tagged);
    REQUIRE(tagged.apply(RemoveRecord<TagRecord>{TagId(tagId)}).ok());
    const std::string withoutTag = serialize(tagged);
    CHECK(withTag != withoutTag);

    // Everything else is exactly as it was: same entities, same relations, same
    // values, same order.
    Document plain;
    const EntityId a2 = addPoint(plain, 0.0, 0.0);
    const EntityId b2 = addPoint(plain, 40.0, 0.0);
    const EntityId c2 = addPoint(plain, 40.0, 25.0);
    const EntityId first2 = addSegment(plain, a2, b2);
    const EntityId second2 = addSegment(plain, b2, c2);
    addConstraint(plain, ConstraintKind::Horizontal, {first2});
    addConstraint(plain, ConstraintKind::Vertical, {second2});
    // The tag allocator's watermark is the one thing dissolution does not
    // rewind, and must not: IDs are never reused.
    CHECK(sameRecords(tagged, plain));
}

// ---------------------------------------------------------------------------
// Regressions from the stage 7 review
// ---------------------------------------------------------------------------

TEST_CASE("a negative scale factor is refused") {
    Triangle t = rigidTriangle();
    ScaleOptions options;
    // A negative factor is a reflection wearing a scale's clothes. Left
    // unguarded it gives circles negative radius seeds and, under
    // scale-the-values, distance constraints demanding negative lengths — a
    // document nothing but undo recovers from.
    options.factor = -1.0;
    CHECK(scaleStep(t.doc, t.all, options).error == TransformError::Degenerate);
    options.factor = -2.5;
    options.values = ValueAnswer::ScaleTheValues;
    CHECK(scaleStep(t.doc, t.all, options).error == TransformError::Degenerate);
}

TEST_CASE("a retarget frame costs no degrees of freedom") {
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 50.0, 0.0);
    const EntityId c = addPoint(doc, 50.0, 30.0);
    const EntityId across = addSegment(doc, a, b);
    const EntityId up = addSegment(doc, b, c);
    addConstraint(doc, ConstraintKind::Horizontal, {across});
    addConstraint(doc, ConstraintKind::Vertical, {up});
    const std::vector<EntityId> selection{a, b, c, across, up};

    SolveContext before = SolveContext::forWholeDocument(doc);
    SolveOptions options;
    options.diagnoseFailures = false;
    const int dofBefore = solve(doc, before, options).dof;
    REQUIRE(dofBefore >= 0);

    RotateOptions rotate;
    rotate.angle = 0.4;
    rotate.axes = AxisAnswer::RetargetToClusterFrame;
    REQUIRE(applyAll(doc, rotateStep(doc, selection, rotate).commands));

    // The frame adds four parameters and its pins remove four, so the cluster is
    // exactly as rigid as it was. A free frame would add a rotational degree of
    // freedom to everything referencing it — the answer chosen to keep the shape
    // square would be the thing that unsquared it.
    SolveContext after = SolveContext::forWholeDocument(doc);
    const SolveOutcome outcome = solve(doc, after, options);
    CHECK(outcome.ok());
    CHECK(outcome.dof == dofBefore);
}

TEST_CASE("distribute orders points along the run, not by ID") {
    // The failure this guards: draw left, then right, then middle. ID order is
    // left-right-middle, and chaining equal gaps along it declares that the
    // right-hand point is the midpoint of the other two — the solver then
    // rearranges the drawing into an order nobody selected.
    Document doc;
    const EntityId left = addPoint(doc, 0.0, 0.0);
    const EntityId right = addPoint(doc, 100.0, 0.0);
    const EntityId middle = addPoint(doc, 43.0, 5.0);
    const std::vector<EntityId> selection{left, right, middle};

    const CompoundStep step = distributeStep(doc, selection);
    REQUIRE(step.ok());
    REQUIRE(applyAll(doc, step.commands));

    SolveContext context = SolveContext::forWholeDocument(doc);
    SolveOptions options;
    options.diagnoseFailures = false;
    REQUIRE(solve(doc, context, options).ok());
    Pose pose(doc);
    pose.overlay(context.params());

    const Point l = *pose.point(left);
    const Point m = *pose.point(middle);
    const Point r = *pose.point(right);
    // The middle point stayed in the middle, and the ends stayed at the ends.
    CHECK(m.x > l.x);
    CHECK(m.x < r.x);
    CHECK(std::hypot(m.x - l.x, m.y - l.y) ==
          doctest::Approx(std::hypot(r.x - m.x, r.y - m.y)).epsilon(1e-6));
    // And nothing was dragged across the drawing to achieve it.
    CHECK(std::abs(l.x - 0.0) < 25.0);
    CHECK(std::abs(r.x - 100.0) < 25.0);
}

TEST_CASE("a broken tag is not carried through a copy") {
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 40.0, 0.0);
    const EntityId c = addPoint(doc, 40.0, 25.0);
    const EntityId first = addSegment(doc, a, b);
    const EntityId second = addSegment(doc, b, c);

    // A rectangle tag whose squaring relations are gone: the edges survive, the
    // tag does not mean anything any more, and it says so.
    TagRecord tag;
    tag.kind = TagKind::Rectangle;
    tag.entities = {first, second};
    REQUIRE(doc.apply(AddRecord<TagRecord>{tag}).ok());
    REQUIRE(tagState(doc, doc.tags().records().front()) == TagState::Broken);

    const std::vector<EntityId> selection{first, second};
    const CopyStep copy = copyStep(doc, selection, 100.0, 0.0);
    REQUIRE(!copy.empty());
    // Dropped rather than duplicated. A degraded record is what an edit leaves
    // behind; nothing has been edited here, and a copy born broken would put the
    // diagnostic on geometry the user just created.
    CHECK(copy.tags.empty());
    CHECK(copy.droppedTags == 1);

    REQUIRE(applyAll(doc, copy.commands));
    CHECK(doc.tags().size() == 1);
    CHECK(brokenTags(doc).size() == 1);
}

// ---------------------------------------------------------------------------
// Review findings 1-3: implicit references under copy and mirror
//
// Three relations carry a reference no operand walk can see: an arc's endpoint
// labels that a mirror permutes, a null-reference axis relation that means the
// document frame, and the origin-symmetric kinds that mean the world origin.
// Each survived an operation that changed what it meant.
// ---------------------------------------------------------------------------

namespace {

// An arc tangent to a segment at one of its ends, satisfied exactly at the
// seeds. `atEnd` picks the touch point: 0 is the arc's start (index 1), 1 is its
// end (index 2), recorded as the tangency's `alternative`.
struct TangentArc {
    Document doc;
    EntityId arc;
    ConstraintId tangent;
    std::vector<EntityId> subject;  // arc + segment, for a mirror
};

TangentArc tangentArc(uint8_t atEnd) {
    TangentArc t;
    const EntityId centre = addPoint(t.doc, 0.0, 0.0);
    const EntityId start = addPoint(t.doc, 10.0, 0.0);
    const EntityId end = addPoint(t.doc, 0.0, 10.0);
    t.arc = addArc(t.doc, centre, start, end);
    // The tangent line touches the chosen end perpendicular to the radius there:
    // vertical at the start (radius points +x), horizontal at the end (radius
    // points +y). The touch point lies on it, so the residual is zero at seeds.
    EntityId s0, s1;
    if(atEnd == 0) {
        s0 = addPoint(t.doc, 10.0, -8.0);
        s1 = addPoint(t.doc, 10.0, 8.0);
    } else {
        s0 = addPoint(t.doc, -8.0, 10.0);
        s1 = addPoint(t.doc, 8.0, 10.0);
    }
    const EntityId seg = addSegment(t.doc, s0, s1);
    t.tangent = addConstraint(t.doc, ConstraintKind::Tangent, {t.arc, seg});
    if(atEnd != 0) {
        ConstraintRecord r = *t.doc.constraints().find(t.tangent);
        r.alternative = 1;
        t.doc.apply(SetRecord<ConstraintRecord>{r});
    }
    t.subject = {t.arc, seg};
    return t;
}

// Two segments squared against the document frame: an L whose corner is at b.
// Nothing names a reference axis, so both relations mean the document frame —
// the meaning a reflection cannot preserve.
struct AxisCluster {
    Document doc;
    EntityId a, b, c, across, up;
    std::vector<EntityId> subject;  // the two segments
};

AxisCluster axisCluster() {
    AxisCluster k;
    k.a = addPoint(k.doc, 10.0, 0.0);
    k.b = addPoint(k.doc, 60.0, 0.0);
    k.c = addPoint(k.doc, 60.0, 30.0);
    k.across = addSegment(k.doc, k.a, k.b);
    k.up = addSegment(k.doc, k.b, k.c);
    addConstraint(k.doc, ConstraintKind::Horizontal, {k.across});
    addConstraint(k.doc, ConstraintKind::Vertical, {k.up});
    k.subject = {k.across, k.up};
    return k;
}

}  // namespace

TEST_CASE("mirror keeps a tangency at the right end of a reflected arc") {
    // copy swaps a reflected arc's endpoints — correct, since the un-swapped copy
    // is the complementary arc — but a tangency names its touch point by index,
    // so the alternative has to flip with the swap or the copy holds at the wrong
    // physical end. Both values of `alternative` are covered.
    auto mirrored = [](TangentArc &t) -> const ConstraintRecord * {
        const EntityId axisFrom = addPoint(t.doc, 30.0, -50.0);
        const EntityId axisTo = addPoint(t.doc, 30.0, 50.0);
        const EntityId axis = addSegment(t.doc, axisFrom, axisTo);
        EntityRecord guide = *t.doc.entities().find(axis);
        guide.role = Role::Construction;
        REQUIRE(t.doc.apply(SetRecord<EntityRecord>{guide}).ok());

        std::vector<EntityId> selection = t.subject;
        selection.push_back(axis);
        const CompoundStep step = mirrorStep(t.doc, selection, axis);
        REQUIRE(step.ok());
        REQUIRE(applyAll(t.doc, step.commands));

        // The copied tangency is the one that is not the original.
        for(const ConstraintRecord &c : t.doc.constraints().records()) {
            if(c.kind == ConstraintKind::Tangent && c.id != t.tangent) return &c;
        }
        return nullptr;
    };

    SUBCASE("tangent at the arc's start flips to the end form on the copy") {
        TangentArc t = tangentArc(0);
        REQUIRE(worstResidual(t.doc) < 1e-9);  // satisfied at the seeds
        const ConstraintRecord *copied = mirrored(t);
        REQUIRE(copied != nullptr);
        // The reflected start now sits at index 2, so the copy touches there.
        CHECK(copied->alternative == 1);
        const Pose pose(t.doc);
        const std::optional<double> r = residual(pose, *copied);
        REQUIRE(r.has_value());
        CHECK(std::abs(*r) < 1e-9);
        // Nothing to settle: the copied tangency already holds at the reflected
        // seeds, so the re-solve is the identity. Without the flip this residual
        // would be the arc's diameter and the re-solve would move the geometry.
        CHECK(resolveDrift(t.doc) < 1e-9);
    }

    SUBCASE("tangent at the arc's end flips to the start form on the copy") {
        TangentArc t = tangentArc(1);
        REQUIRE(worstResidual(t.doc) < 1e-9);
        const ConstraintRecord *copied = mirrored(t);
        REQUIRE(copied != nullptr);
        CHECK(copied->alternative == 0);
        const Pose pose(t.doc);
        const std::optional<double> r = residual(pose, *copied);
        REQUIRE(r.has_value());
        CHECK(std::abs(*r) < 1e-9);
        CHECK(resolveDrift(t.doc) < 1e-9);
    }
}

TEST_CASE("a reflection drops the null-reference axis relations it cannot preserve") {
    // A horizontal or vertical with no reference names the document frame, and a
    // reflection does not preserve that meaning, so it is dropped and counted —
    // uniformly, even when the mirror axis is axis-aligned and the relation is
    // only redundant. A plain translation keeps them, the control that proves the
    // drop is the reversal's doing.

    // Reflection about y = x: (x, y) -> (y, x). Orientation-reversing.
    const CopyPlacement tilted{[](Point p) { return Point{p.y, p.x}; }, true};
    // Reflection about the x-axis: (x, y) -> (x, -y). Axis-aligned, so the images
    // of the H/V edges are themselves H/V and the relation is redundant, not
    // contradictory — and is dropped anyway.
    const CopyPlacement aligned{[](Point p) { return Point{p.x, -p.y}; }, true};

    SUBCASE("about a tilted axis") {
        AxisCluster k = axisCluster();
        const CopyStep copy = copyStep(k.doc, k.subject, tilted);
        CHECK(copy.droppedConstraints == 2);
        CHECK(copy.constraints.empty());
        // The geometry still came: two segments over three points.
        CHECK(copy.entities.size() == 5);
    }

    SUBCASE("about an axis-aligned axis, where the relation is only redundant") {
        AxisCluster k = axisCluster();
        const CopyStep copy = copyStep(k.doc, k.subject, aligned);
        CHECK(copy.droppedConstraints == 2);
        CHECK(copy.constraints.empty());
    }

    SUBCASE("a translation keeps them") {
        AxisCluster k = axisCluster();
        const CopyStep copy = copyStep(k.doc, k.subject, 100.0, 0.0);
        CHECK(copy.droppedConstraints == 0);
        CHECK(copy.constraints.size() == 2);
    }
}

TEST_CASE("mirroring an axis-constrained cluster stays consistent") {
    // The whole of finding 2: without the drop, the copied horizontal contradicts
    // the symmetry that pins the image, and the entire component — original
    // included — goes Inconsistent and freezes. With it, the image is held by the
    // symmetry alone and the document solves.
    auto mirrorAbout = [](Point from, Point to) {
        AxisCluster k = axisCluster();
        const EntityId axisFrom = addPoint(k.doc, from.x, from.y);
        const EntityId axisTo = addPoint(k.doc, to.x, to.y);
        const EntityId axis = addSegment(k.doc, axisFrom, axisTo);
        EntityRecord guide = *k.doc.entities().find(axis);
        guide.role = Role::Construction;
        REQUIRE(k.doc.apply(SetRecord<EntityRecord>{guide}).ok());

        std::vector<EntityId> selection = k.subject;
        selection.push_back(axis);
        const CompoundStep step = mirrorStep(k.doc, selection, axis);
        REQUIRE(step.ok());
        REQUIRE(applyAll(k.doc, step.commands));

        // No axis relation was copied: the two in the document are the originals.
        size_t axisRelations = 0;
        for(const ConstraintRecord &c : k.doc.constraints().records()) {
            if(c.kind == ConstraintKind::Horizontal || c.kind == ConstraintKind::Vertical) {
                axisRelations++;
            }
        }
        CHECK(axisRelations == 2);
        // The image sits at the reflected pose: every symmetric relation is
        // satisfied at the seeds, so nothing is violated and the re-solve is the
        // identity. Without the drop this residual would be enormous and the
        // solve would fail.
        CHECK(worstResidual(k.doc) < 1e-9);
        CHECK(resolveDrift(k.doc) < 1e-6);
    };

    SUBCASE("about a tilted axis") { mirrorAbout(Point{-30.0, -30.0}, Point{30.0, 30.0}); }
    SUBCASE("about an axis-aligned axis") { mirrorAbout(Point{0.0, -40.0}, Point{0.0, 40.0}); }
}

TEST_CASE("a frame-referenced relation does not survive a copy") {
    // Symmetric-horizontal means the two points are mirror images about the world
    // origin's vertical axis — an absolute reference no operand carries. Copying
    // with an offset would carry it verbatim, and the copy's component being
    // consistent on its own, the solver slides the pair back toward the world
    // axis. Instead it is dropped and counted, and the copy lands where placed.
    Document doc;
    const EntityId a = addPoint(doc, -30.0, 20.0);
    const EntityId b = addPoint(doc, 30.0, 20.0);
    addConstraint(doc, ConstraintKind::SymmetricHorizontal, {a, b});
    REQUIRE(worstResidual(doc) < 1e-12);  // equal y, x summing to zero

    const std::vector<EntityId> selection{a, b};
    const CopyStep copy = copyStep(doc, selection, 100.0, 0.0);
    CHECK(copy.constraints.empty());
    CHECK(copy.droppedConstraints == 1);
    CHECK(copy.entities.size() == 2);
    REQUIRE(applyAll(doc, copy.commands));

    const EntityId copyA = copy.entities.at(a);
    const EntityId copyB = copy.entities.at(b);

    // No snap-back: the copied pair has no relation left to pull it, so the solve
    // leaves it exactly at the offset placement.
    SolveContext context = SolveContext::forWholeDocument(doc);
    SolveOptions options;
    options.diagnoseFailures = false;
    REQUIRE(solve(doc, context, options).ok());
    Pose pose(doc);
    pose.overlay(context.params());
    const Point pa = *pose.point(copyA);
    const Point pb = *pose.point(copyB);
    CHECK(pa.x == doctest::Approx(70.0));
    CHECK(pa.y == doctest::Approx(20.0));
    CHECK(pb.x == doctest::Approx(130.0));
    CHECK(pb.y == doctest::Approx(20.0));
}

TEST_CASE("a transform refuses on a frame-referenced relation") {
    // The transform half of finding 3. Symmetric-horizontal means symmetry about
    // the document frame through no operand, so a rotation or scale about any
    // centre but the world origin moves the pair out from under it and the
    // re-solve slides it back — a silent change. There is no operand to retarget,
    // so the transform refuses whole, exactly as it refuses through a lock.
    Document doc;
    const EntityId a = addPoint(doc, -30.0, 20.0);
    const EntityId b = addPoint(doc, 30.0, 20.0);
    addConstraint(doc, ConstraintKind::SymmetricHorizontal, {a, b});
    const std::vector<EntityId> selection{a, b};

    RotateOptions rotate;
    rotate.centre = Point{5.0, 5.0};
    rotate.angle = 0.5;
    const TransformStep rotated = rotateStep(doc, selection, rotate);
    CHECK(rotated.error == TransformError::FrameReferenced);
    CHECK(rotated.commands.empty());

    ScaleOptions scale;
    scale.centre = Point{5.0, 5.0};
    scale.factor = 2.0;
    const TransformStep scaled = scaleStep(doc, selection, scale);
    CHECK(scaled.error == TransformError::FrameReferenced);
    CHECK(scaled.commands.empty());
}
