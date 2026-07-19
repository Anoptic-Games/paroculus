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
#include <variant>
#include <vector>

#include "core/parameters.h"
#include "core/records.h"
#include "core/table.h"

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
// Regions and tags are removed outright in stage 1. Stage 6 replaces that with
// the degradation states PRINCIPLES calls for, where a region whose boundary
// lost an edge renders broken rather than vanishing.
std::vector<Command> deletionStep(const Document &doc, EntityId id);

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
