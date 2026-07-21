#include <doctest/doctest.h>

#include <array>
#include <set>
#include <vector>

#include "core/taxonomy.h"

using paroculus::CONSTRAINT_KINDS;
using paroculus::ConstraintKind;
using paroculus::ENTITY_KINDS;
using paroculus::EntityKind;
using paroculus::Invariance;
using paroculus::SNAP_KINDS;
using paroculus::SnapTier;
using paroculus::accepts;
using paroculus::constraintInfo;
using paroculus::constraintKindFromName;
using paroculus::entityKindFromName;
using paroculus::signatureMatches;

TEST_CASE("serialization tokens are unique and round-trip") {
    // These tokens are written to files, so a collision or a drift silently
    // rebinds records on load.
    std::set<std::string_view> seen;
    for(const auto &c : CONSTRAINT_KINDS) {
        CHECK_MESSAGE(seen.insert(c.name).second, c.name);
        CHECK(constraintKindFromName(c.name) == c.kind);
    }
    seen.clear();
    for(const auto &e : ENTITY_KINDS) {
        CHECK_MESSAGE(seen.insert(e.name).second, e.name);
        CHECK(entityKindFromName(e.name) == e.kind);
    }
    CHECK_FALSE(constraintKindFromName("not-a-constraint").has_value());
    CHECK_FALSE(entityKindFromName("not-an-entity").has_value());
}

TEST_CASE("solver types are unique across the catalogue") {
    // Two kinds mapping to one solver constant would make the failed-set
    // mapping ambiguous in both directions.
    std::set<int32_t> seen;
    for(const auto &c : CONSTRAINT_KINDS) {
        CHECK_MESSAGE(seen.insert(c.solverType).second, c.name);
    }
}

TEST_CASE("operand slots accept exactly the entity kinds they name") {
    using paroculus::OperandKind;
    CHECK(accepts(OperandKind::Point, EntityKind::Point));
    CHECK_FALSE(accepts(OperandKind::Point, EntityKind::Segment));
    CHECK(accepts(OperandKind::Segment, EntityKind::Segment));
    // Curve is the one widening slot: every constraint taking a circle takes
    // an arc, so the table says so once instead of duplicating rows.
    CHECK(accepts(OperandKind::Curve, EntityKind::Circle));
    CHECK(accepts(OperandKind::Curve, EntityKind::Arc));
    CHECK_FALSE(accepts(OperandKind::Curve, EntityKind::Segment));
    CHECK_FALSE(accepts(OperandKind::Curve, EntityKind::Point));
}

TEST_CASE("applicability agrees with the declared signature for every kind") {
    // The conformance sweep in miniature: for every constraint kind, its own
    // declared operand list must match, and a wrong-arity list must not.
    for(const auto &c : CONSTRAINT_KINDS) {
        std::vector<EntityKind> good;
        for(uint8_t i = 0; i < c.operandCount; i++) {
            switch(c.operands[i]) {
                case paroculus::OperandKind::Point:   good.push_back(EntityKind::Point); break;
                case paroculus::OperandKind::Segment: good.push_back(EntityKind::Segment); break;
                case paroculus::OperandKind::Circle:  good.push_back(EntityKind::Circle); break;
                case paroculus::OperandKind::Arc:     good.push_back(EntityKind::Arc); break;
                case paroculus::OperandKind::Curve:   good.push_back(EntityKind::Circle); break;
            }
        }
        CHECK_MESSAGE(signatureMatches(c.kind, good), c.name);

        std::vector<EntityKind> tooFew(good.begin(), good.end() - 1);
        CHECK_MESSAGE(!signatureMatches(c.kind, tooFew), c.name);

        std::vector<EntityKind> tooMany = good;
        tooMany.push_back(EntityKind::Point);
        CHECK_MESSAGE(!signatureMatches(c.kind, tooMany), c.name);
    }
}

TEST_CASE("wrong operand kinds are rejected") {
    const std::array<EntityKind, 2> twoPoints = {EntityKind::Point, EntityKind::Point};
    const std::array<EntityKind, 2> twoSegments = {EntityKind::Segment, EntityKind::Segment};

    CHECK(signatureMatches(ConstraintKind::Coincident, twoPoints));
    CHECK_FALSE(signatureMatches(ConstraintKind::Coincident, twoSegments));
    CHECK(signatureMatches(ConstraintKind::Parallel, twoSegments));
    CHECK_FALSE(signatureMatches(ConstraintKind::Parallel, twoPoints));

    // Order is significant: point-on-line is {point, segment}, not the reverse.
    const std::array<EntityKind, 2> pointThenSegment = {EntityKind::Point, EntityKind::Segment};
    const std::array<EntityKind, 2> segmentThenPoint = {EntityKind::Segment, EntityKind::Point};
    CHECK(signatureMatches(ConstraintKind::PointOnLine, pointThenSegment));
    CHECK_FALSE(signatureMatches(ConstraintKind::PointOnLine, segmentThenPoint));
}

