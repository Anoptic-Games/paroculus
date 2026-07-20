// The declaration layer.
//
// This is what the user meant: entities, constraints, regions, roles, tags,
// styles, layers, groups, parameters, and the seeds recording which solution
// branch they were shown. Geometry on screen is derived output — a cache of the
// last solve — and every editing operation writes declarations here, never
// solved coordinates.
//
// Mutation happens only through commands. Nothing hands out a mutable record,
// because an undo journal that cannot see a change cannot restore it, and
// "restores what was seen" is the property the whole seeds thread exists to
// deliver.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "core/parameters.h"
#include "core/records.h"
#include "core/table.h"
#include "core/usage.h"

namespace paroculus {

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

// Three shapes, applied to every record kind. Whole-record assignment rather
// than per-field setters is what makes each inverse exact: the inverse of Set
// is Set with the old record, with no field-by-field bookkeeping to get wrong,
// and no way for a newly added field to slip out of the undo path.
template <typename Record>
struct AddRecord {
    using RecordType = Record;
    Record record;  // id null means allocate; set means reinstate at that id
};

template <typename Record>
struct RemoveRecord {
    using RecordType = Record;
    typename Record::IdType id;
};

template <typename Record>
struct SetRecord {
    using RecordType = Record;
    Record record;
};

using Command = std::variant<
    AddRecord<EntityRecord>, RemoveRecord<EntityRecord>, SetRecord<EntityRecord>,
    AddRecord<ConstraintRecord>, RemoveRecord<ConstraintRecord>, SetRecord<ConstraintRecord>,
    AddRecord<RegionRecord>, RemoveRecord<RegionRecord>, SetRecord<RegionRecord>,
    AddRecord<TagRecord>, RemoveRecord<TagRecord>, SetRecord<TagRecord>,
    AddRecord<StyleRecord>, RemoveRecord<StyleRecord>, SetRecord<StyleRecord>,
    AddRecord<LayerRecord>, RemoveRecord<LayerRecord>, SetRecord<LayerRecord>,
    AddRecord<GroupRecord>, RemoveRecord<GroupRecord>, SetRecord<GroupRecord>,
    AddRecord<ParameterRecord>, RemoveRecord<ParameterRecord>, SetRecord<ParameterRecord>>;

// Why a command was refused. Applying is all-or-nothing: a refused command
// leaves the document byte-identical to what it was.
enum class CommandError : uint8_t {
    None,
    NoSuchRecord,       // set or remove naming an id that is not live
    IdCollision,        // add at an id that is already live
    UnknownOperand,     // a constraint or region referring to a missing entity
    WrongSignature,     // operand kinds the constraint kind does not admit
    CyclicParameter,    // an assignment that would make a parameter reach itself
    MissingValue,       // a valued constraint with no slot, or vice versa
    HasDependents,      // a removal that would leave a reference dangling
};

struct CommandResult {
    CommandError error = CommandError::None;
    // Set when a command allocated an ID, so the caller can refer to what it
    // just made without guessing.
    uint32_t allocated = 0;

    bool ok() const { return error == CommandError::None; }
    explicit operator bool() const { return ok(); }
};

const char *commandErrorName(CommandError e);

// Rewrites an AddRecord that allocated an ID so it names that ID explicitly.
// Redo replays forward commands, and an add left with a null ID would allocate
// a second, different ID the second time through — so redo-all would not equal
// the state it is supposed to reproduce. No-op for every other command shape.
void pinAllocatedId(Command &command, uint32_t allocated);

// ---------------------------------------------------------------------------
// Document
// ---------------------------------------------------------------------------

class Document {
public:
    const RecordTable<EntityRecord> &entities() const { return entities_; }
    const RecordTable<ConstraintRecord> &constraints() const { return constraints_; }
    const RecordTable<RegionRecord> &regions() const { return regions_; }
    const RecordTable<TagRecord> &tags() const { return tags_; }
    const RecordTable<StyleRecord> &styles() const { return styles_; }
    const RecordTable<LayerRecord> &layers() const { return layers_; }
    const RecordTable<GroupRecord> &groups() const { return groups_; }
    const ParameterTable &parameters() const { return parameters_; }

    // Record lines from a newer format version that this build does not
    // understand, kept verbatim and written back out. A newer file opened in an
    // older build must not shed the parts the old build cannot read, or every
    // round-trip through an older install is a silent data loss.
    const std::vector<std::string> &unknownRecords() const { return unknown_; }

