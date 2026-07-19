#include "solve/demosketch.h"

#include <vector>

#include "solve/solve.h"

namespace paroculus {
namespace {

constexpr double SEGMENT_A_LENGTH = 120.0;

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
    // report success and still fail every residual check downstream.
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

// The whole pipeline in miniature: declarations in, a context forked off them,
// a solve, and solved state out — the same path every interactive edit will
// take once the tools that author documents exist.
Solution solveDemoSketch(double ratio) {
    const Document doc = demoDocument(ratio);
    SolveContext context = SolveContext::forWholeDocument(doc);
    const SolveOutcome outcome = solve(doc, context);

    Solution sln;
    sln.status = outcome.status;
    sln.dof = outcome.dof;
    sln.microseconds = outcome.microseconds;

    // By document identity, in the order the demo declared its points.
    const auto &records = doc.entities().records();
    sln.a0 = context.point(records[0].id).value_or(Point{});
    sln.a1 = context.point(records[1].id).value_or(Point{});
    sln.b0 = context.point(records[2].id).value_or(Point{});
    sln.b1 = context.point(records[3].id).value_or(Point{});
    return sln;
}

}  // namespace paroculus
