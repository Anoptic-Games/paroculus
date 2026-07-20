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
#include "core/glyphs.h"
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
    // What shape the tool's preview is. A circle gesture previewed as a chord
    // would be a preview of something the commit will not produce.
    enum class GhostShape : uint8_t { Line, Circle, Rectangle, Arc };
    bool ghostActive = false;
    GhostShape ghostShape = GhostShape::Line;
    Point ghostFrom;
    Point ghostTo;
    // Arc ghosts only.
    Point ghostCentre;
    double ghostRadius = 0.0;
    double ghostStart = 0.0;
    double ghostSweep = 0.0;

    // Where a speculatively previewed relation would put the drawing, as spans
    // overlaying the pose. Empty when nothing is being previewed.
    //
    // This is the ghost the catalogue is learnable by: the user hovers a
    // relation and sees the geometry move to where committing it would put it,
    // without the document being touched. The spans come from a solve on a
    // forked context, so there is nothing here to keep in step and nothing to
    // undo if the user hovers away.
    //
    // Only entities that would actually move are drawn from it. Ghosting an
    // unmoved drawing would double every line on screen and say nothing, and
    // most impositions are movement-free by design.
    std::vector<SeedSpan> ghostPose;

    // The constraint marks to draw, already reduced to the visible set. Render
    // draws what it is given: deciding which relations matter this frame needs
    // selection, hover and a density budget, none of which are raster concerns.
    std::vector<GlyphMark> glyphs;

    // Document-space spacing of the placement grid, or zero for no grid.
    //
    // Comes from the snap policy by way of the shell, never from a constant
    // here. A drawn grid is a promise about where a click lands, so render
    // holding its own step means changing the policy makes the grid lie — and
    // the lie is silent, because both numbers are separately plausible. Zero
    // draws nothing, which is the right answer for a caller that says nothing:
    // no grid is honest, a default one is a guess about someone else's policy.
    double gridStep = 0.0;
};

// A framing that fits everything the pose can place, with a margin. Stage 3's
// stand-in for view state the user controls; the shell owns pan and zoom on top
// of it.
// width, height: viewport pixels, both > 0.
ViewTransform fitView(const Pose &pose, int width, int height);

// The framing the demo shipped with, kept so a document that places nothing
// still has a sensible view.
ViewTransform defaultView(int width, int height);

// The view the user is looking through: a fitted framing with a pan and a zoom
// over it.
//
// Beside fitView rather than in the shell because the composition is arithmetic,
// and arithmetic inside a Qt event handler is arithmetic no headless test can
// reach — the same reason keyboard resolution lives in the registry rather than
// in a key handler. The shell still owns what the view currently *is*; this owns
// how the parts compose into a transform.
struct ViewState {
    // The fitted framing. Pan and zoom ride on top of it and never replace it.
    ViewTransform base;
    // Screen pixels, and the outermost term of the composition.
    Eigen::Vector2d pan = Eigen::Vector2d::Zero();
    double zoom = 1.0;

    // Whether `base` has been fitted and latched. Clear it to ask for a re-fit;
    // nothing else may. See frameOnce.
    bool framed = false;

    // The composed document-to-screen transform at this viewport size. Pan
    // being outermost means changing it translates the result by exactly that
    // many pixels, which is what makes an anchored zoom a subtraction.
    ViewTransform transform(double width, double height) const;

    // Fits the framing to what `pose` can place, unless it is already latched.
    //
    // Once, because a framing re-derived from the document makes every edit a
    // potential re-frame: drawing more geometry moved the window under the
    // cursor mid-gesture, and any pixel-calibrated tolerance changed meaning as
    // it jumped. Growing a sketch is not a request to look somewhere else.
    //
    // sizeIsReal: the caller's word that this viewport is one worth keeping a
    //   framing from. A shell item has no size during construction, and a
    //   framing fitted against 1x1 must stay provisional rather than latch.
    void frameOnce(const Pose &pose, int width, int height, bool sizeIsReal);

    // Zooms to `factor`, holding the document point under `cursor` where it is
    // and adjusting the pan to pay for it. Anchoring on the viewport centre
    // instead slides whatever is being examined away exactly as it is magnified,
    // so reaching it costs a zoom and then a pan. No-op for a factor that does
    // not change the zoom, which is what a wheel event at the clamp produces.
    void zoomAt(const Eigen::Vector2d &cursor, double factor, double width, double height);
};

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
