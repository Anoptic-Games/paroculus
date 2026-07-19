#include "interact/snap.h"

#include <algorithm>
#include <cmath>

namespace paroculus {
namespace {

constexpr double DEGREES = 180.0 / 3.14159265358979323846;

double distance(Point a, Point b) { return std::hypot(a.x - b.x, a.y - b.y); }

// The smaller angle between two directions, in degrees, ignoring sense: a
// segment drawn right-to-left is as horizontal as one drawn left-to-right.
double directionGap(double ax, double ay, double bx, double by) {
    const double na = std::hypot(ax, ay);
    const double nb = std::hypot(bx, by);
    if(na == 0.0 || nb == 0.0) return 180.0;
    const double c = std::clamp((ax * bx + ay * by) / (na * nb), -1.0, 1.0);
    const double angle = std::acos(c) * DEGREES;
    return std::min(angle, 180.0 - angle);
}

// Foot of the perpendicular from p onto the segment ab, clamped to the span.
Point projectOnto(Point a, Point b, Point p) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    if(len2 == 0.0) return a;
    double t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
    t = std::clamp(t, 0.0, 1.0);
    return Point{a.x + t * dx, a.y + t * dy};
}

std::optional<size_t> recentRankOf(const std::vector<SnapKind> &recent, SnapKind kind,
                                   size_t depth) {
    const size_t limit = std::min(depth, recent.size());
    for(size_t i = 0; i < limit; i++) {
        if(recent[i] == kind) return i;
    }
    return std::nullopt;
}

}  // namespace

std::vector<SnapCandidate> SnapResult::autoCommitted() const {
    std::vector<SnapCandidate> out;
    for(const SnapCandidate &c : candidates) {
        if(c.autoCommits()) out.push_back(c);
    }
    return out;
}

std::vector<SnapCandidate> SnapResult::offered() const {
    std::vector<SnapCandidate> out;
    for(const SnapCandidate &c : candidates) {
        if(c.tier() == SnapTier::Offered) out.push_back(c);
    }
    return out;
}

