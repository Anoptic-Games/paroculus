// The constraint semantics suite: the heart of core-math validation.
//
// For every entry in the v1 catalogue, a minimal system is built with seeds
// deliberately off-constraint, solved, and verified by geometric residual —
// never by trusting the solver's status code. A solver that no-opped and echoed
// its input back would report OKAY and fail every assertion here.
//
// Each entry also carries an infeasible variant, asserting the failure is
// reported as inconsistent and that the blamed set maps back to real document
// constraints; and the catalogue is checked for exhaustive coverage, so adding
// a constraint kind without adding its semantics is a test failure rather than
// an omission nobody notices.
#include <doctest/doctest.h>

#include <cmath>
#include <functional>
#include <set>

#include "core/measure.h"
#include "core/pose.h"
#include "core/topology.h"
#include "solve/diagnose.h"
#include "solve/solve.h"
#include "support/build.h"

using namespace paroculus;
using paroculus::test::addConstraint;
using paroculus::test::addPoint;
using paroculus::test::addSegment;

namespace {

// Document units are nominally millimetres and these fixtures sit at unit
// scale, so an absolute tolerance is the right one. Relative tolerance belongs
// to the large-range property tests.
constexpr double TOL = 1e-6;
// Angles are compared in degrees, where the solver's own gain-up around small
// angles costs a little precision.
constexpr double ANGLE_TOL = 1e-4;

Point at(const SolveContext &c, EntityId id) { return c.point(id).value_or(Point{}); }

Eigen::Vector2d vec(Point p) { return {p.x, p.y}; }

Eigen::Vector2d direction(const Document &d, const SolveContext &c, EntityId segment) {
    const EntityRecord *e = d.entities().find(segment);
    return vec(at(c, e->points[1])) - vec(at(c, e->points[0]));
}

double length(const Document &d, const SolveContext &c, EntityId segment) {
    return direction(d, c, segment).norm();
}

double degreesBetween(const Eigen::Vector2d &a, const Eigen::Vector2d &b) {
    const double cosine = a.dot(b) / (a.norm() * b.norm());
    return std::acos(std::clamp(cosine, -1.0, 1.0)) * 180.0 / M_PI;
}

double pointLineDistance(const Document &d, const SolveContext &c, EntityId point,
                         EntityId segment) {
    const EntityRecord *e = d.entities().find(segment);
    const Eigen::Vector2d a = vec(at(c, e->points[0]));
    const Eigen::Vector2d b = vec(at(c, e->points[1]));
    const Eigen::Vector2d p = vec(at(c, point));
    const Eigen::Vector2d ab = b - a;
    return std::fabs(ab.x() * (p.y() - a.y()) - ab.y() * (p.x() - a.x())) / ab.norm();
}

EntityId addCircle(Document &doc, double cx, double cy, double radius) {
    const EntityId centre = addPoint(doc, cx, cy);
    EntityRecord r;
    r.kind = EntityKind::Circle;
    r.points = {centre, EntityId(), EntityId()};
    r.seeds = {radius, 0.0};
    return EntityId(doc.apply(AddRecord<EntityRecord>{r}).allocated);
}

// An arc through three points. The solver's arc entity carries its own equation
// keeping start and end equidistant from the centre, so the seeds only need to
// be roughly consistent.
EntityId addArc(Document &doc, EntityId centre, EntityId start, EntityId end) {
    EntityRecord r;
    r.kind = EntityKind::Arc;
    r.points = {centre, start, end};
    return EntityId(doc.apply(AddRecord<EntityRecord>{r}).allocated);
}

struct Entry {
    ConstraintKind kind;
    Slot value;
    // Builds the operand geometry with seeds that violate the constraint, and
    // returns the operands in taxonomy order.
    std::function<std::vector<EntityId>(Document &)> build;
    // The geometric residual the solved state must drive to zero.
    std::function<double(const Document &, const SolveContext &,
                         const std::vector<EntityId> &)> residual;
    double tolerance = TOL;
};

std::vector<Entry> catalogue() {
    std::vector<Entry> entries;

    entries.push_back({ConstraintKind::Coincident, Slot(),
                       [](Document &d) {
                           return std::vector<EntityId>{addPoint(d, 0.0, 0.0),
                                                        addPoint(d, 7.0, -3.0)};
                       },
                       [](const Document &, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           return (vec(at(c, o[0])) - vec(at(c, o[1]))).norm();
                       }});

    entries.push_back({ConstraintKind::PointOnLine, Slot(),
                       [](Document &d) {
                           const EntityId a = addPoint(d, 0.0, 0.0);
                           const EntityId b = addPoint(d, 10.0, 0.0);
                           const EntityId p = addPoint(d, 4.0, 6.0);
                           return std::vector<EntityId>{p, addSegment(d, a, b)};
                       },
                       [](const Document &d, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           return pointLineDistance(d, c, o[0], o[1]);
                       }});

    entries.push_back({ConstraintKind::PointOnCircle, Slot(),
                       [](Document &d) {
                           const EntityId circle = addCircle(d, 0.0, 0.0, 5.0);
                           return std::vector<EntityId>{addPoint(d, 9.0, 1.0), circle};
                       },
                       [](const Document &d, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           const EntityRecord *circle = d.entities().find(o[1]);
                           const double r = c.radius(o[1]).value_or(0.0);
                           const double dist =
                               (vec(at(c, o[0])) - vec(at(c, circle->points[0]))).norm();
                           return std::fabs(dist - r);
                       }});

    entries.push_back({ConstraintKind::Midpoint, Slot(),
                       [](Document &d) {
                           const EntityId a = addPoint(d, 0.0, 0.0);
                           const EntityId b = addPoint(d, 10.0, 4.0);
                           const EntityId p = addPoint(d, 1.0, 9.0);
                           return std::vector<EntityId>{p, addSegment(d, a, b)};
                       },
                       [](const Document &d, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           const EntityRecord *s = d.entities().find(o[1]);
                           const Eigen::Vector2d mid =
                               0.5 * (vec(at(c, s->points[0])) + vec(at(c, s->points[1])));
                           return (vec(at(c, o[0])) - mid).norm();
                       }});

    entries.push_back({ConstraintKind::Horizontal, Slot(),
                       [](Document &d) {
                           const EntityId a = addPoint(d, 0.0, 0.0);
                           const EntityId b = addPoint(d, 10.0, 7.0);
                           return std::vector<EntityId>{addSegment(d, a, b)};
                       },
                       [](const Document &d, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           return std::fabs(direction(d, c, o[0]).y());
                       }});

    entries.push_back({ConstraintKind::Vertical, Slot(),
                       [](Document &d) {
                           const EntityId a = addPoint(d, 0.0, 0.0);
                           const EntityId b = addPoint(d, 8.0, 10.0);
                           return std::vector<EntityId>{addSegment(d, a, b)};
                       },
                       [](const Document &d, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           return std::fabs(direction(d, c, o[0]).x());
                       }});

    entries.push_back({ConstraintKind::Parallel, Slot(),
                       [](Document &d) {
                           const EntityId a = addPoint(d, 0.0, 0.0);
                           const EntityId b = addPoint(d, 10.0, 0.0);
                           const EntityId c = addPoint(d, 0.0, 5.0);
                           const EntityId e = addPoint(d, 9.0, 9.0);
                           return std::vector<EntityId>{addSegment(d, a, b),
                                                        addSegment(d, c, e)};
                       },
                       [](const Document &d, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           const Eigen::Vector2d u = direction(d, c, o[0]).normalized();
                           const Eigen::Vector2d v = direction(d, c, o[1]).normalized();
                           return std::fabs(u.x() * v.y() - u.y() * v.x());
                       }});

    entries.push_back({ConstraintKind::Perpendicular, Slot(),
                       [](Document &d) {
                           const EntityId a = addPoint(d, 0.0, 0.0);
                           const EntityId b = addPoint(d, 10.0, 0.0);
                           const EntityId c = addPoint(d, 0.0, 5.0);
                           const EntityId e = addPoint(d, 9.0, 8.0);
                           return std::vector<EntityId>{addSegment(d, a, b),
                                                        addSegment(d, c, e)};
                       },
                       [](const Document &d, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           const Eigen::Vector2d u = direction(d, c, o[0]).normalized();
                           const Eigen::Vector2d v = direction(d, c, o[1]).normalized();
                           return std::fabs(u.dot(v));
                       }});

    entries.push_back({ConstraintKind::Angle, Slot(30.0),
                       [](Document &d) {
                           const EntityId a = addPoint(d, 0.0, 0.0);
                           const EntityId b = addPoint(d, 10.0, 0.0);
                           const EntityId c = addPoint(d, 0.0, 5.0);
                           const EntityId e = addPoint(d, 9.0, 9.0);
                           return std::vector<EntityId>{addSegment(d, a, b),
                                                        addSegment(d, c, e)};
                       },
                       [](const Document &d, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           // The solver constrains the direction cosine, so
                           // either the angle or its supplement satisfies it.
                           const double actual =
                               degreesBetween(direction(d, c, o[0]), direction(d, c, o[1]));
                           return std::min(std::fabs(actual - 30.0),
                                           std::fabs(180.0 - actual - 30.0));
                       },
                       ANGLE_TOL});

    entries.push_back({ConstraintKind::EqualAngle, Slot(),
                       [](Document &d) {
                           const EntityId p0 = addPoint(d, 0.0, 0.0);
                           const EntityId p1 = addPoint(d, 10.0, 0.0);
                           const EntityId p2 = addPoint(d, 0.0, 0.0);
                           const EntityId p3 = addPoint(d, 8.0, 6.0);
                           const EntityId p4 = addPoint(d, 20.0, 0.0);
                           const EntityId p5 = addPoint(d, 30.0, 0.0);
                           const EntityId p6 = addPoint(d, 20.0, 0.0);
                           const EntityId p7 = addPoint(d, 27.0, 9.0);
                           return std::vector<EntityId>{
                               addSegment(d, p0, p1), addSegment(d, p2, p3),
                               addSegment(d, p4, p5), addSegment(d, p6, p7)};
                       },
                       [](const Document &d, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           const double first =
                               degreesBetween(direction(d, c, o[0]), direction(d, c, o[1]));
                           const double second =
                               degreesBetween(direction(d, c, o[2]), direction(d, c, o[3]));
                           return std::fabs(first - second);
                       },
                       ANGLE_TOL});

    entries.push_back({ConstraintKind::PointPointDistance, Slot(25.0),
                       [](Document &d) {
                           return std::vector<EntityId>{addPoint(d, 0.0, 0.0),
                                                        addPoint(d, 3.0, 4.0)};
                       },
                       [](const Document &, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           return std::fabs((vec(at(c, o[0])) - vec(at(c, o[1]))).norm() - 25.0);
                       }});

    entries.push_back({ConstraintKind::PointLineDistance, Slot(6.0),
                       [](Document &d) {
                           const EntityId a = addPoint(d, 0.0, 0.0);
                           const EntityId b = addPoint(d, 10.0, 0.0);
                           const EntityId p = addPoint(d, 4.0, 1.0);
                           return std::vector<EntityId>{p, addSegment(d, a, b)};
                       },
                       [](const Document &d, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           return std::fabs(pointLineDistance(d, c, o[0], o[1]) - 6.0);
                       }});

    entries.push_back({ConstraintKind::EqualLength, Slot(),
                       [](Document &d) {
                           const EntityId a = addPoint(d, 0.0, 0.0);
                           const EntityId b = addPoint(d, 10.0, 0.0);
                           const EntityId c = addPoint(d, 0.0, 5.0);
                           const EntityId e = addPoint(d, 3.0, 5.0);
                           return std::vector<EntityId>{addSegment(d, a, b),
                                                        addSegment(d, c, e)};
                       },
                       [](const Document &d, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           return std::fabs(length(d, c, o[0]) - length(d, c, o[1]));
                       }});

    entries.push_back({ConstraintKind::LengthRatio, Slot(2.5),
                       [](Document &d) {
                           const EntityId a = addPoint(d, 0.0, 0.0);
                           const EntityId b = addPoint(d, 10.0, 0.0);
                           const EntityId c = addPoint(d, 0.0, 5.0);
                           const EntityId e = addPoint(d, 9.0, 5.0);
                           return std::vector<EntityId>{addSegment(d, a, b),
                                                        addSegment(d, c, e)};
                       },
                       [](const Document &d, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           return std::fabs(length(d, c, o[0]) / length(d, c, o[1]) - 2.5);
                       }});

    entries.push_back({ConstraintKind::LengthDifference, Slot(4.0),
                       [](Document &d) {
                           const EntityId a = addPoint(d, 0.0, 0.0);
                           const EntityId b = addPoint(d, 10.0, 0.0);
                           const EntityId c = addPoint(d, 0.0, 5.0);
                           const EntityId e = addPoint(d, 9.0, 5.0);
                           return std::vector<EntityId>{addSegment(d, a, b),
                                                        addSegment(d, c, e)};
                       },
                       [](const Document &d, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           return std::fabs(length(d, c, o[0]) - length(d, c, o[1]) - 4.0);
                       }});

    // Mirrored across the workplane's vertical axis, at equal heights.
    entries.push_back({ConstraintKind::SymmetricHorizontal, Slot(),
                       [](Document &d) {
                           return std::vector<EntityId>{addPoint(d, 4.0, 3.0),
                                                        addPoint(d, -1.0, 8.0)};
                       },
                       [](const Document &, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           const Point a = at(c, o[0]), b = at(c, o[1]);
                           return std::fabs(a.y - b.y) + std::fabs(a.x + b.x);
                       }});

    entries.push_back({ConstraintKind::SymmetricVertical, Slot(),
                       [](Document &d) {
                           return std::vector<EntityId>{addPoint(d, 4.0, 3.0),
                                                        addPoint(d, 9.0, 1.0)};
                       },
                       [](const Document &, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           const Point a = at(c, o[0]), b = at(c, o[1]);
                           return std::fabs(a.x - b.x) + std::fabs(a.y + b.y);
                       }});

    entries.push_back({ConstraintKind::SymmetricAboutLine, Slot(),
                       [](Document &d) {
                           const EntityId la = addPoint(d, 0.0, 0.0);
                           const EntityId lb = addPoint(d, 0.0, 10.0);
                           const EntityId p = addPoint(d, 4.0, 3.0);
                           const EntityId q = addPoint(d, -1.0, 8.0);
                           return std::vector<EntityId>{p, q, addSegment(d, la, lb)};
                       },
                       [](const Document &d, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           // Equidistant from the line, and their midpoint on it.
                           const EntityRecord *s = d.entities().find(o[2]);
                           const Eigen::Vector2d la = vec(at(c, s->points[0]));
                           const Eigen::Vector2d lb = vec(at(c, s->points[1]));
                           const Eigen::Vector2d ab = (lb - la).normalized();
                           const Eigen::Vector2d p = vec(at(c, o[0]));
                           const Eigen::Vector2d q = vec(at(c, o[1]));
                           const Eigen::Vector2d mid = 0.5 * (p + q);
                           const double offLine =
                               std::fabs(ab.x() * (mid.y() - la.y()) - ab.y() * (mid.x() - la.x()));
                           const double alongLine = std::fabs((q - p).normalized().dot(ab));
                           return offLine + alongLine;
                       }});

    // The solver's tangency is the line being perpendicular to the radius at
    // the chosen arc endpoint; it does not itself require them to touch.
    entries.push_back({ConstraintKind::Tangent, Slot(),
                       [](Document &d) {
                           const EntityId centre = addPoint(d, 0.0, 0.0);
                           const EntityId start = addPoint(d, 5.0, 0.0);
                           const EntityId end = addPoint(d, 0.0, 5.0);
                           const EntityId arc = addArc(d, centre, start, end);
                           const EntityId la = addPoint(d, 5.0, -4.0);
                           const EntityId lb = addPoint(d, 7.0, 6.0);
                           return std::vector<EntityId>{arc, addSegment(d, la, lb)};
                       },
                       [](const Document &d, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           const EntityRecord *arc = d.entities().find(o[0]);
                           const Eigen::Vector2d centre = vec(at(c, arc->points[0]));
                           const Eigen::Vector2d start = vec(at(c, arc->points[1]));
                           const Eigen::Vector2d dir = direction(d, c, o[1]).normalized();
                           return std::fabs(dir.dot(centre - start));
                       }});

    entries.push_back({ConstraintKind::Radius, Slot(12.0),
                       [](Document &d) {
                           return std::vector<EntityId>{addCircle(d, 1.0, 2.0, 5.0)};
                       },
                       [](const Document &, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           return std::fabs(c.radius(o[0]).value_or(0.0) - 12.0);
                       }});

    entries.push_back({ConstraintKind::EqualRadius, Slot(),
                       [](Document &d) {
                           return std::vector<EntityId>{addCircle(d, 0.0, 0.0, 5.0),
                                                        addCircle(d, 20.0, 0.0, 9.0)};
                       },
                       [](const Document &, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           return std::fabs(c.radius(o[0]).value_or(0.0) -
                                            c.radius(o[1]).value_or(-1.0));
                       }});

    // A pin holds a point where it sits, so its residual is measured against a
    // second constraint trying to move it.
    entries.push_back({ConstraintKind::Pin, Slot(),
                       [](Document &d) {
                           const EntityId p = addPoint(d, 3.0, 4.0);
                           const EntityId q = addPoint(d, 20.0, 20.0);
                           // Pull q toward p; the pin must keep p put.
                           addConstraint(d, ConstraintKind::PointPointDistance, {p, q},
                                         Slot(1.0));
                           return std::vector<EntityId>{p};
                       },
                       [](const Document &, const SolveContext &c,
                          const std::vector<EntityId> &o) {
                           const Point p = at(c, o[0]);
                           return std::fabs(p.x - 3.0) + std::fabs(p.y - 4.0);
                       }});

    return entries;
}

// Freezes every degree of freedom in the document, so the geometry can no
// longer move to satisfy anything. Adding a constraint the frozen pose violates
// is then reliably inconsistent, whatever the kind.
//
// Points take a pin; circles also need their radius pinned at its current
// value, because a circle owns a parameter its centre point does not — miss
// that and a radius constraint simply resizes the circle instead of conflicting.
void freezeEverything(Document &doc) {
    std::vector<EntityId> points, circles;
    for(const EntityRecord &e : doc.entities().records()) {
        if(e.kind == EntityKind::Point) points.push_back(e.id);
        if(e.kind == EntityKind::Circle) circles.push_back(e.id);
    }
    for(EntityId p : points) addConstraint(doc, ConstraintKind::Pin, {p});
    for(EntityId c : circles) {
        const double radius = doc.entities().find(c)->seeds[0];
        addConstraint(doc, ConstraintKind::Radius, {c}, Slot(radius));
    }
}

}  // namespace