    // Which relations this document reaches for, and the whole of what the
    // context strip ranks by.
    //
    // The one thing here that is not a declaration, and so the one thing that
    // is not mutated through a command. Reaching for a relation is not an edit:
    // it leaves the drawing alone, it has no inverse worth restoring, and an
    // undo that rewound the ranking would take back something the user never
    // did. It rides along with the document because ranking is document-local,
    // and it is droppable because nothing depends on it.
    const UsageHistory &usage() const { return usage_; }
    void noteUsage(ConstraintKind kind) { usage_.note(kind); }

    // Validates, then applies. On refusal the document is untouched.
    CommandResult apply(const Command &command);

    // The exact inverse of `command` as the document stands now, computed
    // before applying. Returns nullopt when the command would be refused, so a
    // caller that inverts first and applies second never journals a no-op.
    std::optional<Command> invert(const Command &command) const;

    // Evaluates a slot against this document's parameters.
    std::optional<double> evaluate(const Slot &slot) const;

    // The validation the taxonomy drives. Public because the action surface
    // asks the same question the model answers — "what can apply to
    // {segment, segment}" has exactly one source of truth — and because the
    // loader validates a finished document rather than each record as it
    // arrives.
    CommandError validate(const EntityRecord &r) const;
    CommandError validate(const ConstraintRecord &r) const;
    CommandError validate(const RegionRecord &r) const;
    CommandError validate(const TagRecord &r) const;
    CommandError validate(const GroupRecord &r) const;
    CommandError validate(const StyleRecord &r) const;
    CommandError validate(const ParameterRecord &r) const;

    friend bool operator==(const Document &, const Document &);
    friend bool operator!=(const Document &a, const Document &b) { return !(a == b); }

private:
    RecordTable<EntityRecord> entities_;
    RecordTable<ConstraintRecord> constraints_;
    RecordTable<RegionRecord> regions_;
    RecordTable<TagRecord> tags_;
    RecordTable<StyleRecord> styles_;
    RecordTable<LayerRecord> layers_;
    RecordTable<GroupRecord> groups_;
    ParameterTable parameters_;
    UsageHistory usage_;
    std::vector<std::string> unknown_;

    // Persist restores tables wholesale rather than replaying commands, since
    // a load is not an edit and must not be journalled as one. It also has to
    // insert records whose operands appear later in the file, which the
    // command layer would rightly refuse one at a time.
    friend struct DocumentLoader;
};

// Everything that would dangle if an entity were removed: relations naming it,
// fills bounded by it, tags and groups listing it, and geometry defined by it.
// Deletion counts these and reports them — a relation without its operand is
// meaningless and dies with it, but never silently.
struct Dependents {
    std::vector<ConstraintId> constraints;
    std::vector<RegionId> regions;
    std::vector<TagId> tags;
    std::vector<GroupId> groups;
    std::vector<EntityId> entities;  // geometry using it as a defining point

    bool empty() const {
        return constraints.empty() && regions.empty() && tags.empty() && groups.empty() &&
               entities.empty();
    }
    size_t count() const {
        return constraints.size() + regions.size() + tags.size() + groups.size() +
               entities.size();
    }
};

Dependents dependentsOf(const Document &doc, EntityId id);

// Everything whose slot would stop evaluating if a parameter were removed:
// constraint values, style widths, and other parameters built on it.
//
// Separate from Dependents because the relationship is different in kind. A
// constraint that lost an operand is meaningless and dies with it; a
// constraint that lost the name in its value still relates two real entities
// and still holds the length it was holding. So these degrade rather than die
// — see the parameter overload of deletionStep.
struct ParameterDependents {
    std::vector<ConstraintId> constraints;
    std::vector<StyleId> styles;
    std::vector<ParameterId> parameters;

