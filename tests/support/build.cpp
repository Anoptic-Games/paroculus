#include "support/build.h"

namespace paroculus::test {

EntityId addPoint(Document &doc, double x, double y) {
    EntityRecord r;
    r.kind = EntityKind::Point;
    r.seeds = {x, y};
    const CommandResult result = doc.apply(AddRecord<EntityRecord>{r});
    return result.ok() ? EntityId(result.allocated) : EntityId();
}

EntityId addSegment(Document &doc, EntityId a, EntityId b) {
    EntityRecord r;
    r.kind = EntityKind::Segment;
    r.points = {a, b, EntityId()};
    const CommandResult result = doc.apply(AddRecord<EntityRecord>{r});
    return result.ok() ? EntityId(result.allocated) : EntityId();
}

EntityId addCircle(Document &doc, EntityId centre, double radius) {
    EntityRecord r;
    r.kind = EntityKind::Circle;
    r.points = {centre, EntityId(), EntityId()};
    r.seeds = {radius, 0.0};
    const CommandResult result = doc.apply(AddRecord<EntityRecord>{r});
    return result.ok() ? EntityId(result.allocated) : EntityId();
}

ConstraintId addConstraint(Document &doc, ConstraintKind kind, std::vector<EntityId> operands,
                           Slot value) {
    ConstraintRecord r;
    r.kind = kind;
    r.value = std::move(value);
    for(size_t i = 0; i < operands.size() && i < MAX_OPERANDS; i++) r.operands[i] = operands[i];
    const CommandResult result = doc.apply(AddRecord<ConstraintRecord>{r});
    return result.ok() ? ConstraintId(result.allocated) : ConstraintId();
}

}  // namespace paroculus::test
