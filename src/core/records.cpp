#include "core/records.h"

namespace paroculus {

// Full-field equality on every record, because undo byte-identity and persist
// round-tripping are asserted against it. A field added to a record and not
// added here would make both properties silently weaker.
bool operator==(const EntityRecord &a, const EntityRecord &b) {
    return a.id == b.id && a.kind == b.kind && a.role == b.role && a.layer == b.layer &&
           a.points == b.points && a.seeds == b.seeds;
}

bool operator==(const ConstraintRecord &a, const ConstraintRecord &b) {
    return a.id == b.id && a.kind == b.kind && a.operands == b.operands &&
           a.value == b.value && a.driving == b.driving;
}

bool operator==(const RegionRecord &a, const RegionRecord &b) {
    return a.id == b.id && a.boundary == b.boundary && a.style == b.style && a.layer == b.layer;
}

bool operator==(const TagRecord &a, const TagRecord &b) {
    return a.id == b.id && a.kind == b.kind && a.entities == b.entities &&
           a.constraints == b.constraints;
}

bool operator==(const StyleRecord &a, const StyleRecord &b) {
    return a.id == b.id && a.name == b.name && a.strokeWidth == b.strokeWidth &&
           a.strokeColor == b.strokeColor && a.fillColor == b.fillColor && a.filled == b.filled;
}

bool operator==(const LayerRecord &a, const LayerRecord &b) {
    return a.id == b.id && a.name == b.name && a.order == b.order && a.visible == b.visible &&
           a.locked == b.locked;
}

bool operator==(const GroupRecord &a, const GroupRecord &b) {
    return a.id == b.id && a.name == b.name && a.members == b.members;
}

bool operator==(const ParameterRecord &a, const ParameterRecord &b) {
    return a.id == b.id && a.name == b.name && a.value == b.value;
}

}  // namespace paroculus
