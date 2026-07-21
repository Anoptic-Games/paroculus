#include "solve/solve.h"

#include <slvs.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>

#include "core/composition.h"
#include "solve/arena.h"

namespace paroculus {
namespace {

// Guards the vendored solver's file-global scratch. Only taken while sharing is
// active — the count is zero on the synchronous-only path, so the branch is never
// entered and the hot drag path is byte-identical to before.
std::mutex g_solverMutex;
std::atomic<int> g_solverSharing{0};


// Group 1 is the fixed base — origin, normal, workplane — which the solver may
// not move. Group 2 is the sketch it may.
enum : Slvs_hGroup { GROUP_BASE = 1, GROUP_SKETCH = 2 };
enum : Slvs_hEntity { E_ORIGIN = 1, E_NORMAL = 2, E_WORKPLANE = 3, E_SKETCH_BASE = 16 };
enum : Slvs_hParam { P_BASE = 1, P_SKETCH_BASE = 8 };

// core/solution.h declares SolveStatus with these values so mapping is a cast.
// A solver renumbering breaks the build here rather than silently mislabelling
// every diagnostic above the seam.
static_assert(static_cast<int>(SolveStatus::Okay) == SLVS_RESULT_OKAY);
static_assert(static_cast<int>(SolveStatus::Inconsistent) == SLVS_RESULT_INCONSISTENT);
static_assert(static_cast<int>(SolveStatus::DidNotConverge) == SLVS_RESULT_DIDNT_CONVERGE);
static_assert(static_cast<int>(SolveStatus::TooManyUnknowns) == SLVS_RESULT_TOO_MANY_UNKNOWNS);
static_assert(static_cast<int>(SolveStatus::RedundantOkay) == SLVS_RESULT_REDUNDANT_OKAY);

// The taxonomy carries a solver constant per constraint kind so the mapping is
// data rather than a switch free to drift from the other four projections.
// These are the checks that make carrying it safe.
constexpr int32_t solverTypeOf(ConstraintKind k) { return constraintInfo(k).solverType; }
static_assert(solverTypeOf(ConstraintKind::Coincident) == SLVS_C_POINTS_COINCIDENT);
static_assert(solverTypeOf(ConstraintKind::PointOnLine) == SLVS_C_PT_ON_LINE);
static_assert(solverTypeOf(ConstraintKind::PointOnCircle) == SLVS_C_PT_ON_CIRCLE);
static_assert(solverTypeOf(ConstraintKind::Midpoint) == SLVS_C_AT_MIDPOINT);
static_assert(solverTypeOf(ConstraintKind::Horizontal) == SLVS_C_HORIZONTAL);
static_assert(solverTypeOf(ConstraintKind::Vertical) == SLVS_C_VERTICAL);
static_assert(solverTypeOf(ConstraintKind::Parallel) == SLVS_C_PARALLEL);
static_assert(solverTypeOf(ConstraintKind::Perpendicular) == SLVS_C_PERPENDICULAR);
static_assert(solverTypeOf(ConstraintKind::Angle) == SLVS_C_ANGLE);
static_assert(solverTypeOf(ConstraintKind::EqualAngle) == SLVS_C_EQUAL_ANGLE);
static_assert(solverTypeOf(ConstraintKind::PointPointDistance) == SLVS_C_PT_PT_DISTANCE);
static_assert(solverTypeOf(ConstraintKind::PointLineDistance) == SLVS_C_PT_LINE_DISTANCE);
static_assert(solverTypeOf(ConstraintKind::EqualLength) == SLVS_C_EQUAL_LENGTH_LINES);
static_assert(solverTypeOf(ConstraintKind::LengthRatio) == SLVS_C_LENGTH_RATIO);
static_assert(solverTypeOf(ConstraintKind::LengthDifference) == SLVS_C_LENGTH_DIFFERENCE);
static_assert(solverTypeOf(ConstraintKind::SymmetricHorizontal) == SLVS_C_SYMMETRIC_HORIZ);
static_assert(solverTypeOf(ConstraintKind::SymmetricVertical) == SLVS_C_SYMMETRIC_VERT);
static_assert(solverTypeOf(ConstraintKind::SymmetricAboutLine) == SLVS_C_SYMMETRIC_LINE);
static_assert(solverTypeOf(ConstraintKind::Tangent) == SLVS_C_ARC_LINE_TANGENT);
static_assert(solverTypeOf(ConstraintKind::Radius) == SLVS_C_DIAMETER);
static_assert(solverTypeOf(ConstraintKind::EqualRadius) == SLVS_C_EQUAL_RADIUS);
static_assert(solverTypeOf(ConstraintKind::Pin) == SLVS_C_WHERE_DRAGGED);

// One invocation's worth of translation: document identity in, solver handles
// out, everything allocated from an arena that dies with the call.
struct Translation {
    SolveArena arena;