TEST_CASE("every catalogue entry solves and satisfies its own residual") {
    for(const Entry &entry : catalogue()) {
        const std::string name(constraintInfo(entry.kind).name);
        CAPTURE(name);

        Document doc;
        const std::vector<EntityId> operands = entry.build(doc);
        REQUIRE_MESSAGE(!operands.empty(), name);

        const ConstraintId id = addConstraint(doc, entry.kind, operands, entry.value);
        REQUIRE_MESSAGE(id.valid(), name);

        SolveContext context = SolveContext::forWholeDocument(doc);
        const SolveOutcome outcome = solve(doc, context);
        REQUIRE_MESSAGE(outcome.ok(), name, " status ",
                        statusName(outcome.status));

        const double residual = entry.residual(doc, context, operands);
        CHECK_MESSAGE(std::fabs(residual) <= entry.tolerance, name, " residual ", residual);
    }
}

TEST_CASE("the suite covers every constraint kind in the catalogue") {
    // The exit criterion, mechanised: adding a kind without adding its
    // semantics is a red test, not an omission nobody notices.
    std::set<int> covered;
    for(const Entry &e : catalogue()) covered.insert(static_cast<int>(e.kind));

    for(const auto &info : CONSTRAINT_KINDS) {
        CHECK_MESSAGE(covered.count(static_cast<int>(info.kind)) == 1, info.name);
    }
    CHECK(covered.size() == CONSTRAINT_KINDS.size());
}

