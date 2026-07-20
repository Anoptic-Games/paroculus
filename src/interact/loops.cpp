#include "interact/loops.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "interact/selection.h"

namespace paroculus {
namespace {

// The endpoints an edge presents to a boundary walk. A segment has two; an arc
// has two and a centre that is construction geometry and not a joint.
std::vector<EntityId> jointsOf(const Document &doc, EntityId edge) {
    const EntityRecord *e = doc.entities().find(edge);
    if(e == nullptr) return {};
    switch(e->kind) {
        case EntityKind::Segment: return {e->points[0], e->points[1]};
        case EntityKind::Arc:     return {e->points[1], e->points[2]};
        default:                  return {};
    }
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

// Where two segments cross, if they cross strictly between their ends.
//
// Strictly, because ends that meet are how a boundary is built: a shared or
// coincident endpoint is the ordinary case and reporting it as a crossing would
// call every drawn outline the deferred case. Parallel and collinear pairs
// report nothing — they have no single crossing point to build through, which
// is the thing the deferred work would need.
bool segmentsCross(const Pose &pose, EntityId a, EntityId b) {
    const std::optional<std::pair<Point, Point>> first = pose.segment(a);
    const std::optional<std::pair<Point, Point>> second = pose.segment(b);
    if(!first || !second) return false;

    const double px = first->first.x, py = first->first.y;
    const double rx = first->second.x - px, ry = first->second.y - py;
    const double qx = second->first.x, qy = second->first.y;
    const double sx = second->second.x - qx, sy = second->second.y - qy;

    const double denominator = rx * sy - ry * sx;
    if(std::fabs(denominator) < 1e-12) return false;  // parallel, or collinear

    const double t = ((qx - px) * sy - (qy - py) * sx) / denominator;
    const double u = ((qx - px) * ry - (qy - py) * rx) / denominator;
    // Open intervals at both ends. The epsilon keeps a joint that the solver
    // has left a hair short of exact from reading as a crossing.
    constexpr double INSIDE = 1e-9;
    return t > INSIDE && t < 1.0 - INSIDE && u > INSIDE && u < 1.0 - INSIDE;
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

    // The run is what the user has been drawing: everything reachable through
    // shared and coincident vertices. Edges are what a boundary is made of, so
    // the points come along for the walk and drop out of the cycle test.
    std::vector<EntityId> edges;
    for(EntityId id : connectedRun(doc, topology, seed)) {
        if(isOutlineEdge(doc, id)) edges.push_back(id);
    }
    if(edges.empty()) return std::nullopt;

    return findBoundaryCycle(doc, topology, edges);
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
            if(segmentsCross(pose, edges[i], edges[j])) {
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
    if(edges.size() < 3) return std::nullopt;

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