SnapResult snap(const Document &doc, const Pose &pose, const SpatialIndex &index,
                const ViewTransform &view, const SnapRequest &request,
                const SnapPolicy &policy) {
    SnapResult result;
    result.placement = request.cursor;

    // Pixel tolerances become document lengths once, here, at a named boundary.
    const double pointRadius = view.toDocumentLength(policy.pointRadius);
    const double lineRadius = view.toDocumentLength(policy.lineRadius);
    const double searchRadius = std::max(pointRadius, lineRadius);

    auto pixels = [&](Point a, Point b) {
        return (view.toScreen(a) - view.toScreen(b)).norm();
    };

    auto add = [&](SnapKind kind, SnapSubject subject, EntityId target, Point placement) {
        SnapCandidate c;
        c.kind = kind;
        c.subject = subject;
        c.target = target;
        c.placement = placement;
        c.correction = pixels(placement, request.cursor);
        c.score = snapScore(policy, snapInfo(kind).tier, c.correction,
                            recentRankOf(request.recent, kind, policy.recentDepth));
        c.confirmed = std::find(request.confirmed.begin(), request.confirmed.end(),
                                std::pair{kind, target}) != request.confirmed.end();
        // A confirmed offer outranks everything: the user has said which
        // relation they meant, and the ranking's job is over.
        if(c.confirmed) c.score += 10.0 * policy.tierWeight;
        result.candidates.push_back(c);
    };

    // Point-valued kinds: what the placed point could bind to.
    for(EntityId id : index.near(request.cursor, searchRadius)) {
        // The chain's own anchor is not a snap target. Offering to make a
        // segment coincident with the point it already starts from would
        // propose a degenerate segment, and the user pointing near their own
        // anchor means "short segment", not "zero-length one".
        if(id == request.anchorEntity) continue;

        const EntityRecord *e = doc.entities().find(id);
        if(e == nullptr) continue;
        // Construction geometry is selectable and constrainable but does not
        // attract, so an arc's centre does not become a magnet by existing.
        if(!policy.snapToConstruction && e->role == Role::Construction) continue;

        if(const std::optional<Point> p = pose.point(id)) {
            if(distance(*p, request.cursor) <= pointRadius) {
                add(SnapKind::Endpoint, SnapSubject::PlacedPoint, id, *p);
            }
            continue;
        }

        if(const auto ends = pose.segment(id)) {
            const Point mid{(ends->first.x + ends->second.x) * 0.5,
                            (ends->first.y + ends->second.y) * 0.5};
            if(distance(mid, request.cursor) <= pointRadius) {
                add(SnapKind::Midpoint, SnapSubject::PlacedPoint, id, mid);
            }
            const Point foot = projectOnto(ends->first, ends->second, request.cursor);
            if(distance(foot, request.cursor) <= lineRadius) {
                add(SnapKind::OnLine, SnapSubject::PlacedPoint, id, foot);
            }
        }

        if(const std::optional<double> radius = pose.curveRadius(id)) {
            const std::optional<Point> centre = pose.curveCentre(id);
            if(centre) {
                const double d = distance(*centre, request.cursor);
                if(d > 0.0 && std::abs(d - *radius) <= lineRadius) {
                    const double k = *radius / d;
                    const Point on{centre->x + (request.cursor.x - centre->x) * k,
                                   centre->y + (request.cursor.y - centre->y) * k};
                    add(SnapKind::OnCircle, SnapSubject::PlacedPoint, id, on);
                }
            }
        }
    }

    // Direction-valued kinds describe the segment being drawn, so they exist
    // only while one is in flight.
    if(request.haveAnchor) {
        const double dx = request.cursor.x - request.anchor.x;
        const double dy = request.cursor.y - request.anchor.y;
        const double length = std::hypot(dx, dy);

        if(length > 0.0) {
            // Projecting onto the axis rather than merely flattening one
            // coordinate: the correction has to be the smallest move that makes
            // the relation true, or the ghost jumps in a direction the user did
            // not ask for.
            if(directionGap(dx, dy, 1.0, 0.0) <= policy.angleTolerance) {
                add(SnapKind::Horizontal, SnapSubject::PlacedSegment, EntityId(),
                    Point{request.cursor.x, request.anchor.y});
            }
            if(directionGap(dx, dy, 0.0, 1.0) <= policy.angleTolerance) {
                add(SnapKind::Vertical, SnapSubject::PlacedSegment, EntityId(),
                    Point{request.anchor.x, request.cursor.y});
            }

            // Parallel and perpendicular are measured against segments already
            // in the document. Axis-aligned references are skipped: a segment
            // parallel to a horizontal one is horizontal, and declaring the
            // derived relation instead of the plain one buries the intent.
            for(const EntityRecord &e : doc.entities().records()) {
                // The same role check the point-valued kinds make above.
                // Construction geometry constrains normally but does not
                // attract, and a construction segment offering to make the
                // placement parallel to it is exactly the magnet the policy
                // exists to prevent.
                if(!policy.snapToConstruction && e.role == Role::Construction) continue;
                const auto ends = pose.segment(e.id);
                if(!ends) continue;
                const double rx = ends->second.x - ends->first.x;
                const double ry = ends->second.y - ends->first.y;
                if(std::hypot(rx, ry) == 0.0) continue;
                const bool axisAligned = directionGap(rx, ry, 1.0, 0.0) <= policy.angleTolerance ||
                                         directionGap(rx, ry, 0.0, 1.0) <= policy.angleTolerance;
                if(axisAligned) continue;

                const double along = directionGap(dx, dy, rx, ry);
                if(along <= policy.angleTolerance) {
                    const double k = (dx * rx + dy * ry) / (rx * rx + ry * ry);
                    add(SnapKind::Parallel, SnapSubject::PlacedSegment, e.id,
                        Point{request.anchor.x + rx * k, request.anchor.y + ry * k});
                } else if(std::abs(along - 90.0) <= policy.angleTolerance) {
                    const double px = -ry, py = rx;
                    const double k = (dx * px + dy * py) / (px * px + py * py);
                    add(SnapKind::Perpendicular, SnapSubject::PlacedSegment, e.id,
                        Point{request.anchor.x + px * k, request.anchor.y + py * k});
                }
            }
        }
    }

    // Grid last and lowest: a placement aid that declares nothing. It only ever
    // wins when nothing else captured the cursor, which is exactly its job.
    if(policy.gridEnabled && policy.gridStep > 0.0) {
        const Point onGrid{std::round(request.cursor.x / policy.gridStep) * policy.gridStep,
                           std::round(request.cursor.y / policy.gridStep) * policy.gridStep};
        if(pixels(onGrid, request.cursor) <= policy.pointRadius) {
            add(SnapKind::Grid, SnapSubject::PlacedPoint, EntityId(), onGrid);
        }
    }

    // Ranked deterministically. Ties break on kind and then on target id, never
    // on discovery order, so the same document and cursor always produce the
    // same ranking — inspectable is a stated requirement, and a ranking that
    // depended on iteration order would not be.
    std::stable_sort(result.candidates.begin(), result.candidates.end(),
                     [](const SnapCandidate &a, const SnapCandidate &b) {
                         if(a.score != b.score) return a.score > b.score;
                         if(a.kind != b.kind) return a.kind < b.kind;
                         return a.target < b.target;
                     });

    // The placement is the best candidate's. Direction-valued and point-valued
    // kinds can disagree about where the point goes; the ranking already said
    // which one the user meant, and quietly averaging them would produce a
    // position that satisfies neither.
    if(!result.candidates.empty()) result.placement = result.candidates.front().placement;
    return result;
}