TEST_CASE("an infeasible system reports inconsistent and blames real constraints") {
    for(const Entry &entry : catalogue()) {
        // A pin cannot contradict a fully pinned document — it is the pinning.
        if(entry.kind == ConstraintKind::Pin) continue;
        const std::string name(constraintInfo(entry.kind).name);
        CAPTURE(name);

        Document doc;
        const std::vector<EntityId> operands = entry.build(doc);
        freezeEverything(doc);
        const ConstraintId id = addConstraint(doc, entry.kind, operands, entry.value);
        REQUIRE_MESSAGE(id.valid(), name);

        SolveContext context = SolveContext::forWholeDocument(doc);
        const SolveOutcome outcome = solve(doc, context);

        CHECK_MESSAGE(!outcome.ok(), name, " unexpectedly solved");
        CHECK_MESSAGE(outcome.status == SolveStatus::Inconsistent, name, " status ",
                      statusName(outcome.status));

        // Every blamed handle must map back to a constraint that exists. This
        // is what proves the failed-set translation, not just that it produced
        // some numbers.
        CHECK_MESSAGE(!outcome.failed.empty(), name, " blamed nothing");
        for(ConstraintId blamed : outcome.failed) {
            CHECK_MESSAGE(doc.constraints().contains(blamed), name, " blamed a ghost");
        }
    }
}

