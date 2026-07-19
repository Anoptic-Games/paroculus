#include "interact/drag.h"

#include <cmath>

namespace paroculus {
namespace {

// Attribution costs one solve per candidate, so it is bounded. A component with
// more relations than this is past the point where highlighting all of them
// would tell the user anything anyway.
constexpr int MAX_RESISTANCE_PROBES = 32;

// The diagonal of the dragged component's bounding box, in document units.
// What the attribution floor is measured against, so "a meaningful amount of
// travel" means the same thing for a shape spanning the canvas as for one
// tucked in a corner, at any zoom.
//
// context: a live solve context. Returns 0 for a component with no extent —
// a lone point — which the caller substitutes for.
double componentExtent(const SolveContext &context, const Document &doc) {
    bool any = false;
    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
    for(const SeedSpan &span : context.params()) {
        const EntityRecord *e = doc.entities().find(span.entity);
        if(e == nullptr || e->kind != EntityKind::Point) continue;
        const double x = span.seeds[0], y = span.seeds[1];
        if(!any) {
            minX = maxX = x;
            minY = maxY = y;
            any = true;
            continue;
        }
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
    }
    return any ? std::hypot(maxX - minX, maxY - minY) : 0.0;
}

}  // namespace

std::optional<DragSession> DragSession::begin(const Document &doc, const Topology &topology,
                                              EntityId grabbed, const HitPolicy &policy) {
    const EntityRecord *e = doc.entities().find(grabbed);
    if(e == nullptr) return std::nullopt;
    // Only entities owning parameters can be dragged directly. A segment is
    // dragged by its endpoints, which is also what the user is pointing at.
    if(entityInfo(e->kind).ownParamCount == 0) return std::nullopt;

    DragSession session;
    session.context_ = SolveContext::forComponent(doc, topology, grabbed);
    if(session.context_.empty()) return std::nullopt;
    session.grabbed_ = grabbed;
    session.policy_ = policy;
    return session;
}

DragUpdate DragSession::update(const Document &doc, const Viewport &viewport, Point cursor,
                               bool diagnoseResistance) {
    DragUpdate result;

    // The last pose that satisfied the constraints, kept so a failed solve can
    // be rolled back to it. A non-converged solve leaves the parameters at the
    // seeds it was given, and the seed we just wrote is the cursor — so without
    // this the geometry would silently follow the cursor through its own
    // constraints and commit the violation on release.
    const SolveContext lastGood = context_;

    // The target goes in as a seed, not as a constraint. The solver is asked to
    // favour it and free to disregard it, which is exactly what makes an
    // unreachable target saturate instead of failing.
    for(SeedSpan &span : context_.params()) {
        if(span.entity != grabbed_) continue;
        span.seeds[0] = cursor.x;
        span.seeds[1] = cursor.y;
    }

    SolveOptions options;
    options.dragged = {grabbed_};
    options.diagnoseFailures = false;  // the pose is what a frame needs
    options.generation = ++generation_;

    const SolveOutcome outcome = solve(doc, context_, options);
    result.status = outcome.status;
    result.microseconds = outcome.microseconds;

    // A drag that cannot be solved is the strongest form of saturation, not a
    // free one. Roll back to the last legal pose: the geometry stops dead and
    // the cursor runs on. Warm-starting the next frame from a diverged state
    // would poison every frame after it, and committing one on release would
    // write a constraint-violating document.
    const bool diverged = !outcome.ok();
    if(diverged) context_ = lastGood;

    const std::optional<Point> landed = context_.point(grabbed_);
    if(landed) {
        // Measured in pixels, because saturation is a thing the user sees at a
        // zoom level, not a thing the document knows about.
        const Eigen::Vector2d gap =
            viewport.view.toScreen(*landed) - viewport.view.toScreen(cursor);
        result.gap = gap.norm();
        result.saturated = diverged || result.gap > policy_.saturationGap;
    }

    // Attribution, by counterfactual.
    //
    // The obvious approach — pin the point at the cursor and ask the solver
    // what broke — does not work: the solver reports the set to remove to make
    // the system solvable again, and removing the pin always does that, so the
    // pin is all it ever names. The pin is the question, not the answer.
    //
    // So ask the question that actually matters: which relation, if it were not
    // there, would let the point reach? One suppressed solve per candidate,
    // bounded by the constraints binding this component, and only on the frames
    // the caller asks for. That is more work than a single solve and it is what
    // makes the resistance legible rather than merely stiff.
    if(result.saturated && diagnoseResistance && landed) {
        SolveOptions probe;
        probe.dragged = {grabbed_};
        probe.diagnoseFailures = false;

        // How much travel a relation has to buy back before it is worth naming,
        // in document units. A ratio was the wrong shape: freeing a rotational
        // degree of freedom wins the point a roughly fixed distance whatever the
        // cursor does, so as the user pulls further that fixed win shrinks as a
        // *fraction* of the gap and the constraint stops being named — the hard
        // pull loses the very explanation it should be showing.
        const double extent = componentExtent(context_, doc);
        const double heldGap = std::hypot(landed->x - cursor.x, landed->y - cursor.y);
        // A component with no extent has nothing to scale against, so how far
        // the drag has run stands in for one.
        const double floor = policy_.attributionFloor * (extent > 0.0 ? extent : heldGap);

        int tested = 0;
        for(const ConstraintRecord &c : doc.constraints().records()) {
            if(!c.driving) continue;
            if(!context_.contains(c.operands[0])) continue;
            if(++tested > MAX_RESISTANCE_PROBES) break;

            SolveContext without = context_;
            for(SeedSpan &span : without.params()) {
                if(span.entity != grabbed_) continue;
                span.seeds[0] = cursor.x;
                span.seeds[1] = cursor.y;
            }
            probe.suppressed = {c.id};

            const SolveOutcome outcome2 = solve(doc, without, probe);
            if(!outcome2.ok()) continue;
            const std::optional<Point> freed = without.point(grabbed_);
            if(!freed) continue;

            // Document units on both sides, because the floor is a fraction of
            // the geometry rather than of the screen. Saturation stays a pixel
            // question; attribution is not one.
            const double freedGap = std::hypot(freed->x - cursor.x, freed->y - cursor.y);
            if(heldGap - freedGap > floor) result.resisting.push_back(c.id);
        }
    }

    // Ripple: did anything land outside what the user can see? The viewport in
    // document units is the inverse-transformed screen rectangle, and the check
    // is against where geometry ended up, not where it started.
    for(const SeedSpan &span : context_.params()) {
        const EntityRecord *e = doc.entities().find(span.entity);
        if(e == nullptr || e->kind != EntityKind::Point) continue;
        // Unchanged parameters cannot have rippled anywhere.
        if(e->seeds == span.seeds) continue;
        if(!viewport.contains(Point{span.seeds[0], span.seeds[1]})) {
            result.rippledOffScreen = true;
            break;
        }
    }

    return result;
}

std::vector<Command> DragSession::commit(const Document &doc) const {
    // Release commits what is on screen: the solved state becomes the new
    // seeds, and because these are ordinary commands they ride the undo journal
    // like any other edit.
    return context_.commitCommands(doc);
}

}  // namespace paroculus
