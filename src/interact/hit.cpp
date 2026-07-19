#include "interact/hit.h"

#include <algorithm>
#include <cmath>

namespace paroculus {
namespace {

// Distance from p to the segment ab, in whatever units they arrive in.
// Clamped to the segment: a point beyond an endpoint is as far as the endpoint,
// not as far as the infinite line, or every collinear edge in the document
// would hit at once.
double distanceToSegment(const Eigen::Vector2d &p, const Eigen::Vector2d &a,
                         const Eigen::Vector2d &b) {
    const Eigen::Vector2d ab = b - a;
    const double lengthSquared = ab.squaredNorm();
    if(lengthSquared <= 0.0) return (p - a).norm();
    const double t = std::clamp((p - a).dot(ab) / lengthSquared, 0.0, 1.0);
    return (p - (a + t * ab)).norm();
}

Eigen::Vector2d toVec(Point p) { return {p.x, p.y}; }

}  // namespace

int64_t SpatialIndex::cellOf(double v) const {
    return static_cast<int64_t>(std::floor(v / cellSize_));
}

void SpatialIndex::rebuild(const Pose &pose) {
    cells_.clear();

    auto insert = [&](int64_t x, int64_t y, EntityId id) {
        for(Cell &c : cells_) {
            if(c.x == x && c.y == y) {
                if(std::find(c.entities.begin(), c.entities.end(), id) == c.entities.end()) {
                    c.entities.push_back(id);
                }
                return;
            }
        }
        cells_.push_back(Cell{x, y, {id}});
    };

    for(const EntityRecord &e : pose.document().entities().records()) {
        if(const std::optional<Point> p = pose.point(e.id)) {
            insert(cellOf(p->x), cellOf(p->y), e.id);
            continue;
        }
        if(const auto s = pose.segment(e.id)) {
            // Every cell the segment's bounding box touches. Coarse, and coarse
            // is right: the exact test runs on the shortlist anyway.
            const int64_t x0 = cellOf(std::min(s->first.x, s->second.x));
            const int64_t x1 = cellOf(std::max(s->first.x, s->second.x));
            const int64_t y0 = cellOf(std::min(s->first.y, s->second.y));
            const int64_t y1 = cellOf(std::max(s->first.y, s->second.y));
            for(int64_t x = x0; x <= x1; x++) {
                for(int64_t y = y0; y <= y1; y++) insert(x, y, e.id);
            }
            continue;
        }

        // Curves take their whole bounding square, arcs included. An arc's true
        // bound is tighter than its circle's, and computing it would buy a
        // shorter shortlist for a test that is already exact.
        const std::optional<Point> centre = pose.curveCentre(e.id);
        const std::optional<double> radius = pose.curveRadius(e.id);
        if(centre && radius) {
            const int64_t x0 = cellOf(centre->x - *radius);
            const int64_t x1 = cellOf(centre->x + *radius);
            const int64_t y0 = cellOf(centre->y - *radius);
            const int64_t y1 = cellOf(centre->y + *radius);
            for(int64_t x = x0; x <= x1; x++) {
                for(int64_t y = y0; y <= y1; y++) insert(x, y, e.id);
            }
        }
    }
}

std::vector<EntityId> SpatialIndex::near(Point centre, double radius) const {
    const int64_t x0 = cellOf(centre.x - radius);
    const int64_t x1 = cellOf(centre.x + radius);
    const int64_t y0 = cellOf(centre.y - radius);
    const int64_t y1 = cellOf(centre.y + radius);

    std::vector<EntityId> out;
    for(const Cell &c : cells_) {
        if(c.x < x0 || c.x > x1 || c.y < y0 || c.y > y1) continue;
        for(EntityId id : c.entities) out.push_back(id);
    }
    // ID-ordered and deduplicated: a segment spanning several cells appears
    // once, and the caller's iteration is deterministic.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<Hit> hitTestAll(const Pose &pose, const SpatialIndex &index,
                            const ViewTransform &view, const Eigen::Vector2d &screen,
                            const HitPolicy &policy, const std::vector<EntityId> &selected) {
    // The named boundary: pixels in, document units out, once, here.
    const Point cursor = view.toDocument(screen);
    const double pointReach = view.toDocumentLength(policy.pointRadius);
    const double edgeReach = view.toDocumentLength(policy.edgeRadius);
    const double reach = std::max(pointReach, edgeReach);

    const bool isSelected = !selected.empty();
    auto selectedContains = [&](EntityId id) {
        return isSelected && std::find(selected.begin(), selected.end(), id) != selected.end();
    };

    std::vector<HitCandidate> candidates;
    const Eigen::Vector2d cursorVec = toVec(cursor);

    for(EntityId id : index.near(cursor, reach)) {
        const EntityRecord *e = pose.document().entities().find(id);
        if(e == nullptr) continue;

        if(const std::optional<Point> p = pose.point(id)) {
            const double distance = (toVec(*p) - cursorVec).norm();
            if(distance > pointReach) continue;
            candidates.push_back(HitCandidate{
                id, HitKind::Point, e->role, selectedContains(id),
                // Reported back in pixels, because that is the unit a priority
                // policy and a UI both think in.
                distance / std::max(pointReach, 1e-12) * policy.pointRadius});
            continue;
        }

        if(const auto s = pose.segment(id)) {
            const double distance =
                distanceToSegment(cursorVec, toVec(s->first), toVec(s->second));
            if(distance > edgeReach) continue;
            candidates.push_back(HitCandidate{
                id, HitKind::Edge, e->role, selectedContains(id),
                distance / std::max(edgeReach, 1e-12) * policy.edgeRadius});
            continue;
        }

        // A curve is picked by its rim, not by its interior: a circle is an
        // edge, and the space it encloses belongs to whatever fill is there —
        // which is a region, and a different thing to pick.
        const std::optional<Point> centre = pose.curveCentre(id);
        const std::optional<double> radius = pose.curveRadius(id);
        if(centre && radius) {
            const Eigen::Vector2d fromCentre = cursorVec - toVec(*centre);
            const double distance = std::abs(fromCentre.norm() - *radius);
            if(distance > edgeReach) continue;
            // An arc is only where it sweeps. Picking the absent part of a
            // circle is picking something that is not on screen.
            if(const auto g = pose.arc(id)) {
                double offset = std::atan2(fromCentre.y(), fromCentre.x()) - g->startAngle;
                constexpr double TAU = 6.283185307179586476925;
                while(offset < 0.0) offset += TAU;
                while(offset > TAU) offset -= TAU;
                if(offset > g->sweep) continue;
            }
            candidates.push_back(HitCandidate{
                id, HitKind::Edge, e->role, selectedContains(id),
                distance / std::max(edgeReach, 1e-12) * policy.edgeRadius});
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const HitCandidate &a, const HitCandidate &b) { return hitBeats(a, b); });

    std::vector<Hit> hits;
    hits.reserve(candidates.size());
    for(const HitCandidate &c : candidates) hits.push_back(Hit{c.entity, c.kind, c.distance});
    return hits;
}

std::optional<Hit> hitTest(const Pose &pose, const SpatialIndex &index,
                           const ViewTransform &view, const Eigen::Vector2d &screen,
                           const HitPolicy &policy, const std::vector<EntityId> &selected) {
    const std::vector<Hit> hits = hitTestAll(pose, index, view, screen, policy, selected);
    if(hits.empty()) return std::nullopt;
    return hits.front();
}

std::vector<EntityId> marquee(const Pose &pose, const ViewTransform &view,
                              const Eigen::Vector2d &cornerA, const Eigen::Vector2d &cornerB) {
    const Point a = view.toDocument(cornerA);
    const Point b = view.toDocument(cornerB);
    const double minX = std::min(a.x, b.x), maxX = std::max(a.x, b.x);
    const double minY = std::min(a.y, b.y), maxY = std::max(a.y, b.y);

    auto inside = [&](Point p) {
        return p.x >= minX && p.x <= maxX && p.y >= minY && p.y <= maxY;
    };

    std::vector<EntityId> out;
    for(const EntityRecord &e : pose.document().entities().records()) {
        if(const std::optional<Point> p = pose.point(e.id)) {
            if(inside(*p)) out.push_back(e.id);
            continue;
        }
        if(const auto s = pose.segment(e.id)) {
            // Wholly inside: a marquee that grabs what it merely grazes is how
            // a selection ends up holding things the user never saw.
            if(inside(s->first) && inside(s->second)) out.push_back(e.id);
        }
    }
    return out;  // already ID-ordered, the entity table is
}

}  // namespace paroculus
