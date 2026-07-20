#include "core/measure.h"

#include <cmath>

#include <Eigen/Dense>

namespace paroculus {
namespace {

constexpr double DEGREES = 180.0 / M_PI;
// Below this a segment has no direction and a ratio no denominator. Absolute
// because document units are nominally millimetres and a segment this short is
// not geometry anyone placed.
constexpr double DEGENERATE = 1e-12;

Eigen::Vector2d vec(Point p) { return {p.x, p.y}; }

std::optional<Eigen::Vector2d> pointAt(const Pose &pose, EntityId id) {
    const std::optional<Point> p = pose.point(id);
    if(!p) return std::nullopt;
    return vec(*p);
}

// A segment's direction, unnormalised. Absent for a missing or degenerate one.
std::optional<Eigen::Vector2d> directionOf(const Pose &pose, EntityId segment) {
    const auto ends = pose.segment(segment);
    if(!ends) return std::nullopt;
    const Eigen::Vector2d d = vec(ends->second) - vec(ends->first);
    if(d.norm() < DEGENERATE) return std::nullopt;
    return d;
}

std::optional<double> lengthOf(const Pose &pose, EntityId segment) {
    const auto ends = pose.segment(segment);
    if(!ends) return std::nullopt;
    return (vec(ends->second) - vec(ends->first)).norm();
}

// The unsigned angle between two directions, in [0, 180] degrees.
double degreesBetween(const Eigen::Vector2d &a, const Eigen::Vector2d &b) {
    const double cosine = a.dot(b) / (a.norm() * b.norm());
    return std::acos(std::clamp(cosine, -1.0, 1.0)) * DEGREES;
}

// The same, folded into [0, 90]: a direction and its reverse are the same
// direction as far as parallelism is concerned, so an antiparallel pair is
// parallel and reads zero rather than 180.
double foldedDegrees(const Eigen::Vector2d &a, const Eigen::Vector2d &b) {
    const double d = degreesBetween(a, b);
    return std::min(d, 180.0 - d);
}

double distanceToLine(const Eigen::Vector2d &p, const Eigen::Vector2d &a,
                      const Eigen::Vector2d &b) {
    const Eigen::Vector2d ab = b - a;
    const double len = ab.norm();
    if(len < DEGENERATE) return (p - a).norm();
    return std::fabs(ab.x() * (p.y() - a.y()) - ab.y() * (p.x() - a.x())) / len;
}

// The axis a horizontal or vertical constraint is measured against. Null names
// the document frame, which is what an unreferenced horizontal has always
// meant; naming a segment makes the reference that segment's direction.
std::optional<Eigen::Vector2d> referenceAxis(const Pose &pose, EntityId reference) {
    if(!reference.valid()) return Eigen::Vector2d(1.0, 0.0);
    return directionOf(pose, reference);
}

std::optional<double> measureValued(const Pose &pose, ConstraintKind kind,
                                    std::span<const EntityId> o) {
    switch(kind) {
        case ConstraintKind::Angle: {
            const auto a = directionOf(pose, o[0]);
            const auto b = directionOf(pose, o[1]);
            if(!a || !b) return std::nullopt;
            return degreesBetween(*a, *b);
        }
        case ConstraintKind::PointPointDistance: {
            const auto a = pointAt(pose, o[0]);
            const auto b = pointAt(pose, o[1]);
            if(!a || !b) return std::nullopt;
            return (*b - *a).norm();
        }
        case ConstraintKind::PointLineDistance: {
            const auto p = pointAt(pose, o[0]);
            const auto ends = pose.segment(o[1]);
            if(!p || !ends) return std::nullopt;
            return distanceToLine(*p, vec(ends->first), vec(ends->second));
        }
        case ConstraintKind::LengthRatio: {
            const auto a = lengthOf(pose, o[0]);
            const auto b = lengthOf(pose, o[1]);
            if(!a || !b || *b < DEGENERATE) return std::nullopt;
            return *a / *b;
        }
        case ConstraintKind::LengthDifference: {
            const auto a = lengthOf(pose, o[0]);
            const auto b = lengthOf(pose, o[1]);
            if(!a || !b) return std::nullopt;
            return *a - *b;
        }
        case ConstraintKind::Radius:
            return pose.curveRadius(o[0]);
        default:
            return std::nullopt;
    }
}

}  // namespace

std::optional<double> measure(const Pose &pose, ConstraintKind kind,
                              std::span<const EntityId> operands) {
    const ConstraintKindInfo &info = constraintInfo(kind);
    if(info.valueArity == 0) return std::nullopt;
    if(operands.size() < info.operandCount) return std::nullopt;
    return measureValued(pose, kind, operands);
}

std::optional<double> measure(const Pose &pose, const ConstraintRecord &constraint) {
    return measure(pose, constraint.kind, constraint.operands);
}

std::optional<double> residual(const Pose &pose, const ConstraintRecord &r) {
    const std::span<const EntityId> o = r.operands;
    const ConstraintKindInfo &info = constraintInfo(r.kind);

    // A valued kind's residual is how far the measurement is from what the slot
    // says, so the value has to be readable before anything else is worth
    // computing. Slots that name a parameter evaluate against the document the
    // pose is over, which is the same one the solver would read.
    std::optional<double> declared;
    if(info.valueArity == 1) {
        declared = pose.document().evaluate(r.value);
        if(!declared) return std::nullopt;
    }

    switch(r.kind) {
        case ConstraintKind::Coincident: {
            const auto a = pointAt(pose, o[0]);
            const auto b = pointAt(pose, o[1]);
            if(!a || !b) return std::nullopt;
            return (*b - *a).norm();
        }
        case ConstraintKind::PointOnLine: {
            const auto p = pointAt(pose, o[0]);
            const auto ends = pose.segment(o[1]);
            if(!p || !ends) return std::nullopt;
            return distanceToLine(*p, vec(ends->first), vec(ends->second));
        }
        case ConstraintKind::PointOnCircle: {
            const auto p = pointAt(pose, o[0]);
            const auto centre = pose.curveCentre(o[1]);
            const auto radius = pose.curveRadius(o[1]);
            if(!p || !centre || !radius) return std::nullopt;
            return std::fabs((*p - vec(*centre)).norm() - *radius);
        }
        case ConstraintKind::Midpoint: {
            const auto p = pointAt(pose, o[0]);
            const auto ends = pose.segment(o[1]);
            if(!p || !ends) return std::nullopt;
            return (*p - 0.5 * (vec(ends->first) + vec(ends->second))).norm();
        }
        case ConstraintKind::Horizontal: {
            const auto d = directionOf(pose, o[0]);
            const auto axis = referenceAxis(pose, o[1]);
            if(!d || !axis) return std::nullopt;
            return foldedDegrees(*d, *axis);
        }
        case ConstraintKind::Vertical: {
            const auto d = directionOf(pose, o[0]);
            const auto axis = referenceAxis(pose, o[1]);
            if(!d || !axis) return std::nullopt;
            return std::fabs(90.0 - foldedDegrees(*d, *axis));
        }
        case ConstraintKind::Parallel: {
            const auto a = directionOf(pose, o[0]);
            const auto b = directionOf(pose, o[1]);
            if(!a || !b) return std::nullopt;
            return foldedDegrees(*a, *b);
        }
        case ConstraintKind::Perpendicular: {
            const auto a = directionOf(pose, o[0]);
            const auto b = directionOf(pose, o[1]);
            if(!a || !b) return std::nullopt;
            return std::fabs(90.0 - foldedDegrees(*a, *b));
        }
        case ConstraintKind::Angle: {
            const auto a = directionOf(pose, o[0]);
            const auto b = directionOf(pose, o[1]);
            if(!a || !b) return std::nullopt;
            // The solver constrains the direction cosine, so either the angle
            // or its supplement satisfies the declaration.
            const double actual = degreesBetween(*a, *b);
            return std::min(std::fabs(actual - *declared),
                            std::fabs(180.0 - actual - *declared));
        }
        case ConstraintKind::EqualAngle: {
            const auto a = directionOf(pose, o[0]);
            const auto b = directionOf(pose, o[1]);
            const auto c = directionOf(pose, o[2]);
            const auto d = directionOf(pose, o[3]);
            if(!a || !b || !c || !d) return std::nullopt;
            return std::fabs(degreesBetween(*a, *b) - degreesBetween(*c, *d));
        }
        case ConstraintKind::PointPointDistance:
        case ConstraintKind::PointLineDistance:
        case ConstraintKind::LengthRatio:
        case ConstraintKind::LengthDifference:
        case ConstraintKind::Radius: {
            const std::optional<double> actual = measureValued(pose, r.kind, o);
            if(!actual) return std::nullopt;
            return std::fabs(*actual - *declared);
        }
        case ConstraintKind::EqualLength: {
            const auto a = lengthOf(pose, o[0]);
            const auto b = lengthOf(pose, o[1]);
            if(!a || !b) return std::nullopt;
            return std::fabs(*a - *b);
        }
        case ConstraintKind::SymmetricHorizontal: {
            const auto a = pointAt(pose, o[0]);
            const auto b = pointAt(pose, o[1]);
            if(!a || !b) return std::nullopt;
            return std::fabs(a->y() - b->y()) + std::fabs(a->x() + b->x());
        }
        case ConstraintKind::SymmetricVertical: {
            const auto a = pointAt(pose, o[0]);
            const auto b = pointAt(pose, o[1]);
            if(!a || !b) return std::nullopt;
            return std::fabs(a->x() - b->x()) + std::fabs(a->y() + b->y());
        }
        case ConstraintKind::SymmetricAboutLine: {
            const auto p = pointAt(pose, o[0]);
            const auto q = pointAt(pose, o[1]);
            const auto ends = pose.segment(o[2]);
            if(!p || !q || !ends) return std::nullopt;
            const Eigen::Vector2d la = vec(ends->first);
            const Eigen::Vector2d ab = (vec(ends->second) - la);
            if(ab.norm() < DEGENERATE) return std::nullopt;
            const Eigen::Vector2d axis = ab.normalized();
            const Eigen::Vector2d mid = 0.5 * (*p + *q);
            // Equidistant from the line, and their midpoint on it.
            const double offLine =
                std::fabs(axis.x() * (mid.y() - la.y()) - axis.y() * (mid.x() - la.x()));
            const Eigen::Vector2d across = *q - *p;
            if(across.norm() < DEGENERATE) return offLine;
            return offLine + std::fabs(across.normalized().dot(axis));
        }
        case ConstraintKind::Tangent: {
            // The solver's tangency is the line being perpendicular to the
            // radius at the chosen arc endpoint. `alternative` says which end,
            // and reading it is the whole reason it is recorded.
            const EntityRecord *arc = pose.document().entities().find(o[0]);
            if(arc == nullptr || arc->kind != EntityKind::Arc) return std::nullopt;
            const auto centre = pointAt(pose, arc->points[0]);
            const auto touch = pointAt(pose, arc->points[r.alternative == 0 ? 1 : 2]);
            const auto dir = directionOf(pose, o[1]);
            if(!centre || !touch || !dir) return std::nullopt;
            return std::fabs(dir->normalized().dot(*centre - *touch));
        }
        case ConstraintKind::EqualRadius: {
            const auto a = pose.curveRadius(o[0]);
            const auto b = pose.curveRadius(o[1]);
            if(!a || !b) return std::nullopt;
            return std::fabs(*a - *b);
        }
        case ConstraintKind::Pin:
            // A pin holds a point where it sits, so it is satisfied wherever
            // the point is. What it costs is a degree of freedom, not a
            // residual, and reporting anything else would make imposing one
            // look like it moved something.
            return pointAt(pose, o[0]) ? std::optional<double>(0.0) : std::nullopt;
    }
    return std::nullopt;
}

std::optional<double> parallelGapDegrees(const Pose &pose, EntityId a, EntityId b) {
    const auto da = directionOf(pose, a);
    const auto db = directionOf(pose, b);
    if(!da || !db) return std::nullopt;
    return foldedDegrees(*da, *db);
}

}  // namespace paroculus
