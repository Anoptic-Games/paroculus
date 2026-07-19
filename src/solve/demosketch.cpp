#include "solve/demosketch.h"

#include <slvs.h>

#include <unordered_map>
#include <vector>

namespace paroculus {
namespace {

// Group 1 is the fixed base (origin, normal, workplane); group 2 is the sketch
// the solver is allowed to move.
enum : Slvs_hGroup { GROUP_BASE = 1, GROUP_SKETCH = 2 };
enum : Slvs_hEntity { E_ORIGIN = 101, E_NORMAL = 102, E_WORKPLANE = 103 };
// Document entities map onto solver handles starting here, in ID order.
constexpr Slvs_hEntity E_SKETCH_BASE = 200;

constexpr double SEGMENT_A_LENGTH = 120.0;

// core/solution.h declares SolveStatus with these values so the mapping is a
// cast. If SolveSpace ever renumbers, this breaks the build here rather than
// silently mislabelling every diagnostic above the seam.
static_assert(static_cast<int>(SolveStatus::Okay) == SLVS_RESULT_OKAY);
static_assert(static_cast<int>(SolveStatus::Inconsistent) == SLVS_RESULT_INCONSISTENT);
static_assert(static_cast<int>(SolveStatus::DidNotConverge) == SLVS_RESULT_DIDNT_CONVERGE);
static_assert(static_cast<int>(SolveStatus::TooManyUnknowns) == SLVS_RESULT_TOO_MANY_UNKNOWNS);
static_assert(static_cast<int>(SolveStatus::RedundantOkay) == SLVS_RESULT_REDUNDANT_OKAY);

// The taxonomy carries a solver constant per constraint kind so that mapping is
// data rather than a switch free to drift from the other four projections.
// These are the checks that make carrying it safe: a solver renumbering is a
// build error here, not a wrong constraint at runtime.
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

EntityId addPoint(Document &doc, double x, double y) {
    EntityRecord r;
    r.kind = EntityKind::Point;
    r.seeds = {x, y};
    return EntityId(doc.apply(AddRecord<EntityRecord>{r}).allocated);
}

EntityId addSegment(Document &doc, EntityId a, EntityId b) {
    EntityRecord r;
    r.kind = EntityKind::Segment;
    r.points = {a, b, EntityId()};
    return EntityId(doc.apply(AddRecord<EntityRecord>{r}).allocated);
}

void addConstraint(Document &doc, ConstraintKind kind, std::vector<EntityId> operands,
                   Slot value = Slot()) {
    ConstraintRecord r;
    r.kind = kind;
    r.value = std::move(value);
    for(size_t i = 0; i < operands.size() && i < MAX_OPERANDS; i++) r.operands[i] = operands[i];
    doc.apply(AddRecord<ConstraintRecord>{r});
}

}  // namespace

Document demoDocument(double ratio) {
    if(!(ratio > 0.0)) ratio = 1.0;

    Document doc;
    // Seeds are deliberately off-constraint — A is not horizontal, B is not
    // parallel — so a solver that no-opped and echoed its input back would
    // report success and still fail every assertion downstream.
    const EntityId a0 = addPoint(doc, 0.0, 0.0);
    const EntityId a1 = addPoint(doc, 100.0, 18.0);
    const EntityId b0 = addPoint(doc, 0.0, -60.0);
    const EntityId b1 = addPoint(doc, 80.0, -44.0);
    const EntityId segA = addSegment(doc, a0, a1);
    const EntityId segB = addSegment(doc, b0, b1);

    // Eight parameters against eight equations: a fully constrained system, so
    // a correct solve reports dof == 0.
    addConstraint(doc, ConstraintKind::Horizontal, {segA});
    addConstraint(doc, ConstraintKind::PointPointDistance, {a0, a1}, Slot(SEGMENT_A_LENGTH));
    addConstraint(doc, ConstraintKind::Parallel, {segA, segB});
    addConstraint(doc, ConstraintKind::LengthRatio, {segA, segB}, Slot(ratio));
    addConstraint(doc, ConstraintKind::Pin, {a0});
    addConstraint(doc, ConstraintKind::Pin, {b0});
    return doc;
}

// Translates the demo document into an Slvs_System, solves it, and reads the
// results back.
//
// This is deliberately the narrow version: it walks the whole document rather
// than a component, allocates from the heap rather than a per-solve arena, and
// has no seed store, dragged set, speculative context or failure mapping. All
// of that is stage 2, and this exists now only so the demo's geometry comes
// from the declaration layer instead of from literals.
Solution solveDemoSketch(double ratio) {
    const Document doc = demoDocument(ratio);

    std::vector<Slvs_Param> params;
    std::vector<Slvs_Entity> entities;
    std::vector<Slvs_Constraint> constraints;

    // Base: origin at 0,0,0 and an identity-quaternion normal, giving the XY
    // workplane. Both live in GROUP_BASE, so the solver treats them as fixed.
    for(int i = 0; i < 3; i++) params.push_back(Slvs_MakeParam(1 + i, GROUP_BASE, 0.0));
    entities.push_back(Slvs_MakePoint3d(E_ORIGIN, GROUP_BASE, 1, 2, 3));
    params.push_back(Slvs_MakeParam(4, GROUP_BASE, 1.0));
    for(int i = 0; i < 3; i++) params.push_back(Slvs_MakeParam(5 + i, GROUP_BASE, 0.0));
    entities.push_back(Slvs_MakeNormal3d(E_NORMAL, GROUP_BASE, 4, 5, 6, 7));
    entities.push_back(Slvs_MakeWorkplane(E_WORKPLANE, GROUP_BASE, E_ORIGIN, E_NORMAL));

    // Handles are transient per invocation and derived from ID order, which is
    // what makes the translation deterministic: document identity is permanent,
    // solver identity is not, and the map between them is rebuilt every solve.
    std::unordered_map<EntityId, Slvs_hEntity> handle;
    std::unordered_map<EntityId, uint32_t> firstParam;
    Slvs_hEntity nextHandle = E_SKETCH_BASE;
    auto nextParam = static_cast<uint32_t>(params.size() + 1);

    for(const EntityRecord &e : doc.entities().records()) {
        if(e.kind != EntityKind::Point) continue;
        firstParam[e.id] = nextParam;
        params.push_back(Slvs_MakeParam(nextParam, GROUP_SKETCH, e.seeds[0]));
        params.push_back(Slvs_MakeParam(nextParam + 1, GROUP_SKETCH, e.seeds[1]));
        handle[e.id] = nextHandle;
        entities.push_back(
            Slvs_MakePoint2d(nextHandle, GROUP_SKETCH, E_WORKPLANE, nextParam, nextParam + 1));
        nextParam += 2;
        nextHandle++;
    }
    for(const EntityRecord &e : doc.entities().records()) {
        if(e.kind != EntityKind::Segment) continue;
        handle[e.id] = nextHandle;
        entities.push_back(Slvs_MakeLineSegment(nextHandle, GROUP_SKETCH, E_WORKPLANE,
                                                handle.at(e.points[0]), handle.at(e.points[1])));
        nextHandle++;
    }

    Slvs_hConstraint nextConstraint = 1;
    for(const ConstraintRecord &c : doc.constraints().records()) {
        const ConstraintKindInfo &info = constraintInfo(c.kind);

        // Points go in the ptA/ptB slots and everything else in entityA/entityB:
        // that split is slvs.h's, and the taxonomy's operand kinds are what
        // decide which side a given operand lands on.
        Slvs_hEntity pt[2] = {0, 0};
        Slvs_hEntity ent[2] = {0, 0};
        size_t points = 0, others = 0;
        for(size_t i = 0; i < info.operandCount; i++) {
            const EntityRecord *e = doc.entities().find(c.operands[i]);
            if(e == nullptr) continue;
            if(e->kind == EntityKind::Point && points < 2) {
                pt[points++] = handle.at(e->id);
            } else if(others < 2) {
                ent[others++] = handle.at(e->id);
            }
        }

        double value = 0.0;
        if(info.valueArity == 1) {
            // solverValueScale carries the radius-versus-diameter mismatch as
            // data, so the seam has no special case the taxonomy cannot see.
            value = doc.evaluate(c.value).value_or(0.0) * info.solverValueScale;
        }

        constraints.push_back(Slvs_MakeConstraint(nextConstraint++, GROUP_SKETCH,
                                                  info.solverType, E_WORKPLANE, value, pt[0],
                                                  pt[1], ent[0], ent[1]));
    }

    std::vector<Slvs_hConstraint> failed(constraints.size());

    Slvs_System sys{};
    sys.param = params.data();
    sys.params = static_cast<int>(params.size());
    sys.entity = entities.data();
    sys.entities = static_cast<int>(entities.size());
    sys.constraint = constraints.data();
    sys.constraints = static_cast<int>(constraints.size());
    sys.failed = failed.data();
    sys.faileds = static_cast<int>(failed.size());
    sys.calculateFaileds = 1;

    Slvs_Solve(&sys, GROUP_SKETCH);

    Solution sln;
    sln.status = static_cast<SolveStatus>(sys.result);
    sln.dof = sys.dof;

    // Read back by document ID, not by array position.
    auto readPoint = [&](EntityId id) {
        const uint32_t base = firstParam.at(id);
        return Point{params[base - 1].val, params[base].val};
    };
    const auto &records = doc.entities().records();
    sln.a0 = readPoint(records[0].id);
    sln.a1 = readPoint(records[1].id);
    sln.b0 = readPoint(records[2].id);
    sln.b1 = readPoint(records[3].id);
    return sln;
}

}  // namespace paroculus
