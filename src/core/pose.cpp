#include "core/pose.h"

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

}  // namespace paroculus
