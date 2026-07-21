// The taxonomy spine: one list, five projections.
//
// Primitives, constraints, snap candidates, applicability predicates and action
// metadata share this schema. Adding a primitive or a relation is a data change
// here that updates drawing, snapping, the action surface, validation and the
// test harness together — or it is five drifting copies. This is the deepest
// structural bet in the codebase after the solver itself.
//
// Mechanism: constexpr tables, not x-macros. The requirement is one list and
// five projections, and plain data plus functions over it gives that while
// staying navigable by clangd and legible in a debugger. Codegen stays off the
// table until three consumers prove it necessary.
//
// The tables carry a solver constant per constraint kind. That is deliberate:
// the alternative is a switch in solve/, which is a sixth projection free to
// drift. solve/ static_asserts every value against its SLVS_C_*, so a solver
// renumbering breaks the build rather than mislabelling geometry.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace paroculus {

// ---------------------------------------------------------------------------
// Entities
// ---------------------------------------------------------------------------

enum class EntityKind : uint8_t { Point, Segment, Circle, Arc };

// Presentation flag on ordinary geometry, never a distinct type. Construction
// geometry participates in constraints identically, is excluded from regions
// and export, and sits at reduced hit priority.
enum class Role : uint8_t { Normal, Construction };

// The most an entity refers to: an arc's centre plus two endpoints.
inline constexpr size_t MAX_ENTITY_POINTS = 3;
// The most solver parameters an entity owns directly: a point's x and y.
inline constexpr size_t MAX_ENTITY_PARAMS = 2;

struct EntityKindInfo {
    EntityKind kind;
    std::string_view name;  // serialization token, stable across versions
    // Defining points this entity refers to: segment has 2 endpoints, circle
    // has a centre, arc has centre plus two endpoints.
    uint8_t pointCount;
    // Solver parameters the entity owns rather than borrowing from its points:
    // a point owns x and y, a circle owns its radius, and a segment owns
    // nothing because its endpoints carry everything. Each owned parameter has
    // a seed, which is why this count and not a boolean.
    uint8_t ownParamCount;
    // Whether the kind can appear as an edge in a region boundary cycle.
    bool boundaryCapable;
};

inline constexpr std::array<EntityKindInfo, 4> ENTITY_KINDS = {{
    {EntityKind::Point, "point", 0, 2, false},
    {EntityKind::Segment, "segment", 2, 0, true},
    {EntityKind::Circle, "circle", 1, 1, true},
    {EntityKind::Arc, "arc", 3, 0, true},
}};

constexpr const EntityKindInfo &entityInfo(EntityKind k) {
    return ENTITY_KINDS[static_cast<size_t>(k)];
}

std::optional<EntityKind> entityKindFromName(std::string_view name);

// ---------------------------------------------------------------------------
// Constraints
// ---------------------------------------------------------------------------

// What an operand slot will accept. Curve admits circle or arc, because every
// constraint that takes one takes the other.
enum class OperandKind : uint8_t { Point, Segment, Circle, Arc, Curve };

// Whether the constraint survives a uniform scale unchanged. This asymmetry is
// the project thesis vindicated: a proportion-built document rescales cleanly
// while absolute dimensions pin physical size, and the two families are
// visually distinguishable because the table says which is which.
enum class Invariance : uint8_t { ScaleInvariant, Absolute };

enum class ConstraintKind : uint8_t {
    Coincident,
    PointOnLine,
    PointOnCircle,
    Midpoint,
    Horizontal,
    Vertical,
    Parallel,
    Perpendicular,
    Angle,
    EqualAngle,
    PointPointDistance,
    PointLineDistance,
    EqualLength,
    LengthRatio,
    LengthDifference,
    SymmetricHorizontal,
    SymmetricVertical,
    SymmetricAboutLine,
    Tangent,
    Radius,
    EqualRadius,
    Pin,
};

inline constexpr size_t MAX_OPERANDS = 4;