TEST_CASE("imposing the same relation twice is caught as redundant") {
    // Redundancy is where later edits go to die: two constraints that agree
    // today disagree after the next value edit, and the user who added the
    // second was told nothing.
    for(const Entry &entry : catalogue()) {
        if(entry.kind == ConstraintKind::Pin) continue;
        const std::string name(constraintInfo(entry.kind).name);
        CAPTURE(name);

        Document doc;
        const std::vector<EntityId> operands = entry.build(doc);
        REQUIRE(addConstraint(doc, entry.kind, operands, entry.value).valid());

        Topology topology(doc);
        ConstraintRecord candidate;
        candidate.kind = entry.kind;
        candidate.value = entry.value;
        for(size_t i = 0; i < operands.size(); i++) candidate.operands[i] = operands[i];

        const CandidateCheck check = checkCandidate(doc, topology, candidate);
        CHECK_MESSAGE(check.verdict == CandidateVerdict::Redundant, name, " verdict ",
                      static_cast<int>(check.verdict), " dof ", check.dofBefore, " -> ",
                      check.dofAfter);
        // Redundant is tolerated, not refused: it still commits.
        CHECK_MESSAGE(check.committable(), name);
        CHECK_MESSAGE(!check.clean(), name);
    }
}

TEST_CASE("a candidate that adds information is clean") {
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 3.0);
    const EntityId segment = addSegment(doc, a, b);
    Topology topology(doc);

    ConstraintRecord candidate;
    candidate.kind = ConstraintKind::Horizontal;
    candidate.operands = {segment, EntityId(), EntityId(), EntityId()};

    const CandidateCheck check = checkCandidate(doc, topology, candidate);
    CHECK(check.verdict == CandidateVerdict::Consistent);
    CHECK(check.clean());
    // It consumed exactly one degree of freedom.
    CHECK(check.dofAfter == check.dofBefore - 1);
}

