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
};

// Horizontal and vertical are recorded as axis-referenced parallelism rather
// than intrinsic properties, which is what makes the rotate-a-subset question
// answerable at all. The default reference is the document frame; the reference
// entity itself arrives with cluster frames in stage 7.
inline constexpr std::array<ConstraintKindInfo, 22> CONSTRAINT_KINDS = {{
    {ConstraintKind::Coincident, "coincident", 2,
     {OperandKind::Point, OperandKind::Point}, 0, Invariance::ScaleInvariant, 100000, 1.0},
    {ConstraintKind::PointOnLine, "point-on-line", 2,
     {OperandKind::Point, OperandKind::Segment}, 0, Invariance::ScaleInvariant, 100006, 1.0},
    {ConstraintKind::PointOnCircle, "point-on-circle", 2,
     {OperandKind::Point, OperandKind::Curve}, 0, Invariance::ScaleInvariant, 100022, 1.0},
    {ConstraintKind::Midpoint, "midpoint", 2,
     {OperandKind::Point, OperandKind::Segment}, 0, Invariance::ScaleInvariant, 100018, 1.0},
    {ConstraintKind::Horizontal, "horizontal", 1,
     {OperandKind::Segment}, 0, Invariance::ScaleInvariant, 100019, 1.0},
    {ConstraintKind::Vertical, "vertical", 1,
     {OperandKind::Segment}, 0, Invariance::ScaleInvariant, 100020, 1.0},
    {ConstraintKind::Parallel, "parallel", 2,
     {OperandKind::Segment, OperandKind::Segment}, 0, Invariance::ScaleInvariant, 100025, 1.0},
    {ConstraintKind::Perpendicular, "perpendicular", 2,
     {OperandKind::Segment, OperandKind::Segment}, 0, Invariance::ScaleInvariant, 100026, 1.0},
    {ConstraintKind::Angle, "angle", 2,
     {OperandKind::Segment, OperandKind::Segment}, 1, Invariance::ScaleInvariant, 100024, 1.0},
    {ConstraintKind::EqualAngle, "equal-angle", 4,
     {OperandKind::Segment, OperandKind::Segment, OperandKind::Segment, OperandKind::Segment},
     0, Invariance::ScaleInvariant, 100012, 1.0},
    {ConstraintKind::PointPointDistance, "distance", 2,
     {OperandKind::Point, OperandKind::Point}, 1, Invariance::Absolute, 100001, 1.0},
    {ConstraintKind::PointLineDistance, "point-line-distance", 2,
     {OperandKind::Point, OperandKind::Segment}, 1, Invariance::Absolute, 100003, 1.0},
    {ConstraintKind::EqualLength, "equal-length", 2,
     {OperandKind::Segment, OperandKind::Segment}, 0, Invariance::ScaleInvariant, 100008, 1.0},
    {ConstraintKind::LengthRatio, "length-ratio", 2,
     {OperandKind::Segment, OperandKind::Segment}, 1, Invariance::ScaleInvariant, 100009, 1.0},
    {ConstraintKind::LengthDifference, "length-difference", 2,
     {OperandKind::Segment, OperandKind::Segment}, 1, Invariance::Absolute, 100033, 1.0},
    {ConstraintKind::SymmetricHorizontal, "symmetric-horizontal", 2,
     {OperandKind::Point, OperandKind::Point}, 0, Invariance::ScaleInvariant, 100015, 1.0},
    {ConstraintKind::SymmetricVertical, "symmetric-vertical", 2,
     {OperandKind::Point, OperandKind::Point}, 0, Invariance::ScaleInvariant, 100016, 1.0},
    {ConstraintKind::SymmetricAboutLine, "symmetric-about-line", 3,
     {OperandKind::Point, OperandKind::Point, OperandKind::Segment},
     0, Invariance::ScaleInvariant, 100017, 1.0},
    {ConstraintKind::Tangent, "tangent", 2,
     {OperandKind::Arc, OperandKind::Segment}, 0, Invariance::ScaleInvariant, 100027, 1.0},
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
