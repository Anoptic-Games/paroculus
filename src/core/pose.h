// The geometry currently on screen.
//
// The document stores seeds — the committed pose, and the record of which
// solution branch the user was shown. A drag in flight has newer values living
// in a solve context that has not been committed and may never be. Rendering
// and hit testing both need whichever is current, and they must agree, or the
// user picks one thing and selects another.
//
// So: a pose is the committed seeds, optionally overlaid with in-flight spans.
// It lives in core because render may not see solve, and both layers read it.
#pragma once

#include <span>
#include <unordered_map>

#include "core/document.h"
#include "core/geom.h"

namespace paroculus {

class Pose {
public:
    // Reads the document's committed seeds. The document must outlive the pose.
    explicit Pose(const Document &doc) : doc_(&doc) {}

    // Values that supersede the committed ones. Layers stack in call order and
    // a later layer wins where two carry the same entity, because that is what
    // the pose actually is: stored seeds, the solved result over them, and the
    // in-flight drag over that. Passing an empty span adds nothing.
    void overlay(std::span<const SeedSpan> spans);

    // Drops every overlaid layer, leaving the document's committed seeds.
    void clearOverlay();

    // id: any entity. Returns its centre for a point, or nullopt for a kind
    // that owns no position of its own.
    std::optional<Point> point(EntityId id) const;

    // id: a circle. Returns its radius, or nullopt for anything else.
    std::optional<double> radius(EntityId id) const;

    // Both endpoints of a segment, or nullopt if either is missing.
    std::optional<std::pair<Point, Point>> segment(EntityId id) const;

    // An arc, resolved from its three defining points.
    //
    // An arc owns no parameters of its own: its radius and span are consequences
    // of where its centre and endpoints are, which is what lets the solver move
    // it without anything here having to be kept in step. Sweep is measured
    // counter-clockwise from start to end, matching the solver's convention, and
    // is in (0, 2*pi].
    struct ArcGeometry {
        Point centre;
        double radius = 0.0;
        double startAngle = 0.0;
        double sweep = 0.0;
    };
    std::optional<ArcGeometry> arc(EntityId id) const;

    // The radius of whatever curve `id` is — a circle's own parameter or an
    // arc's derived one. What snapping and hit testing want, since neither
    // cares which kind of curve it is holding.
    std::optional<double> curveRadius(EntityId id) const;
    // And its centre, likewise.
    std::optional<Point> curveCentre(EntityId id) const;

    const Document &document() const { return *doc_; }

private:
    const std::array<double, MAX_ENTITY_PARAMS> *valuesOf(EntityId id) const;

    const Document *doc_;
    std::unordered_map<EntityId, std::array<double, MAX_ENTITY_PARAMS>> overlay_;
};

}  // namespace paroculus