struct ConstraintKindInfo {
    ConstraintKind kind;
    std::string_view name;  // serialization token, stable across versions
    uint8_t operandCount;
    std::array<OperandKind, MAX_OPERANDS> operands;
    // 0 or 1 in v1. A valued constraint carries exactly one slot.
    uint8_t valueArity;
    Invariance invariance;
    // SLVS_C_*, verified against slvs.h by static_assert in solve/.
    int32_t solverType;
    // Radius is declared here but the solver speaks diameter, so the value
    // needs doubling at the seam. Recorded as data so the seam does not grow a
    // special case the taxonomy cannot see.
    double solverValueScale;

    // Trailing operands the kind may carry but does not require, and what the
    // solver calls the kind when they are present. Both zero for every kind
    // that has neither, which is every kind but two.
    //
    // v1 uses this for exactly one thing. Horizontal and vertical are
    // axis-referenced parallelism, and the reference is nullable with the
    // document frame as the default — so an unreferenced horizontal is the
    // ordinary one and names nothing extra, while naming a reference axis makes
    // horizontal mean parallel to it and vertical mean perpendicular to it. The
    // same declaration, a different solver primitive, and the difference is data
    // rather than a branch in the translation.
    //
    // Required operands come first and are never null; optional ones follow and
    // may be. Applicability is decided over the required prefix alone, which is
    // why selecting one segment still offers horizontal.
    uint8_t optionalOperands;
    int32_t solverTypeReferenced;

    // How many alternative forms the kind has beyond its default one. Zero for
    // every kind whose operands say everything about it, and one for the three
    // that leave a choice the operands cannot express: tangency holds at one
    // end of an arc or the other, and an angle — plain or equal — is either the
    // angle between the directions as drawn or its supplement. The solver reads
    // the choice as Slvs_Constraint.other and applies it the same way in all
    // three, by reversing the first direction or picking the far endpoint.
    uint8_t alternatives;

    // Whether swapping two operands the kind accepts in either slot changes
    // what it says. False for almost everything: a coincidence between A and B
    // is the coincidence between B and A, and offering both would be offering
    // the same relation twice.
    //
    // True for the two kinds that read one operand against the other —
    // len(A)/len(B) and len(A)-len(B) — which is precisely the role ambiguity
    // PRINCIPLES sends to the surface rather than resolving in prose. The
    // surface asks which way and previews the answer; this column is how it
    // knows there is a question. A kind whose slots take different entity kinds
    // has its roles assigned by type and needs no flag either way.
    bool orderSensitive;

    // How many consecutive slots form one interchangeable group, or zero for a
    // kind whose slots do not group.
    //
    // Order sensitivity is the right question for two slots and the wrong shape
    // for four. Equal-angle relates angle(A,B) to angle(C,D): swapping within
    // either pair says nothing new, and swapping the pairs says nothing new
    // either, because equality is symmetric — but which segments are paired
    // with which is three different declarations, and the operands cannot say.
    // That is the same ambiguity length-ratio gets a surface for, on a kind
    // where the wrong reading is much harder to see, so the surface has to
    // enumerate the groupings and ask.
    //
    // Grouped kinds have all their required slots accepting one operand kind —
    // otherwise the grouping would be decided by type and there would be
    // nothing to ask — and the groups are interchangeable with each other, so a
    // reading is canonical when each group is in ascending ID order and the
    // groups themselves are.
    uint8_t operandGroupSize;

    // Whether the kind's meaning references the document frame or world origin
    // through no operand at all — an absolute reference no operand walk can see.
    //
    // True for exactly the two origin-symmetric kinds: symmetric-horizontal and
    // symmetric-vertical constrain a pair about the workplane axis through the
    // world origin (the solver's au+bu=0). A copy or an off-origin transform
    // reasoning over operands alone would carry them verbatim and let the pair
    // slide back toward the world axis — so copy drops-and-counts them on every
    // copy, because the world frame is outside every copied set, and transforms
    // leave them to resist, which is the default answer that rewrites nothing.
    //
    // Distinct from the nullable reference horizontal and vertical carry: those
    // mean the document frame too, but through an optional operand slot that
    // rotate can retarget, so they are a question with an answer rather than an
    // absolute. A frame-referenced kind therefore carries no optional operand.
    bool frameReferenced;
};

