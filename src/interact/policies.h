// Feel policies: the knobs iterative discovery will turn.
//
// Everything here is a replaceable plain function or a plain struct of numbers,
// deliberately. These values are not derivable from anything — they are found
// by using the tool — so the design commitment is not the numbers but the shape:
// each lands behind a stable interface, corpus tests pin the current behaviour,
// and changing one means updating the corpus deliberately rather than watching
// it drift.
//
// All radii here are pixel quantities. Screen-calibrated gestures against
// absolute numbers is architectural: the same sloppy gesture should infer
// coarser relations zoomed out than zoomed in, because that matches intent at
// each zoom. Conversion happens at named boundaries, never implicitly.
#pragma once

#include <cstdint>

#include "core/ids.h"
#include "core/taxonomy.h"

namespace paroculus {

struct HitPolicy {
    // How close the cursor must come, in pixels.
    double pointRadius = 9.0;
    double edgeRadius = 6.0;

    // A drag does not begin until the cursor has travelled this far, so a
    // sloppy click stays a click.
    double dragThreshold = 3.0;

    // How far the cursor may outrun the geometry before the drag counts as
    // saturated and the resisting constraints are worth diagnosing.
    double saturationGap = 6.0;

    // How much closer suppressing a relation must bring the dragged point
    // before that relation is named as resisting — as a fraction of the dragged
    // component's own extent, in document units.
    //
    // The odd one out, and deliberately so. Every other number here describes
    // the hand: pointer precision, which Qt already keeps stable across display
    // scales and which has no business changing when a window is resized. This
    // one describes the drawing, and the regime that motivated it is a shape
    // spanning the whole canvas, where any pixel-denominated number is either
    // noise or unreachable. Scaling against the geometry rather than the
    // viewport also makes it zoom-invariant: the same shape attributes the same
    // way however close the user has zoomed in.
    double attributionFloor = 0.02;
};

// What a hit landed on. Points sit above edges because a vertex is smaller,
// harder to hit, and almost always what the user meant when both are under the
// cursor.
enum class HitKind : uint8_t { Point, Edge };

struct HitCandidate {
    EntityId entity;
    HitKind kind = HitKind::Point;
    Role role = Role::Normal;
    bool selected = false;
    // Pixels from the cursor. Within tolerance by construction.
    double distance = 0.0;
};

// Ranks one candidate against another; the greater priority wins, and distance
// breaks ties. Higher is better.
//
// The policy: points over edges, selected over unselected, construction
// demoted. Construction geometry participates in constraints identically and is
// only *presented* differently, so demoting rather than excluding it is what
// keeps a guide pickable when it is genuinely what the user aimed at.
//
// Replaceable: this is the function stage 3's discovery window is expected to
// rewrite, and the corpus is what will tell us whether the rewrite was an
// improvement.
int defaultHitPriority(const HitCandidate &candidate);

// a, b: candidates within tolerance. Returns true when a should win.
bool hitBeats(const HitCandidate &a, const HitCandidate &b);

}  // namespace paroculus
