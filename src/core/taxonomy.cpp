#include "core/taxonomy.h"

namespace paroculus {
namespace {

// The tables are indexed by enum value, so a reordered enum would silently
// mislabel every kind. Cheap to assert, catastrophic to miss.
constexpr bool entityTableIsAligned() {
    for(size_t i = 0; i < ENTITY_KINDS.size(); i++) {
        if(static_cast<size_t>(ENTITY_KINDS[i].kind) != i) return false;
    }
    return true;
}

constexpr bool constraintTableIsAligned() {
    for(size_t i = 0; i < CONSTRAINT_KINDS.size(); i++) {
        if(static_cast<size_t>(CONSTRAINT_KINDS[i].kind) != i) return false;
    }
    return true;
}

constexpr bool snapTableIsAligned() {
    for(size_t i = 0; i < SNAP_KINDS.size(); i++) {
        if(static_cast<size_t>(SNAP_KINDS[i].kind) != i) return false;
    }
    return true;
}

// Operand counts must fit the fixed-size array, and a valued constraint carries
// exactly one slot in v1.
constexpr bool constraintTableIsWellFormed() {
    for(const ConstraintKindInfo &c : CONSTRAINT_KINDS) {
        if(c.operandCount == 0 || c.operandCount > MAX_OPERANDS) return false;
        // The optional tail has to fit beside the required operands, and a kind
        // that has one needs a solver constant for the referenced form —
        // otherwise the document could express a relation the solve drops.
        if(static_cast<size_t>(c.operandCount) + c.optionalOperands > MAX_OPERANDS) return false;
        if((c.optionalOperands == 0) != (c.solverTypeReferenced == 0)) return false;
        // The alternative is one byte on the record and the solver reads it as
        // a small selector, so a kind claiming more forms than that would be
        // claiming something nothing can carry.
        if(c.alternatives > 3) return false;
        if(c.valueArity > 1) return false;
        if(c.name.empty()) return false;
        if(c.solverValueScale == 0.0) return false;
        // Order sensitivity is a question about two slots that accept the same
        // thing. A kind whose required operands are all of different kinds has
        // its roles assigned by type, so claiming the question exists there
        // would send the surface asking something it cannot ask.
        if(c.orderSensitive) {
            bool interchangeable = false;
            for(size_t i = 0; i < c.operandCount && !interchangeable; i++) {
                for(size_t j = i + 1; j < c.operandCount; j++) {
                    if(c.operands[i] == c.operands[j]) interchangeable = true;
                }
            }
            if(!interchangeable) return false;
        }
        // A grouping has to divide the required slots into at least two whole
        // groups, all accepting one operand kind. Groups decided by type are
        // not a question the surface can ask, and a partial group would leave
        // slots belonging to nothing.
        if(c.operandGroupSize > 0) {
            if(c.operandGroupSize < 2) return false;
            if(c.operandCount % c.operandGroupSize != 0) return false;
            if(c.operandCount / c.operandGroupSize < 2) return false;
            if(c.optionalOperands != 0) return false;
            for(size_t i = 1; i < c.operandCount; i++) {
                if(c.operands[i] != c.operands[0]) return false;
            }
        }
        // A frame-referenced kind carries its reference absolutely, through no
        // operand, so it cannot also own an optional reference slot: the two are
        // different answers to "what frame does this mean", and a kind claiming
        // both would be ambiguous about which one the copy rule reads.
        if(c.frameReferenced && c.optionalOperands != 0) return false;
    }
    return true;
}

static_assert(entityTableIsAligned(), "ENTITY_KINDS must be indexed by EntityKind");
static_assert(constraintTableIsAligned(), "CONSTRAINT_KINDS must be indexed by ConstraintKind");
static_assert(snapTableIsAligned(), "SNAP_KINDS must be indexed by SnapKind");
static_assert(constraintTableIsWellFormed(), "CONSTRAINT_KINDS has a malformed row");

}  // namespace

std::optional<EntityKind> entityKindFromName(std::string_view name) {
    for(const EntityKindInfo &e : ENTITY_KINDS) {
        if(e.name == name) return e.kind;
    }
    return std::nullopt;
}

std::optional<ConstraintKind> constraintKindFromName(std::string_view name) {
    for(const ConstraintKindInfo &c : CONSTRAINT_KINDS) {
        if(c.name == name) return c.kind;
    }
    return std::nullopt;
}

std::string_view strengthName(Strength s) {
    switch(s) {
        case Strength::Measure:   return "measure";
        case Strength::Impose:    return "impose";
        case Strength::Reference: return "reference";
    }
    return "impose";
}

bool signatureMatches(ConstraintKind k, std::span<const EntityKind> kinds) {
    const ConstraintKindInfo &info = constraintInfo(k);
    if(kinds.size() != info.operandCount) return false;
    for(size_t i = 0; i < kinds.size(); i++) {
        if(!accepts(info.operands[i], kinds[i])) return false;
    }
    return true;
}

}  // namespace paroculus