    bool empty() const {
        return constraints.empty() && styles.empty() && parameters.empty();
    }
    size_t count() const {
        return constraints.size() + styles.size() + parameters.size();
    }
};

ParameterDependents dependentsOf(const Document &doc, ParameterId id);

// The ordered command sequence that removes `id` along with everything that
// would otherwise dangle, dependents first. Apply it as one undo step.
//
// The model refuses a removal that would leave a dangling reference, so this
// helper is how deletion is expressed — not a convenience over a permissive
// remove. That is what keeps "no dangling reference" an invariant persist can
// rely on, while keeping "cannot delete: in use" out of the tool entirely: the
// answer to a deletion is always a bigger deletion, reported by count.
//
// Nothing higher-order dies here. Relations naming a deleted operand are
// meaningless without it and are removed; regions, tags and groups shrink and
// keep what they have left. A region thinned past enclosing anything renders in
// the broken-diagnostic state rather than vanishing, which is what makes undo a
// one-step restore: the shrink is a whole-record set and its inverse is exact.
//
// The shrinks are emitted before the removals, so no record names something
// already gone even between two commands of the same step.
//
// A multi-entity deletion is one call, never one call per victim stitched
// together. Each shrink is computed over the whole doomed set: two calls would
// each drop a different edge from the same region and one of them would have to
// win, which is the wrong answer whichever it is.
std::vector<Command> deletionStep(const Document &doc, EntityId id);
std::vector<Command> deletionStep(const Document &doc, std::span<const EntityId> ids);

// The tags naming `id` among the relations that define them, in ID order.
//
// A tag is the one record that names constraints rather than only geometry, so
// a relation is depended on in its own right and not merely as a consequence of
// its operands. Removing one while a tag lists it is refused for the same reason
// an entity removal is: the loader validates tag references, so the state is one
// the document could save and never read back.
std::vector<TagId> tagsOver(const Document &doc, ConstraintId id);

// The ordered command sequence that removes relations the user named directly —
// a selected glyph, a walked conflict set — shrinking the tags that list them
// first. Apply it as one undo step.
//
// A relation deleted on its own is not a degradation of the geometry it binds:
// the geometry is untouched and only the declaration goes. What can degrade is a
// tag built on that declaration, and it shrinks exactly as a region does when it
// loses an edge — keeping what is left, saying so through tagState, and restored
// whole by one undo because the shrink is a whole-record set.
std::vector<Command> deletionStep(const Document &doc, std::span<const ConstraintId> ids);

// Geometry and relations in one cascade, which is what a selection reaching both
// needs. Every shrink is computed over the whole doomed set — the entities'
// relations and the named ones together — because a tag or region that would
// lose members to each of two passes can only be set once.
std::vector<Command> deletionStep(const Document &doc, std::span<const EntityId> entities,
                                  std::span<const ConstraintId> constraints);

// What a layer or a style is attached to: entities and regions naming it, in ID
// order.
//
// One shape for both because it is one relationship. A layer and a style are
// organization hanging off a record rather than something the record is built
// from — nothing about what an entity means changes when either goes — which is
// why the deletion answer for both is reassignment rather than a cascade.
struct AttachmentDependents {
    std::vector<EntityId> entities;
    std::vector<RegionId> regions;

    bool empty() const { return entities.empty() && regions.empty(); }
    size_t count() const { return entities.size() + regions.size(); }
};

AttachmentDependents dependentsOf(const Document &doc, LayerId id);
AttachmentDependents dependentsOf(const Document &doc, StyleId id);

// The ordered command sequence that removes a layer or a style without leaving a
// reference dangling. Apply it as one undo step.
//
// Freeze-shaped, like the parameter case and for the same reason: what the user
// deleted was the organization, so only the organization is lost. A layer's
// entities and regions move to the base layer, which every document has without
// anyone creating one; a style's references are nulled and the geometry falls
// back to the default look. Nothing moves and nothing else is removed.
//
// Refusing the bare removal is what makes this the only way to express one — and
// it has to be refused, because the loader validates layer and style references,
// so a dangling one is a document that saves and will not open.
std::vector<Command> deletionStep(const Document &doc, LayerId id);
std::vector<Command> deletionStep(const Document &doc, StyleId id);

// The composites naming `id` as an operand, in ID order.
std::vector<RegionId> compositesOver(const Document &doc, RegionId id);

// The ordered command sequence that removes a region, lifting it out of every
// composite that names it first. Apply it as one undo step.
//
// The composites shrink rather than dying: a combination of the operands it has
// left is still what it means, and the operands it kept become visible in their
// own right again. That is the non-destructive half of the boolean promise —
// nothing was consumed to make the composite, so nothing is stranded by
// dismantling one.
std::vector<Command> deletionStep(const Document &doc, RegionId id);

// The ordered command sequence that removes a parameter without leaving a slot
// reading a name that is gone. Apply it as one undo step.
//
// Each referring slot is frozen to the value it evaluates to right now, then
// the parameter is removed. Freezing rather than cascading is what the two
// kinds of reference earn: nothing on screen moves, the drawing keeps every
// dimension it had, and all that is lost is the provenance the user just chose
// to delete. Freezing terminates in one pass — a frozen slot refers to
// nothing, so there is no second generation of dependents to walk.
//
// A slot that cannot be evaluated at all (division by zero) freezes to zero.
// It had no value to preserve.
std::vector<Command> deletionStep(const Document &doc, ParameterId id);

// Record-content equality, ignoring the ID watermarks.
//
// Undo restores every record exactly but never rewinds a watermark. That is
// deliberate and it outranks byte-identity: the redo record still names the ID
// the undone add took, and reissuing it would rebind that reference to a
// different object the next time the user edits instead of redoing. So an
// undone add leaves its ID spent, and this is the equality that holds across
// apply-then-undo. Document::operator== is the stricter one, and is what
// persist round-trips are asserted against.
bool sameRecords(const Document &a, const Document &b);

}  // namespace paroculus
