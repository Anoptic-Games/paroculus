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
#include <optional>

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

// Inference policy.
//
// A snap is not a coordinate correction, it is a constraint candidate that
// placement commits — so these numbers decide what gets *declared*, not merely
// where a point lands. That makes them the highest-stakes feel numbers in the
// file: too generous and documents rigidify by helpfulness, too mean and
// freehand drawing never reaches the parametric layer at all.
struct SnapPolicy {
    // Capture radii in logical pixels, for the same reason the hit radii are:
    // they describe how precisely a hand can aim.
    double pointRadius = 12.0;
    double lineRadius = 8.0;

    // How near an axis or a reference direction counts, in degrees. Angular
    // rather than pixel because a direction is scale-free — the same 3° looks
    // like two pixels on a short segment and twenty on a long one, and it is
    // the intent that is the same, not the distance.
    double angleTolerance = 4.0;

    // Placement only, never a constraint: a document where every point is
    // grid-pinned is rigidity by helpfulness. Document units.
    double gridStep = 20.0;
    bool gridEnabled = true;

    // Construction geometry participates in constraints identically and is only
    // presented differently — but it must not *attract* by default. An arc
    // leaves its centre behind as a construction point, and a sketch full of
    // arcs would otherwise be a sketch full of magnets nobody aimed at.
    bool snapToConstruction = false;

    // Ranking weights. Tier dominates, so a coincidence never loses to a
    // parallel that happens to be nearer; closeness settles the rest.
    double tierWeight = 1000.0;
    double closenessWeight = 10.0;
    // Recent choices in this document weigh in, which is what "contextual and
    // document-local" means — a small, inspectable, deterministic nudge rather
    // than anything learned.
    double recencyBonus = 25.0;
    size_t recentDepth = 8;
};

// Overlay policy.
//
// Glyph overload is real at scale: a document with a thousand relations cannot
// draw a thousand marks and still be readable, and the answer is not to let
// each mark decide for itself whether it matters. Visibility is a property of
// the overlay as a whole — a budget, filled by whatever is most worth seeing
// this frame.
struct GlyphPolicy {
    // Marks per million square pixels of viewport. Density rather than a flat
    // count is what makes this zoom-dependent in the right direction: zooming
    // out brings more geometry on screen without bringing more clutter.
    double density = 90.0;
    // A rail, not the mechanism. Nothing should ever reach it.
    size_t hardCap = 400;

    // What survives the budget. Selection first, because a mark the user is
    // looking at is the one they asked about.
    double selectedWeight = 1000.0;
    double hoveredWeight = 700.0;
    double freshWeight = 500.0;
    // Nearer the cursor takes the remainder.
    double proximityWeight = 1.0;
};

// Ranks one candidate against another. Higher wins.
//
// tier: the candidate's commit tier. correction: how far the placement had to
// move, in pixels. recentRank: 0 when this kind was the most recently committed
// in this document, growing with age, or absent when it has not been used.
double snapScore(const SnapPolicy &policy, SnapTier tier, double correction,
                 std::optional<size_t> recentRank);

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