// The three strengths every relation the tool can compute exists at.
//
// One semantic, not three scattered features: align-left, distribute, measure
// distance and dimension are the same relation presented at different
// strengths, and the action surface offers them as one action with a choice
// rather than as three menu entries that happen to be related.
//
// Measure is the one that records nothing. It solves with the relation in force
// and keeps the geometry that comes out, then throws the relation away — which
// is exactly "align these now, remember nothing", and is why it needs no record
// type of its own.
enum class Strength : uint8_t {
    Measure,    // apply it once and record nothing
    Impose,     // a driving constraint: the geometry must hold it
    Reference,  // a driven measurement: recorded, displayed, never driving
};

// Stable token, used by action names and therefore format.
std::string_view strengthName(Strength s);

// The CAD-surface grouping of the imposition rows, for the constraints toolbar.
// Presentation rather than solver semantics — it changes nothing about what a
// constraint means — but data in the taxonomy all the same, so the toolbar is a
// projection of one table rather than a shell-side list that drifts from the
// catalogue, exactly as the registry's category column keeps the menus. The six
// families are the ones the spec's constraints toolbar fixes.
enum class ConstraintFamily : uint8_t {
    Placement,
    Direction,
    Size,
    Symmetry,
    Curve,
    Anchor,
};

// A stable lowercase token, for the surfaces that group by it.
std::string_view familyName(ConstraintFamily family);

// Which family a kind belongs to. The coverage test in core_taxonomy pins every
// kind to a family and to the spec's per-family counts, so adding a kind without
// placing it is a red test — the build itself does not warn, since the targets
// carry no -Wswitch and the trailing return keeps an unplaced kind compiling.
ConstraintFamily constraintFamily(ConstraintKind kind);

