// The document's record types.
//
// Every record is a plain value: copyable, equality-comparable, and carrying a
// persistent ID. That uniformity is what lets one command shape (add, remove,
// set-whole-record) cover every table, which in turn is what makes inversion
// exact and undo byte-identity a property rather than a hope.
//
// Each record declares `IdType` so the generic table and the generic commands
// can name it without a traits class.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/ids.h"
#include "core/slots.h"
#include "core/taxonomy.h"

namespace paroculus {

// One entity's own parameter values.
//
// Seeds are the persistent record of which solution branch the user was shown.
// Constraint systems admit multiple solutions — reflections, elbow-up versus
// elbow-down — and Newton converges to the one nearest its seed, so which
// branch is on screen is part of the document, not an artefact of solving. The
// same span type carries seeds into a solve, results out of one, and the
// before/after pair in an undo record.
struct SeedSpan {
    EntityId entity;
    std::array<double, MAX_ENTITY_PARAMS> seeds{};

    friend bool operator==(const SeedSpan &a, const SeedSpan &b) {
        return a.entity == b.entity && a.seeds == b.seeds;
    }
    friend bool operator!=(const SeedSpan &a, const SeedSpan &b) { return !(a == b); }
};

// Geometry. A point owns its coordinates as seeds; every other kind is defined
// by the points it refers to, plus a radius seed for a circle.
//
// Seeds are the persistent record of which solution branch the user was shown.
// Constraint systems admit multiple solutions, and Newton converges to the one
// nearest its seed, so a document without seeds can reopen into a mirror
// solution that satisfies every constraint and still betrays the user. Stage 1
// stores and round-trips them; stage 2 starts writing them from solves.
struct EntityRecord {
    using IdType = EntityId;

    EntityId id;
    EntityKind kind = EntityKind::Point;
    Role role = Role::Normal;
    LayerId layer;
    std::array<EntityId, MAX_ENTITY_POINTS> points{};
    std::array<double, MAX_ENTITY_PARAMS> seeds{};

    friend bool operator==(const EntityRecord &, const EntityRecord &);
    friend bool operator!=(const EntityRecord &a, const EntityRecord &b) { return !(a == b); }
};

// A relation over geometry. `driving` false makes it a reference measurement:
// the same object with a toggle, which is how measurement is promoted to
// intent and demoted again at an over-constraint downgrade.
struct ConstraintRecord {
    using IdType = ConstraintId;

    ConstraintId id;
    ConstraintKind kind = ConstraintKind::Coincident;
    std::array<EntityId, MAX_OPERANDS> operands{};
    Slot value;
    bool driving = true;

    friend bool operator==(const ConstraintRecord &, const ConstraintRecord &);
    friend bool operator!=(const ConstraintRecord &a, const ConstraintRecord &b) {
        return !(a == b);
    }
};

// How many of a constraint's operand slots it actually binds: the required
// ones, plus any optional ones it names.
//
// The one place that knows a null optional operand means absent rather than
// dangling. Everything that walks operands — validation, topology, deletion,
// glyphs, solver translation — walks this many, so a nullable reference cannot
// be read as a missing operand by one of them and skipped by another.
//
// Optional operands are a prefix too: a kind carrying one either names it or
// does not, and a gap would be a record no command could have produced.
inline size_t boundOperandCount(const ConstraintRecord &r) {
    const ConstraintKindInfo &info = constraintInfo(r.kind);
    size_t count = info.operandCount;
    while(count < static_cast<size_t>(info.operandCount) + info.optionalOperands &&
          r.operands[count].valid()) {
        count++;
    }
    return count;
}

// A fill referencing a closed cycle of edges. Not a copy of them: the outline
// keeps its constraints, and dragging a vertex moves the fill because the fill
// has no geometry of its own to go stale.
struct RegionRecord {
    using IdType = RegionId;

    RegionId id;
    std::vector<EntityId> boundary;
    StyleId style;
    LayerId layer;

    friend bool operator==(const RegionRecord &, const RegionRecord &);
    friend bool operator!=(const RegionRecord &a, const RegionRecord &b) { return !(a == b); }
};

// A revocable macro identity over primitives. Convenience, never load-bearing:
// a tag owns nothing, so dissolving it leaves perfectly ordinary constrained
// geometry and loses nothing.
enum class TagKind : uint8_t { Rectangle, Distribution, Mirror };

struct TagRecord {
    using IdType = TagId;

    TagId id;
    TagKind kind = TagKind::Rectangle;
    std::vector<EntityId> entities;
    std::vector<ConstraintId> constraints;

    friend bool operator==(const TagRecord &, const TagRecord &);
    friend bool operator!=(const TagRecord &a, const TagRecord &b) { return !(a == b); }
};

// Presentation properties. Widths are slots like every other value, so they
// scrub and take expressions for free; colours are packed RGBA because they are
// not quantities arithmetic applies to.
struct StyleRecord {
    using IdType = StyleId;

    StyleId id;
    std::string name;
    Slot strokeWidth{1.0};
    uint32_t strokeColor = 0xff000000u;
    uint32_t fillColor = 0x00000000u;
    bool filled = false;

    friend bool operator==(const StyleRecord &, const StyleRecord &);
    friend bool operator!=(const StyleRecord &a, const StyleRecord &b) { return !(a == b); }
};

// Organization, not semantics. Constraints cross layers freely. Two couplings
// are real: a locked layer's geometry enters solves pinned, and a hidden
// layer's geometry still constrains — with an indication when it does.
struct LayerRecord {
    using IdType = LayerId;

    LayerId id;
    std::string name;
    int32_t order = 0;
    bool visible = true;
    bool locked = false;

    friend bool operator==(const LayerRecord &, const LayerRecord &);
    friend bool operator!=(const LayerRecord &a, const LayerRecord &b) { return !(a == b); }
};

struct GroupRecord {
    using IdType = GroupId;

    GroupId id;
    std::string name;
    std::vector<EntityId> members;

    friend bool operator==(const GroupRecord &, const GroupRecord &);
    friend bool operator!=(const GroupRecord &a, const GroupRecord &b) { return !(a == b); }
};

// A named value a slot expression may reference. The one place a cycle can
// enter the value graph, which is why the document checks on assignment.
struct ParameterRecord {
    using IdType = ParameterId;

    ParameterId id;
    std::string name;
    Slot value;

    friend bool operator==(const ParameterRecord &, const ParameterRecord &);
    friend bool operator!=(const ParameterRecord &a, const ParameterRecord &b) {
        return !(a == b);
    }
};

}  // namespace paroculus