std::optional<ConstraintRecord> constraintFor(const SnapCandidate &candidate,
                                              const PlacementSubjects &placed) {
    const SnapKindInfo &info = snapInfo(candidate.kind);
    if(!info.commitsConstraint) return std::nullopt;

    if(candidate.subject == SnapSubject::PlacedSegment) {
        if(!placed.segment.valid()) return std::nullopt;
        ConstraintRecord r;
        r.kind = info.constraint;
        r.operands[0] = placed.segment;
        if(candidate.target.valid()) r.operands[1] = candidate.target;
        return r;
    }

    if(placed.point.valid()) {
        ConstraintRecord r;
        r.kind = info.constraint;
        r.operands[0] = placed.point;
        if(candidate.target.valid()) r.operands[1] = candidate.target;
        return r;
    }

    // The placement put no point here, but a curve may still pass through.
    // Snapping a circle's rim onto an existing vertex is a real declaration and
    // the obvious wrong answer is to bind it to whatever point the tool did
    // create — for a circle that is the centre, and the circle would teleport
    // so its centre sat on the vertex the rim touched.
    //
    // Only the endpoint kind reinterprets, because only it names an existing
    // point, and point-on-curve is a relation between a point and a curve. The
    // rest name a segment or another curve, and what the user would mean by
    // those is tangency — a relation about whole curves rather than about this
    // position, and not one this gesture has evidence for. They declare
    // nothing, which is the honest answer, not an oversight.
    if(placed.curve.valid() && candidate.kind == SnapKind::Endpoint &&
       candidate.target.valid()) {
        ConstraintRecord r;
        r.kind = ConstraintKind::PointOnCircle;
        r.operands[0] = candidate.target;  // the existing point
        r.operands[1] = placed.curve;
        return r;
    }
    return std::nullopt;
}

std::optional<ConstraintKind> declaredKind(const SnapCandidate &candidate, PlacementRoles roles) {
    // Stand-in ids for entities the placement has not created yet. Only the
    // shape of the answer matters here, never the ids in it, and running the
    // real resolver is what keeps the ghost and the commit from drifting apart.
    PlacementSubjects hypothetical;
    if(roles.point) hypothetical.point = EntityId(1);
    if(roles.segment) hypothetical.segment = EntityId(2);
    if(roles.curve) hypothetical.curve = EntityId(3);

    const std::optional<ConstraintRecord> r = constraintFor(candidate, hypothetical);
    if(!r) return std::nullopt;
    return r->kind;
}

}  // namespace paroculus
