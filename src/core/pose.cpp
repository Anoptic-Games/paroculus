#include "core/pose.h"

#include <cmath>

namespace paroculus {

void Pose::overlay(std::span<const SeedSpan> spans) {
    for(const SeedSpan &s : spans) overlay_[s.entity] = s.seeds;
}

void Pose::clearOverlay() { overlay_.clear(); }

const std::array<double, MAX_ENTITY_PARAMS> *Pose::valuesOf(EntityId id) const {
    const auto it = overlay_.find(id);
    if(it != overlay_.end()) return &it->second;
    const EntityRecord *e = doc_->entities().find(id);
    return e == nullptr ? nullptr : &e->seeds;
}

std::optional<Point> Pose::point(EntityId id) const {
    const EntityRecord *e = doc_->entities().find(id);
    if(e == nullptr || e->kind != EntityKind::Point) return std::nullopt;
    const auto *values = valuesOf(id);
    if(values == nullptr) return std::nullopt;
    return Point{(*values)[0], (*values)[1]};
}

std::optional<double> Pose::radius(EntityId id) const {
    const EntityRecord *e = doc_->entities().find(id);
    if(e == nullptr || e->kind != EntityKind::Circle) return std::nullopt;
    const auto *values = valuesOf(id);
    if(values == nullptr) return std::nullopt;
    return (*values)[0];
}

std::optional<std::pair<Point, Point>> Pose::segment(EntityId id) const {
    const EntityRecord *e = doc_->entities().find(id);
    if(e == nullptr || e->kind != EntityKind::Segment) return std::nullopt;
    const std::optional<Point> a = point(e->points[0]);
    const std::optional<Point> b = point(e->points[1]);
    if(!a || !b) return std::nullopt;
    return std::make_pair(*a, *b);
}

std::optional<Pose::ArcGeometry> Pose::arc(EntityId id) const {
    const EntityRecord *e = doc_->entities().find(id);
    if(e == nullptr || e->kind != EntityKind::Arc) return std::nullopt;
    const std::optional<Point> centre = point(e->points[0]);
    const std::optional<Point> start = point(e->points[1]);
    const std::optional<Point> end = point(e->points[2]);
    if(!centre || !start || !end) return std::nullopt;

    ArcGeometry g;
    g.centre = *centre;
    g.radius = std::hypot(start->x - centre->x, start->y - centre->y);
    g.startAngle = std::atan2(start->y - centre->y, start->x - centre->x);
    const double endAngle = std::atan2(end->y - centre->y, end->x - centre->x);

    // Counter-clockwise from start to end, and never zero: a start and end at
    // the same angle is a full turn, not an absent arc. The solver keeps both
    // endpoints at one radius, so only the angles are read here.
    constexpr double TAU = 6.283185307179586476925;
    g.sweep = endAngle - g.startAngle;
    while(g.sweep <= 0.0) g.sweep += TAU;
    while(g.sweep > TAU) g.sweep -= TAU;
    return g;
}

std::optional<double> Pose::curveRadius(EntityId id) const {
    if(const std::optional<double> r = radius(id)) return r;
    if(const std::optional<ArcGeometry> g = arc(id)) return g->radius;
    return std::nullopt;
}

std::optional<Point> Pose::curveCentre(EntityId id) const {
    const EntityRecord *e = doc_->entities().find(id);
    if(e == nullptr) return std::nullopt;
    if(e->kind == EntityKind::Circle || e->kind == EntityKind::Arc) return point(e->points[0]);
    return std::nullopt;
}

}  // namespace paroculus