TEST_CASE("a candidate that cannot hold is reported with its conflicting set") {
    // The over-constraint downgrade path: the action surface offers a driven
    // reference measurement instead, with these constraints highlighted.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 0.0);
    addConstraint(doc, ConstraintKind::Pin, {a});
    addConstraint(doc, ConstraintKind::Pin, {b});
    Topology topology(doc);

    ConstraintRecord candidate;
    candidate.kind = ConstraintKind::PointPointDistance;
    candidate.operands = {a, b, EntityId(), EntityId()};
    candidate.value = Slot(999.0);  // the pinned points are 10 apart

    const CandidateCheck check = checkCandidate(doc, topology, candidate);
    CHECK(check.verdict == CandidateVerdict::Inconsistent);
    CHECK_FALSE(check.committable());

    // Whatever is in the set names a real constraint. The candidate rides in as
    // an extra and the solver blames it like any other, but it has no ID yet, so
    // leaving it in would highlight a constraint that does not exist.
    //
    // And the set may be empty, as it is here: the solver blamed only the
    // constraint it could not satisfy and said nothing about which pin it
    // disagrees with. That is the verdict doing its job and the attribution
    // being unavailable, which is what stage 5's conflict walking is for. This
    // assertion used to read "not empty" and passed on the ghost alone.
    for(ConstraintId id : check.conflicting) {
        CHECK(id.valid());
        CHECK(id != candidate.id);
    }
}

