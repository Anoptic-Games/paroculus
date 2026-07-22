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

    // The most marks one anchor fans before the rest collapse into a single ⋯.
    // A vertex where relations cluster reads as a small fan the eye can count
    // plus a promise of more, rather than a smear of overlapping marks no denser
    // information survives. The overflow mark is pickable and opens the crowd in
    // the inspector, so the cap costs the user nothing but the smear.
    size_t fanLimit = 5;

    // Marks per million square pixels below which the overlay is loose enough to
    // carry mnemonic labels beside the unvalued marks. Labels share the mark
    // budget rather than getting one of their own — two budgets over one overlay
    // is how it becomes unreadable in two ways at once — so they appear only when
    // the overlay is not already crowded and are the first thing to go as it is.
    double labelDensity = 32.0;
};

// Action-surface policy.
//
// Ranking within the transient strip is contextual; placement of the permanent
// furniture is not. These numbers govern the first and say nothing about the
// second — menus and the palette stay in taxonomy order however often a
// relation is used, because a surface whose slots reshuffle with context is a
// surface muscle memory cannot hold.
struct SurfacePolicy {
    // What one prior use of a kind in this document is worth. Small and linear:
    // ranking is a nudge that a user can predict and inspect, not a model of
    // them. No global learned magic.
    double usageWeight = 1.0;
    // Beyond which further uses stop counting, so a kind reached for a hundred
    // times cannot bury one reached for twice. Habit should tilt the list, not
    // freeze it.
    double usageCeiling = 12.0;

    // How many offers the strip carries. The rest stay reachable through the
    // palette, which is the surface that does not rank.
    size_t stripLimit = 8;

    // How far geometry may move and still count as a movement-free imposition,
    // in document units. Not a feel number so much as a numerical one: a solver
    // asked to hold a value it is already holding still walks a Newton step or
    // two, and the last bits move.
    double movementTolerance = 1e-6;

    // How near two segments must be to parallel, in degrees, for imposing
    // parallelism to read as snapping the angle shut rather than as a
    // reorientation. The one imposition that moves geometry on purpose; this is
    // where the surface decides to say so.
    double snapShutDegrees = 8.0;
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