    Slvs_Param *params = nullptr;
    int paramCount = 0;
    Slvs_Entity *entities = nullptr;
    int entityCount = 0;
    Slvs_Constraint *constraints = nullptr;
    int constraintCount = 0;
    Slvs_hParam *dragged = nullptr;
    int draggedCount = 0;
    Slvs_hConstraint *failed = nullptr;

    // Document identity is permanent; these maps are not, and are rebuilt every
    // solve. Nothing above this file ever sees a solver handle.
    std::unordered_map<EntityId, Slvs_hEntity> entityHandle;
    std::unordered_map<EntityId, Slvs_hParam> firstParam;
    std::vector<ConstraintId> constraintOrder;  // solver handle - 1 indexes this
};

// An entity's own-parameter span within the context, or null if it holds none.
// By identity, never by array position — the same rule the readback obeys.
const SeedSpan *spanFor(const SolveContext &context, EntityId id) {
    for(const SeedSpan &s : context.params()) {
        if(s.entity == id) return &s;
    }
    return nullptr;
}

// Whether every operand of `c` is inside the context. A component solve carries
// exactly the relations that bind it and no others, which is what makes a drag
// unable to disturb geometry it is not connected to.
bool fullyInside(const ConstraintRecord &c, const SolveContext &context) {
    const size_t n = boundOperandCount(c);
    for(size_t i = 0; i < n; i++) {
        if(!context.contains(c.operands[i])) return false;
    }
    return true;
}

// Which group an entity's own parameters belong to.
//
// Locking a layer means "this does not move", and in a solver world that means
// its parameters join the fixed set — the base group, the same one the workplane
// sits in, which Slvs_Solve treats as known. Not a Pin constraint: a pin is a
// relation the user asked for, it appears in the failing set, and it can
// over-constrain. A lock is presentation state and must never be able to make a
// system inconsistent; it removes unknowns rather than adding equations.
//
// Derived from the document rather than passed in as an option, because every
// caller would otherwise have to remember — the drag path, the diagnose path,
// each speculative preview — and forgetting produces geometry that slides out
// from under a lock with nothing asserting.
Slvs_hGroup groupFor(const Document &doc, EntityId id) {
    return isLocked(doc, id) ? GROUP_BASE : GROUP_SKETCH;
}

// Whether every operand of `c` has all of its parameters locked.
//
// Such a constraint has nothing to solve for. Emitting it anyway would hand the
// solver an equation with no unknown to satisfy it, and the verdict would be
// Inconsistent — a lock making a system contradictory, which is exactly what a
// lock must never be able to do. It is already satisfied, by geometry that
// cannot move, so leaving it out changes no answer.
bool allFrozen(const Document &doc, const ConstraintRecord &c) {
    const size_t n = boundOperandCount(c);
    if(n == 0) return false;
    for(size_t i = 0; i < n; i++) {
        if(!isFrozen(doc, c.operands[i])) return false;
    }
    return true;
}

void translate(const Document &doc, const SolveContext &context, const SolveOptions &options,
               Translation &out) {
    // Upper bounds, so the arena is sized once rather than grown.
    const size_t memberCount = context.members().size();
    const size_t maxParams = P_SKETCH_BASE + memberCount * MAX_ENTITY_PARAMS;
    // A circle costs an extra distance entity beyond itself.
    const size_t maxEntities = 4 + memberCount * 2;
    const size_t maxConstraints = doc.constraints().size() + options.extra.size() + 1;

    out.params = out.arena.array<Slvs_Param>(maxParams);
    out.entities = out.arena.array<Slvs_Entity>(maxEntities);
    out.constraints = out.arena.array<Slvs_Constraint>(maxConstraints);
    out.dragged = out.arena.array<Slvs_hParam>(
        options.dragged.empty() ? 1 : options.dragged.size() * MAX_ENTITY_PARAMS);
    out.failed = out.arena.array<Slvs_hConstraint>(maxConstraints);

    // Base: origin at the workplane origin and an identity-quaternion normal,
    // giving the XY plane. Both live in GROUP_BASE, so they are fixed.
    for(int i = 0; i < 3; i++) {
        out.params[out.paramCount++] = Slvs_MakeParam(P_BASE + i, GROUP_BASE, 0.0);
    }
    out.entities[out.entityCount++] = Slvs_MakePoint3d(E_ORIGIN, GROUP_BASE, 1, 2, 3);
    out.params[out.paramCount++] = Slvs_MakeParam(P_BASE + 3, GROUP_BASE, 1.0);
    for(int i = 4; i < 7; i++) {
        out.params[out.paramCount++] = Slvs_MakeParam(P_BASE + i, GROUP_BASE, 0.0);
    }
    out.entities[out.entityCount++] = Slvs_MakeNormal3d(E_NORMAL, GROUP_BASE, 4, 5, 6, 7);
    out.entities[out.entityCount++] =
        Slvs_MakeWorkplane(E_WORKPLANE, GROUP_BASE, E_ORIGIN, E_NORMAL);

    auto nextParam = static_cast<Slvs_hParam>(P_SKETCH_BASE);
    auto nextEntity = static_cast<Slvs_hEntity>(E_SKETCH_BASE);

    // Points first: everything else refers to them, and doing this in ID order
    // is what makes the parameter numbering — and therefore the floating-point
    // arithmetic, and therefore the answer — reproducible run to run.
    for(const SeedSpan &span : context.params()) {
        const EntityRecord *e = doc.entities().find(span.entity);
        if(e == nullptr || e->kind != EntityKind::Point) continue;
        const Slvs_hGroup group = groupFor(doc, span.entity);
        out.firstParam[span.entity] = nextParam;
        out.params[out.paramCount++] = Slvs_MakeParam(nextParam, group, span.seeds[0]);
        out.params[out.paramCount++] = Slvs_MakeParam(nextParam + 1, group, span.seeds[1]);
        out.entityHandle[span.entity] = nextEntity;
        out.entities[out.entityCount++] =
            Slvs_MakePoint2d(nextEntity, group, E_WORKPLANE, nextParam, nextParam + 1);
        nextParam += 2;
        nextEntity++;
    }

    for(EntityId id : context.members()) {
        const EntityRecord *e = doc.entities().find(id);
        if(e == nullptr) continue;
        switch(e->kind) {
            case EntityKind::Point:
                break;  // already emitted

            case EntityKind::Segment: {
                const auto a = out.entityHandle.find(e->points[0]);
                const auto b = out.entityHandle.find(e->points[1]);
                if(a == out.entityHandle.end() || b == out.entityHandle.end()) break;
                out.entityHandle[id] = nextEntity;
                out.entities[out.entityCount++] = Slvs_MakeLineSegment(
                    nextEntity, GROUP_SKETCH, E_WORKPLANE, a->second, b->second);
                nextEntity++;
                break;
            }

            case EntityKind::Circle: {
                const auto centre = out.entityHandle.find(e->points[0]);
                if(centre == out.entityHandle.end()) break;
                // A circle's radius is a parameter of its own, carried through a
                // distance entity, which is why the taxonomy gives circles an
                // ownParamCount of one.
                //
                // Seeded from the context, like every point above and unlike
                // the document record: the context is the parameter store, so
                // reading e->seeds here would re-seed the radius from the
                // committed value on every frame of a drag and would silently
                // discard any speculative context that perturbed it.
                const SeedSpan *span = spanFor(context, id);
                if(span == nullptr) break;
                const Slvs_hGroup group = groupFor(doc, id);
                out.firstParam[id] = nextParam;
                out.params[out.paramCount++] = Slvs_MakeParam(nextParam, group, span->seeds[0]);
                const Slvs_hEntity distance = nextEntity++;
                out.entities[out.entityCount++] =
                    Slvs_MakeDistance(distance, group, E_WORKPLANE, nextParam);
                nextParam++;
                out.entityHandle[id] = nextEntity;
                out.entities[out.entityCount++] = Slvs_MakeCircle(
                    nextEntity, group, E_WORKPLANE, centre->second, E_NORMAL, distance);
                nextEntity++;
                break;
            }

            case EntityKind::Arc: {
                const auto centre = out.entityHandle.find(e->points[0]);
                const auto start = out.entityHandle.find(e->points[1]);
                const auto end = out.entityHandle.find(e->points[2]);
                if(centre == out.entityHandle.end() || start == out.entityHandle.end() ||
                   end == out.entityHandle.end()) {
                    break;
                }
                out.entityHandle[id] = nextEntity;
                out.entities[out.entityCount++] =
                    Slvs_MakeArcOfCircle(nextEntity, GROUP_SKETCH, E_WORKPLANE, E_NORMAL,
                                         centre->second, start->second, end->second);
                nextEntity++;
                break;
            }
        }
    }

    // Document constraints first, then any speculative ones, so a candidate
    // under test is numbered last and is trivially identifiable in the failed
    // set.
    std::vector<const ConstraintRecord *> active;
    for(const ConstraintRecord &c : doc.constraints().records()) active.push_back(&c);
    for(const ConstraintRecord &c : options.extra) active.push_back(&c);

    for(const ConstraintRecord *record : active) {
        const ConstraintRecord &c = *record;
        if(!c.driving) continue;  // a reference measurement reads, it never drives
        if(!fullyInside(c, context)) continue;
        if(allFrozen(doc, c)) continue;
        if(c.id.valid() && std::find(options.suppressed.begin(), options.suppressed.end(),
                                     c.id) != options.suppressed.end()) {
            continue;
        }

        const ConstraintKindInfo &info = constraintInfo(c.kind);
        const size_t bound = boundOperandCount(c);
        Slvs_Constraint sc{};
        sc.h = static_cast<Slvs_hConstraint>(out.constraintOrder.size() + 1);
        sc.group = GROUP_SKETCH;
        // A kind that names its optional reference is a different solver
        // primitive: horizontal against an axis is parallelism to it, vertical
        // is perpendicularity. Data, not a branch the taxonomy cannot see.
        sc.type = bound > info.operandCount ? info.solverTypeReferenced : info.solverType;
        sc.wrkpl = E_WORKPLANE;
        // Which of the kind's forms this is. Tangency reads it to pick the arc
        // end it holds at; zero-filling it made every tangent a tangent at the
        // start and left the other form unreachable.
        sc.other = c.alternative;

        // slvs.h splits operands into point slots and entity slots; the
        // taxonomy's operand kinds are what decide which side each lands on.
        Slvs_hEntity points[2] = {0, 0};
        Slvs_hEntity others[4] = {0, 0, 0, 0};
        size_t pointCount = 0, otherCount = 0;
        bool complete = true;
        for(size_t i = 0; i < bound; i++) {
            const EntityRecord *e = doc.entities().find(c.operands[i]);
            const auto handle = out.entityHandle.find(c.operands[i]);
            if(e == nullptr || handle == out.entityHandle.end()) {
                complete = false;
                break;
            }
            if(e->kind == EntityKind::Point && pointCount < 2) {
                points[pointCount++] = handle->second;
            } else if(otherCount < 4) {
                others[otherCount++] = handle->second;
            }
        }
        if(!complete) continue;

        sc.ptA = points[0];
        sc.ptB = points[1];
        sc.entityA = others[0];
        sc.entityB = others[1];
        sc.entityC = others[2];
        sc.entityD = others[3];

        if(info.valueArity == 1) {
            // solverValueScale carries the radius-versus-diameter mismatch as
            // data, so the seam grows no special case the taxonomy cannot see.
            sc.valA = doc.evaluate(c.value).value_or(0.0) * info.solverValueScale;
        }

        out.constraints[out.constraintCount++] = sc;
        out.constraintOrder.push_back(c.id);
    }

    for(EntityId id : options.dragged) {
        // A locked entity's parameters are not unknowns, so asking the solver
        // to favour keeping them near the cursor is asking about something it
        // is not solving for.
        if(isLocked(doc, id)) continue;
        const auto first = out.firstParam.find(id);
        if(first == out.firstParam.end()) continue;
        const EntityRecord *e = doc.entities().find(id);
        if(e == nullptr) continue;
        const size_t count = entityInfo(e->kind).ownParamCount;
        for(size_t i = 0; i < count; i++) {
            out.dragged[out.draggedCount++] = first->second + static_cast<Slvs_hParam>(i);
        }
    }
}

}  // namespace

SolveOutcome solve(const Document &doc, SolveContext &context, const SolveOptions &options) {
    SolveOutcome outcome;
    outcome.generation = options.generation;
    if(context.empty()) {
        // Nothing to solve is a solved system with nothing free, not a failure.
        outcome.status = SolveStatus::Okay;
        outcome.dof = 0;
        return outcome;
    }

    const auto started = std::chrono::steady_clock::now();

    Translation t;
    translate(doc, context, options, t);
    const auto translated = std::chrono::steady_clock::now();

    Slvs_System sys{};
    sys.param = t.params;
    sys.params = t.paramCount;
    sys.entity = t.entities;
    sys.entities = t.entityCount;
    sys.constraint = t.constraints;
    sys.constraints = t.constraintCount;
    sys.dragged = t.dragged;
    sys.ndragged = t.draggedCount;
    sys.failed = t.failed;
    sys.faileds = t.constraintCount;
    sys.calculateFaileds = options.diagnoseFailures ? 1 : 0;

    // Only the solver call is guarded — translation above and readback below use
    // per-call storage and stay concurrent. The lock is skipped entirely unless a
    // scheduler has begun sharing, so nothing crosses threads on the pure path.
    if(g_solverSharing.load(std::memory_order_acquire) > 0) {
        std::lock_guard<std::mutex> lock(g_solverMutex);
        Slvs_Solve(&sys, GROUP_SKETCH);
    } else {
        Slvs_Solve(&sys, GROUP_SKETCH);
    }

    outcome.status = static_cast<SolveStatus>(sys.result);
    outcome.dof = sys.dof;

    // Map the blamed handles back to document identity. Solver status and
    // solver handles both stop here; what travels upward is a set of
    // constraints the user can select and walk.
    for(int i = 0; i < sys.faileds; i++) {
        const auto index = static_cast<size_t>(t.failed[i]);
        if(index >= 1 && index <= t.constraintOrder.size()) {
            outcome.failed.push_back(t.constraintOrder[index - 1]);
        }
    }

    // Read back by document identity, never by array position. Slvs_Solve
    // mutates param[] in place, so the solved values come out of the same
    // array the seeds went into.
    std::unordered_map<Slvs_hParam, double> solved;
    solved.reserve(static_cast<size_t>(sys.params));
    for(int i = 0; i < sys.params; i++) solved[t.params[i].h] = t.params[i].val;

    for(SeedSpan &span : context.params()) {
        const auto first = t.firstParam.find(span.entity);
        if(first == t.firstParam.end()) continue;
        const EntityRecord *e = doc.entities().find(span.entity);
        if(e == nullptr) continue;
        const size_t count = entityInfo(e->kind).ownParamCount;
        for(size_t i = 0; i < count; i++) {
            const auto it = solved.find(first->second + static_cast<Slvs_hParam>(i));
            if(it != solved.end()) span.seeds[i] = it->second;
        }
    }

    outcome.arenaBytes = t.arena.bytesAllocated();
    outcome.translateMicroseconds =
        std::chrono::duration<double, std::micro>(translated - started).count();
    outcome.microseconds =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started)
            .count();
    return outcome;
}

void beginSolverSharing() { g_solverSharing.fetch_add(1, std::memory_order_release); }
void endSolverSharing() { g_solverSharing.fetch_sub(1, std::memory_order_release); }

}  // namespace paroculus
