#include "core/records.h"

namespace paroculus {

namespace {

// Spelled once, read by persist in both directions. Names rather than ordinals
// on the wire, so inserting an operation later cannot silently reinterpret
// files written before it existed.
struct CompositeOpName {
    CompositeOp op;
    const char *name;
};

constexpr CompositeOpName COMPOSITE_OP_NAMES[] = {
    {CompositeOp::Outline, "outline"},
    {CompositeOp::Union, "union"},
    {CompositeOp::Intersect, "intersect"},
    {CompositeOp::Subtract, "subtract"},
};

}  // namespace

const char *compositeOpName(CompositeOp op) {
    for(const CompositeOpName &row : COMPOSITE_OP_NAMES) {
        if(row.op == op) return row.name;
    }
    return "outline";
}

bool compositeOpFromName(std::string_view name, CompositeOp &out) {
    for(const CompositeOpName &row : COMPOSITE_OP_NAMES) {
        if(name == row.name) {
            out = row.op;
            return true;
        }
    }
    return false;
}

// Full-field equality on every record, because undo byte-identity and persist
// round-tripping are asserted against it. A field added to a record and not
// added here would make both properties silently weaker.
bool operator==(const EntityRecord &a, const EntityRecord &b) {
    return a.id == b.id && a.kind == b.kind && a.role == b.role && a.layer == b.layer &&
           a.style == b.style && a.points == b.points && a.seeds == b.seeds;
}

bool operator==(const ConstraintRecord &a, const ConstraintRecord &b) {
    return a.id == b.id && a.kind == b.kind && a.operands == b.operands &&
           a.value == b.value && a.driving == b.driving && a.alternative == b.alternative;
}

bool operator==(const RegionRecord &a, const RegionRecord &b) {
    return a.id == b.id && a.op == b.op && a.boundary == b.boundary &&
           a.operands == b.operands && a.style == b.style && a.layer == b.layer &&
           a.z == b.z && a.punch == b.punch;
}

bool operator==(const TagRecord &a, const TagRecord &b) {
    return a.id == b.id && a.kind == b.kind && a.entities == b.entities &&
           a.constraints == b.constraints;
}

bool operator==(const StyleRecord &a, const StyleRecord &b) {
    return a.id == b.id && a.name == b.name && a.strokeWidth == b.strokeWidth &&
           a.opacity == b.opacity && a.strokeColor == b.strokeColor &&
           a.fillColor == b.fillColor && a.filled == b.filled;
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