// Horizontal and vertical are recorded as axis-referenced parallelism rather
// than intrinsic properties, which is what makes the rotate-a-subset question
// answerable at all. The reference is the nullable operand and the default is
// the document frame. What stage 7 adds is the cluster frames worth pointing at
// and the retarget flow that offers them, not a change to this signature.
inline constexpr std::array<ConstraintKindInfo, 22> CONSTRAINT_KINDS = {{
    {ConstraintKind::Coincident, "coincident", 2,
     {OperandKind::Point, OperandKind::Point}, 0, Invariance::ScaleInvariant, 100000, 1.0},
    {ConstraintKind::PointOnLine, "point-on-line", 2,
     {OperandKind::Point, OperandKind::Segment}, 0, Invariance::ScaleInvariant, 100006, 1.0},
    {ConstraintKind::PointOnCircle, "point-on-circle", 2,
     {OperandKind::Point, OperandKind::Curve}, 0, Invariance::ScaleInvariant, 100022, 1.0},
    {ConstraintKind::Midpoint, "midpoint", 2,
     {OperandKind::Point, OperandKind::Segment}, 0, Invariance::ScaleInvariant, 100018, 1.0},
    // One required segment and one nullable reference axis. Null is the
    // document frame, which is what an unreferenced horizontal has always meant
    // and still means; the second slot is what makes rotate-a-subset answerable
    // rather than a question the format cannot hold.
    {ConstraintKind::Horizontal, "horizontal", 1,
     {OperandKind::Segment, OperandKind::Segment}, 0, Invariance::ScaleInvariant, 100019, 1.0,
     1, 100025},
    {ConstraintKind::Vertical, "vertical", 1,
     {OperandKind::Segment, OperandKind::Segment}, 0, Invariance::ScaleInvariant, 100020, 1.0,
     1, 100026},
    {ConstraintKind::Parallel, "parallel", 2,
     {OperandKind::Segment, OperandKind::Segment}, 0, Invariance::ScaleInvariant, 100025, 1.0},
    {ConstraintKind::Perpendicular, "perpendicular", 2,
     {OperandKind::Segment, OperandKind::Segment}, 0, Invariance::ScaleInvariant, 100026, 1.0},
    // Two forms, exactly as tangency has: the angle between the segments as
    // drawn, or its supplement. The solver reads the choice as
    // Slvs_Constraint.other and applies it by reversing the first direction, so
    // without a column to say which the supplementary form is not merely unused
    // but unsayable — and stage 7's rotate, which reverses directions wholesale,
    // is where a document needs to be able to say it.
    {ConstraintKind::Angle, "angle", 2,
     {OperandKind::Segment, OperandKind::Segment}, 1, Invariance::ScaleInvariant, 100024, 1.0,
     0, 0, 1},
    // Four segments in two pairs, and which pairing was meant is a question the
    // operands cannot answer — angle(A,B) = angle(C,D) is not angle(A,C) =
    // angle(B,D). The group size is how the surface knows to ask.
    {ConstraintKind::EqualAngle, "equal-angle", 4,
     {OperandKind::Segment, OperandKind::Segment, OperandKind::Segment, OperandKind::Segment},
     0, Invariance::ScaleInvariant, 100012, 1.0, 0, 0, 1, false, 2},
    {ConstraintKind::PointPointDistance, "distance", 2,
     {OperandKind::Point, OperandKind::Point}, 1, Invariance::Absolute, 100001, 1.0},
    {ConstraintKind::PointLineDistance, "point-line-distance", 2,
     {OperandKind::Point, OperandKind::Segment}, 1, Invariance::Absolute, 100003, 1.0},
    {ConstraintKind::EqualLength, "equal-length", 2,
     {OperandKind::Segment, OperandKind::Segment}, 0, Invariance::ScaleInvariant, 100008, 1.0},
    // The two that read one operand against the other, and so the two the
    // surface has to ask about: len(A)/len(B) is not len(B)/len(A).
    {ConstraintKind::LengthRatio, "length-ratio", 2,
     {OperandKind::Segment, OperandKind::Segment}, 1, Invariance::ScaleInvariant, 100009, 1.0,
     0, 0, 0, true},
    {ConstraintKind::LengthDifference, "length-difference", 2,
     {OperandKind::Segment, OperandKind::Segment}, 1, Invariance::Absolute, 100033, 1.0,
     0, 0, 0, true},
    // Frame-referenced: symmetric about the world origin's axis through no
    // operand, so a copy or an off-origin transform reading operands alone would
    // slide the pair back toward it. The trailing `true` is the marker; the
    // zeros before it are the optional/alternative/group columns these kinds do
    // not use, spelled out because the marker sits after them.
    {ConstraintKind::SymmetricHorizontal, "symmetric-horizontal", 2,
     {OperandKind::Point, OperandKind::Point}, 0, Invariance::ScaleInvariant, 100015, 1.0,
     0, 0, 0, false, 0, true},
    {ConstraintKind::SymmetricVertical, "symmetric-vertical", 2,
     {OperandKind::Point, OperandKind::Point}, 0, Invariance::ScaleInvariant, 100016, 1.0,
     0, 0, 0, false, 0, true},
    {ConstraintKind::SymmetricAboutLine, "symmetric-about-line", 3,
     {OperandKind::Point, OperandKind::Point, OperandKind::Segment},
     0, Invariance::ScaleInvariant, 100017, 1.0},
    // Two forms: tangent at the arc's start, or at its end. Both are the same
    // relation over the same pair, so this is one row with a choice rather than
    // two rows a surface would have to offer side by side.
    {ConstraintKind::Tangent, "tangent", 2,
     {OperandKind::Arc, OperandKind::Segment}, 0, Invariance::ScaleInvariant, 100027, 1.0,
     0, 0, 1},
    // The solver constrains diameter; the document declares radius.
    {ConstraintKind::Radius, "radius", 1,
     {OperandKind::Curve}, 1, Invariance::Absolute, 100021, 2.0},
    {ConstraintKind::EqualRadius, "equal-radius", 2,
     {OperandKind::Curve, OperandKind::Curve}, 0, Invariance::ScaleInvariant, 100029, 1.0},
    {ConstraintKind::Pin, "pin", 1,
     {OperandKind::Point}, 0, Invariance::ScaleInvariant, 100031, 1.0},
}};

