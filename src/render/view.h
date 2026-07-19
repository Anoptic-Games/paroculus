// The raster layer.
//
// Skia reaches this layer only, linked PRIVATE, so no Skia type appears in any
// header above render — which is what lets the known QQuickPaintedItem shortcut
// be swapped for the GPU path without anything above noticing.
//
// Geometry is drawn in document space through the view transform; everything
// that is an adorner — handles, selection marks, the marquee — is drawn in
// screen space at a fixed pixel size, so it does not scale with zoom. That
// split is the same one hit testing uses, and both must agree or the user picks
// one thing and selects another.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/geom.h"
#include "core/pose.h"

namespace paroculus {

// What the interaction layer wants shown on top of the geometry. Transient
// per frame; never a second source of truth about the document.
struct Adornment {
    std::vector<EntityId> selected;
    EntityId hovered;
    // Constraints currently resisting a saturated drag. Their operands tint, so
    // resistance has attribution rather than being merely stiff.
    std::vector<ConstraintId> resisting;

    bool marqueeActive = false;
    Eigen::Vector2d marqueeFrom = Eigen::Vector2d::Zero();
    Eigen::Vector2d marqueeTo = Eigen::Vector2d::Zero();

    // The rubber band a creation tool is showing, in document space, because it
    // is geometry-to-be rather than an adorner. Preview shows truth: this is
    // where commit would put it, so it is drawn as the thing it will become.
    bool ghostActive = false;
    Point ghostFrom;
    Point ghostTo;
};

// A framing that fits everything the pose can place, with a margin. Stage 3's
// stand-in for view state the user controls; the shell owns pan and zoom on top
// of it.
// width, height: viewport pixels, both > 0.
ViewTransform fitView(const Pose &pose, int width, int height);

// The framing the demo shipped with, kept so a document that places nothing
// still has a sensible view.
ViewTransform defaultView(int width, int height);

// Paints the document into a caller-owned BGRA8888 premultiplied buffer.
// pixels must have at least height*rowBytes bytes; rowBytes at least 4*width.
// No-ops on a degenerate viewport or a null buffer.
//
// width, height: the buffer's own dimensions, in device pixels.
// deviceScale: device pixels per logical pixel, > 0. Everything else here —
//   the view transform, the adornment's screen coordinates, and every cosmetic
//   size below — speaks logical pixels, matching the units hit testing and the
//   feel policies use. This is the one place the two meet, so a HiDPI display
//   rasterises at its true resolution without a pixel ratio leaking upward into
//   interact, where a handle radius must stay a property of the hand.
void renderDocument(const Pose &pose, const ViewTransform &view, const Adornment &adornment,
                    uint8_t *pixels, int width, int height, size_t rowBytes,
                    double deviceScale = 1.0);

}  // namespace paroculus