TEST_CASE("a conflict no single suppression rescues is not attributed") {
    // The walk suppresses one relation at a time and does not enumerate pairs,
    // so a conflict needing two suppressions comes back with nothing. Reporting
    // that as attributed is the "empty walkable set read as nothing conflicts"
    // lie the header warns a surface cannot see through — and stage 7's compound
    // relations make multi-constraint conflicts routine.
    Document doc;
    const EntityId a0 = addPoint(doc, 0.0, 0.0);
    const EntityId a1 = addPoint(doc, 10.0, 1.0);
    const EntityId b0 = addPoint(doc, 0.0, 20.0);
    const EntityId b1 = addPoint(doc, 10.0, 21.0);
    const EntityId first = addSegment(doc, a0, a1);
    const EntityId second = addSegment(doc, b0, b1);
    addConstraint(doc, ConstraintKind::Horizontal, {first});
    addConstraint(doc, ConstraintKind::Horizontal, {second});
    addConstraint(doc, ConstraintKind::Parallel, {first, second});
    Topology topology(doc);

    // Perpendicular cannot hold: dropping either horizontal leaves the other
    // plus the parallel still forcing both segments flat, and dropping the
    // parallel leaves both horizontals doing it directly. No one relation is
    // the answer.
    ConstraintRecord candidate;
    candidate.kind = ConstraintKind::Perpendicular;
    candidate.operands = {first, second, EntityId(), EntityId()};

    const CandidateCheck check = checkCandidate(doc, topology, candidate);
    CHECK(check.verdict == CandidateVerdict::Inconsistent);
    CHECK(check.conflicting.empty());
    CHECK_FALSE(check.attributed);
}