TEST_CASE("valued constraints carry exactly one slot") {
    for(const auto &c : CONSTRAINT_KINDS) {
        CHECK_MESSAGE(c.valueArity <= 1, c.name);
    }
    CHECK(constraintInfo(ConstraintKind::PointPointDistance).valueArity == 1);
    CHECK(constraintInfo(ConstraintKind::Coincident).valueArity == 0);
}

TEST_CASE("the invariance split matches the thesis") {
    // Relational constraints survive uniform scale; absolute ones pin physical
    // size. Stage 7's scale-the-values offer reads exactly this column.
    for(ConstraintKind k : {ConstraintKind::Parallel, ConstraintKind::Perpendicular,
                            ConstraintKind::Angle, ConstraintKind::LengthRatio,
                            ConstraintKind::EqualLength, ConstraintKind::Midpoint,
                            ConstraintKind::SymmetricAboutLine, ConstraintKind::Tangent,
                            ConstraintKind::EqualRadius}) {
        CHECK(constraintInfo(k).invariance == Invariance::ScaleInvariant);
    }
    for(ConstraintKind k : {ConstraintKind::PointPointDistance,
                            ConstraintKind::PointLineDistance,
                            ConstraintKind::LengthDifference, ConstraintKind::Radius}) {
        CHECK(constraintInfo(k).invariance == Invariance::Absolute);
    }
}

TEST_CASE("exactly the origin-symmetric kinds are frame-referenced") {
    // Symmetric-horizontal and vertical constrain a pair about the world origin
    // through no operand — an absolute reference no operand walk can see — so
    // copy drops-and-counts them and transforms leave them to resist. Nothing
    // else carries the marker: a copy dropping a relation a translation actually
    // preserves would be as wrong as one keeping these.
    for(const auto &c : CONSTRAINT_KINDS) {
        const bool expected = c.kind == ConstraintKind::SymmetricHorizontal ||
                              c.kind == ConstraintKind::SymmetricVertical;
        CHECK_MESSAGE(c.frameReferenced == expected, c.name);
        // The coherence rule the table asserts: a frame-referenced kind carries
        // its reference absolutely, so it owns no optional reference slot.
        if(c.frameReferenced) CHECK_MESSAGE(c.optionalOperands == 0, c.name);
    }
}

TEST_CASE("radius declares its solver value scale") {
    // The solver constrains diameter; the document declares radius. Recording
    // the factor as data keeps the seam from growing a special case the
    // taxonomy cannot see.
    CHECK(constraintInfo(ConstraintKind::Radius).solverValueScale == 2.0);
    for(const auto &c : CONSTRAINT_KINDS) {
        if(c.kind != ConstraintKind::Radius) {
            CHECK_MESSAGE(c.solverValueScale == 1.0, c.name);
        }
    }
}

TEST_CASE("snap kinds commit constraints from the same catalogue") {
    // A snap is a constraint candidate, not a coordinate correction. Every
    // committing snap must name a constraint that actually exists.
    for(const auto &s : SNAP_KINDS) {
        if(!s.commitsConstraint) continue;
        CHECK_MESSAGE(constraintKindFromName(constraintInfo(s.constraint).name) == s.constraint,
                      s.name);
    }
}

TEST_CASE("only the strongest snaps auto-commit") {
    // Helpful rigidity is its own failure mode; grid never generates a
    // constraint at all.
    using paroculus::SnapKind;
    using paroculus::snapInfo;
    CHECK(snapInfo(SnapKind::Endpoint).tier == SnapTier::AutoCommit);
    CHECK(snapInfo(SnapKind::Horizontal).tier == SnapTier::AutoCommit);
    CHECK(snapInfo(SnapKind::Vertical).tier == SnapTier::AutoCommit);
    CHECK(snapInfo(SnapKind::Parallel).tier == SnapTier::Offered);
    CHECK(snapInfo(SnapKind::Grid).tier == SnapTier::PlacementOnly);
    CHECK_FALSE(snapInfo(SnapKind::Grid).commitsConstraint);
}

TEST_CASE("entity kinds declare their defining points") {
    using paroculus::entityInfo;
    CHECK(entityInfo(EntityKind::Point).pointCount == 0);
    CHECK(entityInfo(EntityKind::Segment).pointCount == 2);
    CHECK(entityInfo(EntityKind::Circle).pointCount == 1);
    CHECK(entityInfo(EntityKind::Arc).pointCount == 3);
    // Owned parameters are the ones carrying seeds: a point's coordinates and
    // a circle's radius. A segment borrows everything from its endpoints.
    CHECK(entityInfo(EntityKind::Point).ownParamCount == 2);
    CHECK(entityInfo(EntityKind::Segment).ownParamCount == 0);
    CHECK(entityInfo(EntityKind::Circle).ownParamCount == 1);
    CHECK(entityInfo(EntityKind::Arc).ownParamCount == 0);
    for(const auto &e : ENTITY_KINDS) {
        CHECK_MESSAGE(e.pointCount <= paroculus::MAX_ENTITY_POINTS, e.name);
        CHECK_MESSAGE(e.ownParamCount <= paroculus::MAX_ENTITY_PARAMS, e.name);
    }
    CHECK_FALSE(entityInfo(EntityKind::Point).boundaryCapable);
    CHECK(entityInfo(EntityKind::Segment).boundaryCapable);
}