constexpr const ConstraintKindInfo &constraintInfo(ConstraintKind k) {
    return CONSTRAINT_KINDS[static_cast<size_t>(k)];
}

std::optional<ConstraintKind> constraintKindFromName(std::string_view name);

// entity: what the operand actually is. slot: what the kind will accept.
// Returns whether the entity can fill that slot.
constexpr bool accepts(OperandKind slot, EntityKind entity) {
    switch(slot) {
        case OperandKind::Point:   return entity == EntityKind::Point;
        case OperandKind::Segment: return entity == EntityKind::Segment;
        case OperandKind::Circle:  return entity == EntityKind::Circle;
        case OperandKind::Arc:     return entity == EntityKind::Arc;
        case OperandKind::Curve:   return entity == EntityKind::Circle || entity == EntityKind::Arc;
    }
    return false;
}

// The applicability projection, and the single source the action surface and
// solver-side validation both read. Order is significant: operands are
// positional, and role ambiguity is resolved in the surface, not here.
// kinds: the operand entity kinds, in order.
// Returns whether this constraint kind admits exactly that signature.
bool signatureMatches(ConstraintKind k, std::span<const EntityKind> kinds);

// ---------------------------------------------------------------------------
// Snap candidates
// ---------------------------------------------------------------------------

// A snap is not a coordinate correction; it is a constraint candidate that
// placement commits. The kinds are declared now, with the constraint each
// commits, so the snap engine of stage 4 is born sharing this taxonomy instead
// of being a geometric corrector with inference bolted on later.
enum class SnapKind : uint8_t {
    Endpoint,
    OnLine,
    Midpoint,
    Horizontal,
    Vertical,
    Parallel,
    Perpendicular,
    OnCircle,
    Grid,
};

// Whether a candidate commits on placement or is offered for confirmation.
// Only the strongest tier auto-commits; helpful rigidity is its own failure
// mode. The tiering is a policy that stage 4 may retune, but the two-tier
// shape is fixed here because the commit rule is taxonomy, not feel.
enum class SnapTier : uint8_t { AutoCommit, Offered, PlacementOnly };

struct SnapKindInfo {
    SnapKind kind;
    std::string_view name;
    SnapTier tier;
    // The constraint this candidate commits. Absent for PlacementOnly kinds:
    // grid snap is a placement aid and generates no constraint, because a
    // document where every point is grid-pinned is rigidity by helpfulness.
    bool commitsConstraint;
    ConstraintKind constraint;
};

inline constexpr std::array<SnapKindInfo, 9> SNAP_KINDS = {{
    {SnapKind::Endpoint, "endpoint", SnapTier::AutoCommit, true, ConstraintKind::Coincident},
    {SnapKind::OnLine, "on-line", SnapTier::Offered, true, ConstraintKind::PointOnLine},
    {SnapKind::Midpoint, "midpoint", SnapTier::Offered, true, ConstraintKind::Midpoint},
    {SnapKind::Horizontal, "horizontal", SnapTier::AutoCommit, true, ConstraintKind::Horizontal},
    {SnapKind::Vertical, "vertical", SnapTier::AutoCommit, true, ConstraintKind::Vertical},
    {SnapKind::Parallel, "parallel", SnapTier::Offered, true, ConstraintKind::Parallel},
    {SnapKind::Perpendicular, "perpendicular", SnapTier::Offered, true,
     ConstraintKind::Perpendicular},
    {SnapKind::OnCircle, "on-circle", SnapTier::Offered, true, ConstraintKind::PointOnCircle},
    {SnapKind::Grid, "grid", SnapTier::PlacementOnly, false, ConstraintKind::Coincident},
}};

constexpr const SnapKindInfo &snapInfo(SnapKind k) {
    return SNAP_KINDS[static_cast<size_t>(k)];
}

}  // namespace paroculus