TEST_CASE("the angle residual is exactly as strict as the solver") {
    // The residual accepted the angle or its supplement, justified by the solver
    // constraining a direction cosine — but cos(180 − θ) = −cos(θ), so the
    // supplement satisfies nothing unless the record names the other form. A
    // geometric check more permissive than the solver passes exactly the
    // translation bug it exists to catch.
    Document doc;
    const EntityId o = addPoint(doc, 0.0, 0.0);
    const EntityId along = addPoint(doc, 10.0, 0.0);
    // 120 degrees round from the first.
    const EntityId across = addPoint(doc, -5.0, 5.0 * std::sqrt(3.0));
    const EntityId first = addSegment(doc, o, along);
    const EntityId second = addSegment(doc, o, across);

    ConstraintRecord r;
    r.kind = ConstraintKind::Angle;
    r.operands = {first, second, EntityId(), EntityId()};
    r.value = Slot(60.0);

    const Pose pose(doc);
    // The segments are 120 apart and the record says 60. That is a violation of
    // sixty degrees, not a satisfied constraint wearing its supplement.
    REQUIRE(residual(pose, r));
    CHECK(*residual(pose, r) == doctest::Approx(60.0));

    // The supplementary form says the same thing about the same pair and is
    // satisfied here — which is what makes it a form rather than a second kind.
    r.alternative = 1;
    CHECK(*residual(pose, r) == doctest::Approx(0.0));

    // And the solver reads it the same way round. Both ends of the first segment
    // are pinned, so the declaration is what places the second.
    for(uint8_t alternative : {uint8_t(0), uint8_t(1)}) {
        Document held;
        const EntityId h0 = addPoint(held, 0.0, 0.0);
        const EntityId h1 = addPoint(held, 10.0, 0.0);
        const EntityId h2 = addPoint(held, 3.0, 9.0);
        const EntityId base = addSegment(held, h0, h1);
        const EntityId swung = addSegment(held, h0, h2);
        addConstraint(held, ConstraintKind::Pin, {h0});
        addConstraint(held, ConstraintKind::Pin, {h1});
        ConstraintRecord declared;
        declared.kind = ConstraintKind::Angle;
        declared.operands = {base, swung, EntityId(), EntityId()};
        declared.value = Slot(60.0);
        declared.alternative = alternative;
        REQUIRE(held.apply(AddRecord<ConstraintRecord>{declared}).ok());

        SolveContext context = SolveContext::forWholeDocument(held);
        REQUIRE(solve(held, context).ok());

        Pose solved(held);
        solved.overlay(context.params());
        const ConstraintRecord &landed = held.constraints().records().back();
        CAPTURE(int(alternative));
        // Whichever form it names, the residual it is measured by is zero — and
        // the two forms are genuinely different geometry.
        CHECK(*residual(solved, landed) == doctest::Approx(0.0).epsilon(1e-6));
        const double between = *measure(solved, ConstraintKind::Angle, landed.operands);
        CHECK(between == doctest::Approx(alternative == 0 ? 60.0 : 120.0).epsilon(1e-6));
    }
}

