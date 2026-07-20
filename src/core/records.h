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
#include <string_view>
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
    StyleId style;
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

    // Which of the kind's alternative forms this instance selects. Zero is the
    // default form and the only legal value for a kind that has no others.
    //
    // v1 uses it for one thing: tangency holds at one end of an arc, and which
    // end is not derivable from the operands — the pair is an arc and a segment
    // either way. Recorded here rather than as a second constraint kind because
    // it is the same relation with a choice in it, not a different relation:
    // one row in the taxonomy, one entry on a menu, and a property the user can
    // flip like `driving`.
    uint8_t alternative = 0;

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

// The joints a boundary edge presents to a walk, and whether it needs any.
//
// Which points these are is per-kind, and emphatically not "points[0] and
// points[1]": an arc's first point is its centre, which is construction geometry
// and never a joint, and a circle has no joints at all because it closes on
// itself. Asking a circle for two ends is asking the wrong question rather than
// getting a wrong answer, which is why `selfClosing` is a state here and not a
// null pair.
//
// One function because three walks ask this — the connected run in interact, the
// cycle test in topology, and the ring walk in core — and two of them had
// answered it the segment way. A segments-only guard in the cycle test hid both,
// so the bug was latent rather than visible: lifting that guard without this
// would have produced regions bounded through an arc's centre. Same discipline
// as boundOperandCount below, and for the same reason.
struct BoundaryEnds {
    // Whether this kind can bound a region at all.
    bool capable = false;
    // Whether it bounds one on its own, with no neighbour to meet.
    bool selfClosing = false;
    // Whether it encloses area between its two joints rather than along the
    // straight line between them. What makes two edges enough to bound a region
    // when one of them is curved, and three the minimum when none is.
    bool curved = false;
    // The joints, when it has them.
    EntityId from;
    EntityId to;
};

inline BoundaryEnds boundaryEnds(const EntityRecord &r) {
    BoundaryEnds out;
    if(!entityInfo(r.kind).boundaryCapable) return out;
    out.capable = true;
    switch(r.kind) {
        case EntityKind::Segment:
            out.from = r.points[0];
            out.to = r.points[1];
            break;
        case EntityKind::Arc:
            // points[0] is the centre. It is construction geometry the arc macro
            // leaves behind, and treating it as a joint is how an arc comes to
            // bound a region through the middle of itself.
            out.from = r.points[1];
            out.to = r.points[2];
            out.curved = true;
            break;
        case EntityKind::Circle:
            out.selfClosing = true;
            out.curved = true;
            break;
        case EntityKind::Point:
            out.capable = false;
            break;
    }
    // A record missing a joint it should have is not a boundary edge, whatever
    // its kind says. The loader validates, but a walk that trusted the kind
    // alone would still have to guard, so it guards here once.
    if(out.capable && !out.selfClosing && (!out.from.valid() || !out.to.valid())) {
        out.capable = false;
    }
    return out;
}

// Whether a set of boundary edges encloses any area.
//
// Straight edges need three: two segments over one pair of vertices pass every
// degree and connectivity test and walk closed, and the 2-gon they report
// encloses nothing. A curve changes that — two arcs enclose a lens, an arc and a
// chord enclose a circular segment, and a single closed curve encloses on its
// own — so the bound is about what the edges are, not how many.
//
// Harmless while closure only notices; wrong the moment make-solid fills what it
// is handed, which is why this is one predicate rather than a number compared in
// two walks.
inline bool enclosesArea(size_t edges, bool anyCurved) {
    if(edges == 0) return false;
    return anyCurved || edges >= 3;
}

// How a region gets its area. Outline walks a cycle of edges; the rest combine
// other regions, which stay live records of their own.
//
// Booleans are composition, never consumption: a composite names its operands
// and computes their combination every frame, so the operands keep their
// constraints, can be constrained to each other — the hole staying concentric
// with the plate — and reappear the moment the composite is deleted. Destructive
// path booleans, where operands are consumed to mint a new outline, exist
// nowhere in this model; the only place they exist at all is the export bake.
enum class CompositeOp : uint8_t { Outline, Union, Intersect, Subtract };

const char *compositeOpName(CompositeOp op);
bool compositeOpFromName(std::string_view name, CompositeOp &out);

// A fill referencing a closed cycle of edges, or a combination of other fills.
// Not a copy of either: the outline keeps its constraints, and dragging a vertex
// moves the fill because the fill has no geometry of its own to go stale.
struct RegionRecord {
    using IdType = RegionId;

    RegionId id;
    CompositeOp op = CompositeOp::Outline;

    // Outline regions name edges here and nothing in `operands`; composites do
    // the reverse. Exactly one of the two is populated, which is what makes
    // "how does this region get its area" a question with one answer.
    std::vector<EntityId> boundary;
    std::vector<RegionId> operands;

    StyleId style;
    LayerId layer;

    // Order within the layer. Signed, so a region can be pushed below the ones
    // that were there before it without renumbering them. Ties break by ID,
    // which is creation order, so the order is total and stable.
    int32_t z = 0;

    // Alpha overwrite: this region carves visibility out of what its layer has
    // accumulated so far rather than painting over it. The cut-out half of the
    // README's layering thesis, and compositional like the rest of it — the
    // shape stays a live constrained object and lifting it out restores what it
    // was hiding.
    bool punch = false;

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

// Presentation properties. The quantities are slots like every other value, so
// they scrub, take expressions and follow a named document parameter for free —
// that is the whole point of the slot indirection being unconditional rather
// than reserved for constraint values. Colours are packed RGBA because they are
// not quantities arithmetic applies to.
struct StyleRecord {
    using IdType = StyleId;

    StyleId id;
    std::string name;
    Slot strokeWidth{1.0};
    Slot opacity{1.0};
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
