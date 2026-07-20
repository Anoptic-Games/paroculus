#include "interact/loops.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "interact/selection.h"

namespace paroculus {
namespace {

// The joints an edge presents to a boundary walk, from the one function that
// knows which points those are per kind. A closed curve presents none: it meets
// no neighbour, which is exactly why it bounds a region alone.
std::vector<EntityId> jointsOf(const Document &doc, EntityId edge) {
    const EntityRecord *e = doc.entities().find(edge);
    if(e == nullptr) return {};
    const BoundaryEnds ends = boundaryEnds(*e);
    if(!ends.capable || ends.selfClosing) return {};
    return {ends.from, ends.to};
}

// Whether this edge closes a region on its own.
bool boundsAlone(const Document &doc, EntityId edge) {
    const EntityRecord *e = doc.entities().find(edge);
    return e != nullptr && boundaryEnds(*e).selfClosing;
}

bool isOutlineEdge(const Document &doc, EntityId id) {
    const EntityRecord *e = doc.entities().find(id);
    if(e == nullptr) return false;
    // Construction geometry is not part of the outline it helped place. An
    // arc's centre would otherwise turn every arc into a run that cannot close.
    if(e->role == Role::Construction) return false;
    return entityInfo(e->kind).boundaryCapable;
}

double gapBetween(const Pose &pose, EntityId a, EntityId b) {
    const std::optional<Point> pa = pose.point(a);
    const std::optional<Point> pb = pose.point(b);
    if(!pa || !pb) return std::numeric_limits<double>::infinity();
    const double dx = pb->x - pa->x;
    const double dy = pb->y - pa->y;
    return std::sqrt(dx * dx + dy * dy);
}

// An outline edge reduced to what a crossing test needs.
//
// Curves and straight edges answer the same two questions — where does this meet
// that, and is the meeting strictly between my ends — so the test is written
// once over this rather than as a matrix of kind pairs.
struct EdgeShape {
    bool curve = false;
    // Straight edges.
    Point from, to;
    // Curves. `sweep` is a full turn for a circle, and `endless` says so: a
    // circle has no ends, so no meeting point on it can be the joint where two
    // edges were meant to meet.
    Point centre;
    double radius = 0.0;
    double startAngle = 0.0;
    double sweep = 0.0;
    bool endless = false;
};

std::optional<EdgeShape> shapeOf(const Document &doc, const Pose &pose, EntityId id) {
    const EntityRecord *e = doc.entities().find(id);
    if(e == nullptr) return std::nullopt;
    EdgeShape out;
    if(e->kind == EntityKind::Segment) {
        const std::optional<std::pair<Point, Point>> ends = pose.segment(id);
        if(!ends) return std::nullopt;
        out.from = ends->first;
        out.to = ends->second;
        return out;
    }
    if(e->kind == EntityKind::Circle) {
        const std::optional<Point> centre = pose.point(e->points[0]);
        const std::optional<double> radius = pose.radius(id);
        if(!centre || !radius || *radius <= 0.0) return std::nullopt;
        out.curve = true;
        out.centre = *centre;
        out.radius = *radius;
        out.sweep = 2.0 * M_PI;
        out.endless = true;
        return out;
    }
    if(e->kind == EntityKind::Arc) {
        const std::optional<Pose::ArcGeometry> arc = pose.arc(id);
        if(!arc || arc->radius <= 0.0) return std::nullopt;
        out.curve = true;
        out.centre = arc->centre;
        out.radius = arc->radius;
        out.startAngle = arc->startAngle;
        out.sweep = arc->sweep;
        return out;
    }
    return std::nullopt;
}

// Open intervals at both ends, on both kinds of edge.
//
// Ends that meet are how a boundary is built: a shared or coincident endpoint is
// the ordinary case, and reporting it as a crossing would call every drawn
// outline the deferred case. The epsilon keeps a joint the solver has left a
// hair short of exact from reading as one.
constexpr double INSIDE = 1e-9;

bool interiorOfSegment(double t) { return t > INSIDE && t < 1.0 - INSIDE; }

// Whether a point on the curve's circle lies strictly inside the swept part.
bool interiorOfCurve(const EdgeShape &shape, Point at) {
    if(shape.endless) return true;  // no ends to be at
    if(shape.sweep == 0.0) return false;
    const double angle = std::atan2(at.y - shape.centre.y, at.x - shape.centre.x);
    double delta = std::fmod(angle - shape.startAngle, 2.0 * M_PI);
    if(delta < 0.0) delta += 2.0 * M_PI;
    // The sweep runs counter-clockwise and is positive, matching the solver.
    const double u = delta / shape.sweep;
    return u > INSIDE && u < 1.0 - INSIDE;
}

// Parameters along a straight edge where it meets a curve's circle.
//
// Tangency reports nothing. A line touching a circle at a single point crosses
// it no more than two parallel segments cross each other, and the deferred work
// this diagnostic points at — building through an explicit intersection point —
// has nothing to build there either.
void straightMeetsCircle(const EdgeShape &line, const EdgeShape &curve,
                         std::vector<double> &out) {
    const double rx = line.to.x - line.from.x;
    const double ry = line.to.y - line.from.y;
    const double fx = line.from.x - curve.centre.x;
    const double fy = line.from.y - curve.centre.y;

    const double a = rx * rx + ry * ry;
    if(a < 1e-18) return;  // a degenerate edge meets nothing
    const double b = 2.0 * (fx * rx + fy * ry);
    const double c = fx * fx + fy * fy - curve.radius * curve.radius;

    const double discriminant = b * b - 4.0 * a * c;
    // Scaled by the quadratic's own magnitude, so the tangency test means the
    // same thing on a drawing measured in millimetres and one in metres.
    const double scale = std::max(1.0, std::fabs(b * b));
    if(discriminant <= scale * 1e-18) return;

    const double root = std::sqrt(discriminant);
    out.push_back((-b - root) / (2.0 * a));
    out.push_back((-b + root) / (2.0 * a));
}

// Where two circles meet. Tangent and concentric pairs report nothing, for the
// same reason a tangent line does.
void circleMeetsCircle(const EdgeShape &a, const EdgeShape &b, std::vector<Point> &out) {
    const double dx = b.centre.x - a.centre.x;
    const double dy = b.centre.y - a.centre.y;
    const double d = std::hypot(dx, dy);
    if(d < 1e-12) return;  // concentric: no crossing, however the radii compare
    if(d >= a.radius + b.radius) return;            // apart, or externally tangent
    if(d <= std::fabs(a.radius - b.radius)) return;  // nested, or internally tangent

    const double t = (a.radius * a.radius - b.radius * b.radius + d * d) / (2.0 * d);
    const double hSquared = a.radius * a.radius - t * t;
    if(hSquared <= 0.0) return;
    const double h = std::sqrt(hSquared);

    const double mx = a.centre.x + t * dx / d;
    const double my = a.centre.y + t * dy / d;
    out.push_back(Point{mx + h * dy / d, my - h * dx / d});
    out.push_back(Point{mx - h * dy / d, my + h * dx / d});
}

// Where two outline edges cross, if they cross strictly between their ends.
//
// Every pairing of straight and curved, because both bound regions now. An
// arc that crosses a segment encloses an area the model cannot name exactly as
// two crossing segments do, and saying nothing about it is the silence this
// diagnostic exists to break.
bool edgesCross(const Document &doc, const Pose &pose, EntityId a, EntityId b) {
    const std::optional<EdgeShape> first = shapeOf(doc, pose, a);
    const std::optional<EdgeShape> second = shapeOf(doc, pose, b);
    if(!first || !second) return false;

    if(!first->curve && !second->curve) {
        const double px = first->from.x, py = first->from.y;
        const double rx = first->to.x - px, ry = first->to.y - py;
        const double qx = second->from.x, qy = second->from.y;
        const double sx = second->to.x - qx, sy = second->to.y - qy;

        const double denominator = rx * sy - ry * sx;
        if(std::fabs(denominator) < 1e-12) return false;  // parallel, or collinear

        const double t = ((qx - px) * sy - (qy - py) * sx) / denominator;
        const double u = ((qx - px) * ry - (qy - py) * rx) / denominator;
        return interiorOfSegment(t) && interiorOfSegment(u);
    }

    if(first->curve && second->curve) {
        std::vector<Point> meetings;
        circleMeetsCircle(*first, *second, meetings);
        for(const Point &at : meetings) {
            if(interiorOfCurve(*first, at) && interiorOfCurve(*second, at)) return true;
        }
        return false;
    }

    const EdgeShape &line = first->curve ? *second : *first;
    const EdgeShape &curve = first->curve ? *first : *second;
    std::vector<double> parameters;
    straightMeetsCircle(line, curve, parameters);
    for(double t : parameters) {
        if(!interiorOfSegment(t)) continue;
        const Point at{line.from.x + t * (line.to.x - line.from.x),
                       line.from.y + t * (line.to.y - line.from.y)};
        if(interiorOfCurve(curve, at)) return true;
    }
    return false;
}

// The outline edges around `seed`, following coincident joints and near-miss
// ones alike.
//
// Proximity has to grow the neighbourhood, not merely decorate it: an outline
// whose only open corner is the one joining its first edge to its last is two
// runs as far as the coincidence graph is concerned, and a walk that only
// followed coincidences would never see them as one thing.
//
// Scans the entity table per edge reached, so this costs O(n^2) in the size of
// the neighbourhood — the same trade connectedRun records, and worth revisiting
// with the same adjacency index when a profile asks.
std::vector<EntityId> outlineNeighbourhood(const Document &doc, const Topology &topology,
                                           const Pose &pose, EntityId seed, double tolerance) {
    std::vector<EntityId> found;
    std::vector<EntityId> pending;

    auto consider = [&](EntityId id) {
        if(!isOutlineEdge(doc, id)) return;
        if(std::find(found.begin(), found.end(), id) != found.end()) return;
        if(std::find(pending.begin(), pending.end(), id) != pending.end()) return;
        pending.push_back(id);
    };

    // The seed may be an edge or a point the user clicked; either way, start
    // from whatever outline edges it belongs to.
    if(isOutlineEdge(doc, seed)) {
        pending.push_back(seed);
    } else {
        for(EntityId id : connectedRun(doc, topology, seed)) consider(id);
    }

    while(!pending.empty()) {
        const EntityId edge = pending.back();
        pending.pop_back();
        found.push_back(edge);

        for(EntityId joint : jointsOf(doc, edge)) {
            for(const EntityRecord &other : doc.entities().records()) {
                if(other.id == edge) continue;
                if(!isOutlineEdge(doc, other.id)) continue;
                for(EntityId theirs : jointsOf(doc, other.id)) {
                    if(topology.coincident(joint, theirs) ||
                       gapBetween(pose, joint, theirs) <= tolerance) {
                        consider(other.id);
                        break;
                    }
                }
            }
        }
    }

    std::sort(found.begin(), found.end());
    return found;
}

}  // namespace

std::optional<std::vector<EntityId>> closedBoundaryContaining(const Document &doc,
                                                              const Topology &topology,
                                                              EntityId seed) {
    if(!seed.valid() || doc.entities().find(seed) == nullptr) return std::nullopt;

    // A closed curve is its own boundary, answered here rather than in the cycle
    // walk. It has no joints, so it can never be an edge of a joint cycle — and
    // asking the walk to special-case it would mean deciding, for a circle
    // sitting on a triangle's corner, which of the two closed things the user
    // meant. The seed already says: they clicked one of them.
    if(boundsAlone(doc, seed)) return std::vector<EntityId>{seed};

    // The run is what the user has been drawing: everything reachable through
    // shared and coincident vertices. Edges are what a boundary is made of, so
    // the points come along for the walk and drop out of the cycle test.
    std::vector<EntityId> edges;
    for(EntityId id : connectedRun(doc, topology, seed)) {
        if(isOutlineEdge(doc, id)) edges.push_back(id);
    }
    if(edges.empty()) return std::nullopt;

    if(std::optional<std::vector<EntityId>> cycle = findBoundaryCycle(doc, topology, edges)) {
        return cycle;
    }

    // No joint cycle, but the run may still hold a closed curve.
    //
    // Reached whenever the seed is not the curve itself, which is the ordinary
    // case rather than an edge case: selecting a circle takes the connected
    // shape — its centre and its rim — and the offers are seeded from the front
    // of that selection, which is the centre point. A circle the user has just
    // clicked on would otherwise offer nothing, while the same circle offered
    // make-solid a moment earlier when it was the thing just drawn.
    //
    // Exactly one, because two closed curves in one run is a question about
    // which the user meant, and guessing is the silent choice the surface exists
    // to avoid. After the cycle walk, so a triangle with a circle centred on one
    // of its corners still offers the triangle.
    EntityId alone;
    for(EntityId id : edges) {
        if(!boundsAlone(doc, id)) continue;
        if(alone.valid()) return std::nullopt;
        alone = id;
    }
    if(alone.valid()) return std::vector<EntityId>{alone};
    return std::nullopt;
}

std::optional<std::pair<EntityId, EntityId>> crossingAmong(const Document &doc,
                                                           const Topology &topology,
                                                           const Pose &pose,
                                                           std::span<const EntityId> seeds) {
    std::vector<EntityId> edges;
    for(EntityId seed : seeds) {
        if(!seed.valid() || doc.entities().find(seed) == nullptr) continue;
        for(EntityId id : connectedRun(doc, topology, seed)) {
            if(!isOutlineEdge(doc, id)) continue;
            if(std::find(edges.begin(), edges.end(), id) == edges.end()) edges.push_back(id);
        }
    }
    // ID-ordered and first-found, so the same drawing always names the same
    // pair. Which crossing is reported matters as little as the fact that it is
    // always the same one.
    std::sort(edges.begin(), edges.end());
    for(size_t i = 0; i < edges.size(); i++) {
        for(size_t j = i + 1; j < edges.size(); j++) {
            if(edgesCross(doc, pose, edges[i], edges[j])) {
                return std::pair<EntityId, EntityId>{edges[i], edges[j]};
            }
        }
    }
    return std::nullopt;
}

std::optional<HealableLoop> healableLoopContaining(const Document &doc,
                                                   const Topology &topology, const Pose &pose,
                                                   EntityId seed, double tolerance) {
    if(!seed.valid() || doc.entities().find(seed) == nullptr) return std::nullopt;
    if(tolerance <= 0.0) return std::nullopt;

    // Already closed is not healable. That is not a technicality: make-solid
    // and heal-and-fill are different offers because one moves nothing and the
    // other moves geometry, and a surface that ran them together would be
    // moving geometry on an action that promised not to.
    if(closedBoundaryContaining(doc, topology, seed)) return std::nullopt;

    const std::vector<EntityId> edges =
        outlineNeighbourhood(doc, topology, pose, seed, tolerance);
    // Same bound the closed walk uses, and read from the same place: an arc and
    // a chord whose ends are a hair apart are two edges that will enclose a
    // circular segment once the gap is shut, and a hard three here would refuse
    // to offer the heal that make-solid would then accept.
    bool anyCurved = false;
    for(EntityId id : edges) {
        const EntityRecord *e = doc.entities().find(id);
        if(e != nullptr && boundaryEnds(*e).curved) anyCurved = true;
    }
    if(!enclosesArea(edges.size(), anyCurved)) return std::nullopt;

    // Every near miss among the neighbourhood's joints, ID-ordered. Ordered
    // because they become constraint records applied in sequence, and a set
    // that reshuffled would make the same drawing heal into two different
    // documents.
    std::vector<LoopGap> candidates;
    std::vector<EntityId> joints;
    for(EntityId edge : edges) {
        for(EntityId j : jointsOf(doc, edge)) {
            if(std::find(joints.begin(), joints.end(), j) == joints.end()) joints.push_back(j);
        }
    }
    std::sort(joints.begin(), joints.end());
    for(size_t i = 0; i < joints.size(); i++) {
        for(size_t j = i + 1; j < joints.size(); j++) {
            if(topology.coincident(joints[i], joints[j])) continue;
            const double d = gapBetween(pose, joints[i], joints[j]);
            if(d > tolerance) continue;
            candidates.push_back({joints[i], joints[j], d});
        }
    }
    if(candidates.empty()) return std::nullopt;

    // Ask the question on a copy with the coincidences in place, rather than
    // reimplementing the cycle walk over an augmented graph. One cycle finder,
    // one definition of closed — the alternative is two, and the second one
    // drifts. The copy is legitimate for the same reason the imposition check's
    // is: this runs when a placement lands or a selection changes, not per
    // frame, and it must not leave a trace either way.
    Document healed = doc;
    for(const LoopGap &gap : candidates) {
        ConstraintRecord r;
        r.kind = ConstraintKind::Coincident;
        r.operands[0] = gap.a;
        r.operands[1] = gap.b;
        healed.apply(AddRecord<ConstraintRecord>{r});
    }
    Topology healedTopology(healed);
    const auto boundary = findBoundaryCycle(healed, healedTopology, edges);
    if(!boundary) return std::nullopt;

    // Which gaps the cycle actually uses. Consecutive edges meet at one joint
    // pair, and a pair that was not coincident before is a gap the heal has to
    // shut — so the reported set is the minimal one rather than every near miss
    // in the neighbourhood. Imposing a coincidence the loop does not need would
    // be moving geometry for no reason the user asked for.
    HealableLoop out;
    out.boundary = *boundary;
    for(size_t i = 0; i < out.boundary.size(); i++) {
        const EntityId here = out.boundary[i];
        const EntityId next = out.boundary[(i + 1) % out.boundary.size()];
        for(EntityId a : jointsOf(doc, here)) {
            for(EntityId b : jointsOf(doc, next)) {
                if(topology.coincident(a, b)) continue;
                if(!healedTopology.coincident(a, b)) continue;
                const double d = gapBetween(pose, a, b);
                if(d > tolerance) continue;
                const LoopGap gap{std::min(a, b), std::max(a, b), d};
                if(std::find(out.gaps.begin(), out.gaps.end(), gap) == out.gaps.end()) {
                    out.gaps.push_back(gap);
                }
            }
        }
    }
    if(out.gaps.empty()) return std::nullopt;

    std::sort(out.gaps.begin(), out.gaps.end(), [](const LoopGap &x, const LoopGap &y) {
        return x.a == y.a ? x.b < y.b : x.a < y.a;
    });
    for(const LoopGap &gap : out.gaps) out.widestGap = std::max(out.widestGap, gap.distance);
    return out;
}

std::vector<Command> healingStep(const Document &doc, const HealableLoop &loop) {
    std::vector<Command> out;
    for(const LoopGap &gap : loop.gaps) {
        if(doc.entities().find(gap.a) == nullptr) continue;
        if(doc.entities().find(gap.b) == nullptr) continue;
        ConstraintRecord r;
        r.kind = ConstraintKind::Coincident;
        r.operands[0] = gap.a;
        r.operands[1] = gap.b;
        out.push_back(AddRecord<ConstraintRecord>{r});
    }
    return out;
}

}  // namespace paroculus