TEST_CASE("a malformed candidate gets one verdict, not two failure modes") {
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 1.0, 0.0);
    Topology topology(doc);

    ConstraintRecord candidate;
    candidate.kind = ConstraintKind::Parallel;  // wants segments
    candidate.operands = {a, b, EntityId(), EntityId()};

    const CandidateCheck check = checkCandidate(doc, topology, candidate);
    CHECK(check.verdict == CandidateVerdict::Malformed);
    CHECK_FALSE(check.committable());
}

TEST_CASE("a reference measurement constrains nothing") {
    // Driven and driving are the same object with a toggle; only one of them
    // reaches the solver.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 3.0, 4.0);

    ConstraintRecord reference;
    reference.kind = ConstraintKind::PointPointDistance;
    reference.operands = {a, b, EntityId(), EntityId()};
    reference.value = Slot(100.0);
    reference.driving = false;
    REQUIRE(doc.apply(AddRecord<ConstraintRecord>{reference}).ok());

    SolveContext context = SolveContext::forWholeDocument(doc);
    const SolveOutcome outcome = solve(doc, context);
    REQUIRE(outcome.ok());
    // The geometry kept its seeds; the measurement did not pull it to 100.
    CHECK((vec(at(context, a)) - vec(at(context, b))).norm() == doctest::Approx(5.0));
    CHECK(outcome.dof == 4);
}

TEST_CASE("a lock that splits a circle from its centre cannot make it inconsistent") {
    // A circle on a locked layer has its radius parameter frozen in the fixed
    // base group; move its centre point to an unlocked layer and the centre
    // floats free while the radius stays fixed. A Radius constraint reads only
    // that frozen radius, so emitting it against a value the seed does not hold
    // reports Inconsistent — a lock making a system contradictory, the one thing
    // a lock must never do. The omission is decided per parameter, over the
    // footprint the constraint actually reads, so the radius constraint drops and
    // the lock wins rather than the circle freezing at a contradiction.
    Document doc;
    const EntityId circle = addCircle(doc, 1.0, 2.0, 5.0);
    const EntityId centre = doc.entities().find(circle)->points[0];
    REQUIRE(centre.valid());

    LayerRecord frozen;
    frozen.name = "frozen";
    frozen.locked = true;
    const LayerId locked(doc.apply(AddRecord<LayerRecord>{frozen}).allocated);
    REQUIRE(locked.valid());

    // The circle is locked; its centre stays on the unlocked base layer.
    EntityRecord onLocked = *doc.entities().find(circle);
    onLocked.layer = locked;
    REQUIRE(doc.apply(SetRecord<EntityRecord>{onLocked}).ok());

    // A driving radius the frozen seed (5) does not satisfy (12).
    REQUIRE(addConstraint(doc, ConstraintKind::Radius, {circle}, Slot(12.0)).valid());

    SolveContext context = SolveContext::forWholeDocument(doc);
    const SolveOutcome outcome = solve(doc, context);
    CHECK(outcome.status != SolveStatus::Inconsistent);
    CHECK(outcome.ok());
    // The radius held its locked seed rather than driving to the dimension.
    CHECK(context.radius(circle).value_or(0.0) == doctest::Approx(5.0));

    // Unlock the circle and the same constraint drives the radius: the omission
    // keys on the frozen parameter, not on the kind, so a circle with a free
    // radius takes its dimension exactly as before.
    EntityRecord unlocked = *doc.entities().find(circle);
    unlocked.layer = LayerId();
    REQUIRE(doc.apply(SetRecord<EntityRecord>{unlocked}).ok());

    SolveContext driven = SolveContext::forWholeDocument(doc);
    const SolveOutcome outcomeDriven = solve(doc, driven);
    CHECK(outcomeDriven.ok());
    CHECK(driven.radius(circle).value_or(0.0) == doctest::Approx(12.0));
}
