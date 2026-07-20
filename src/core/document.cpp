#include "core/document.h"

#include <algorithm>

namespace paroculus {

const char *commandErrorName(CommandError e) {
    switch(e) {
        case CommandError::None:            return "ok";
        case CommandError::NoSuchRecord:    return "no such record";
        case CommandError::IdCollision:     return "id collision";
        case CommandError::UnknownOperand:  return "unknown operand";
        case CommandError::WrongSignature:  return "wrong signature";
        case CommandError::CyclicParameter: return "cyclic parameter";
        case CommandError::MissingValue:    return "missing value";
        case CommandError::HasDependents:   return "has dependents";
    }
    return "unknown";
}

void pinAllocatedId(Command &command, uint32_t allocated) {
    if(allocated == 0) return;
    std::visit(
        [&](auto &c) {
            using C = std::decay_t<decltype(c)>;
            using Record = typename C::RecordType;
            if constexpr(std::is_same_v<C, AddRecord<Record>>) {
                c.record.id = typename Record::IdType(allocated);
            }
        },
        command);
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

namespace {

// Whether installing `candidate` would let a composite region reach itself.
//
// Walks operands downward from the candidate, reading the candidate's own
// operand list in place of whatever is stored under its ID — the question is
// about the document as it would be, not as it is. A cycle here is not merely
// nonsense: the per-frame path evaluation walks the same edges and would not
// terminate.
bool wouldCycleRegions(const RecordTable<RegionRecord> &regions, const RegionRecord &candidate) {
    std::vector<RegionId> pending = candidate.operands;
    std::vector<RegionId> seen;
    while(!pending.empty()) {
        const RegionId id = pending.back();
        pending.pop_back();
        if(id == candidate.id) return true;
        if(std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
        seen.push_back(id);
        const RegionRecord *r = regions.find(id);
        if(r == nullptr) continue;
        pending.insert(pending.end(), r->operands.begin(), r->operands.end());
    }
    return false;
}

}  // namespace

CommandError Document::validate(const EntityRecord &r) const {
    const EntityKindInfo &info = entityInfo(r.kind);
    for(size_t i = 0; i < info.pointCount; i++) {
        const EntityId p = r.points[i];
        const EntityRecord *pr = entities_.find(p);
        // A defining point must exist and must actually be a point; an arc
        // whose centre is a segment is not a degraded arc, it is nonsense.
        if(pr == nullptr || pr->kind != EntityKind::Point) return CommandError::UnknownOperand;
    }
    // Slots beyond the kind's arity must be null, so two records that mean the
    // same thing compare equal and serialize identically.
    for(size_t i = info.pointCount; i < MAX_ENTITY_POINTS; i++) {
        if(r.points[i].valid()) return CommandError::UnknownOperand;
    }
    // And the same for seeds, for the same reason and one more: an unused seed
    // slot is not scratch space. Junk left in a circle's second slot survives
    // the commit, serializes, and makes a record compare unequal to its own
    // round-trip — while nothing on screen ever shows it.
    for(size_t i = info.ownParamCount; i < MAX_ENTITY_PARAMS; i++) {
        if(r.seeds[i] != 0.0) return CommandError::WrongSignature;
    }
    if(r.layer.valid() && !layers_.contains(r.layer)) return CommandError::UnknownOperand;
    if(r.style.valid() && !styles_.contains(r.style)) return CommandError::UnknownOperand;
    return CommandError::None;
}

// The applicability projection in its enforcing role. The action surface reads
// the same taxonomy to decide what to offer, so an action the model refuses is
// an action no surface offers.
CommandError Document::validate(const ConstraintRecord &r) const {
    const ConstraintKindInfo &info = constraintInfo(r.kind);

    std::vector<EntityKind> kinds;
    kinds.reserve(info.operandCount);
    for(size_t i = 0; i < info.operandCount; i++) {
        const EntityRecord *e = entities_.find(r.operands[i]);
        if(e == nullptr) return CommandError::UnknownOperand;
        kinds.push_back(e->kind);
    }
    // Applicability is decided over the required prefix alone: selecting one
    // segment offers horizontal whether or not a reference axis is named.
    if(!signatureMatches(r.kind, kinds)) return CommandError::WrongSignature;

    // The optional tail. A null slot is the kind's default — for horizontal and
    // vertical, the document frame — and a filled one has to be the kind of
    // thing the taxonomy says it is, exactly like a required operand.
    const size_t bound = boundOperandCount(r);
    for(size_t i = info.operandCount; i < bound; i++) {
        const EntityRecord *e = entities_.find(r.operands[i]);
        if(e == nullptr) return CommandError::UnknownOperand;
        if(!accepts(info.operands[i], e->kind)) return CommandError::WrongSignature;
    }
    // Slots past what the record binds must be null, so two records that mean
    // the same thing compare equal and serialize identically. A gap inside the
    // optional tail lands here too: it is a record no command could produce.
    for(size_t i = bound; i < MAX_OPERANDS; i++) {
        if(r.operands[i].valid()) return CommandError::WrongSignature;
    }

    // A kind with no alternative forms carries the default one, so two records
    // meaning the same thing compare equal and serialize identically.
    if(r.alternative > info.alternatives) return CommandError::WrongSignature;

    // A valueless kind carries no slot: storing a value on a coincidence would
    // serialize a field nothing reads. Only this direction is checkable. A
    // valued kind with no slot is indistinguishable from one holding zero, and
    // zero is a legitimate distance.
    const bool hasValue = !(r.value.isConstant() && r.value.constant() == 0.0);
    if(info.valueArity == 0 && hasValue) return CommandError::MissingValue;

    for(ParameterId p : r.value.references()) {
        if(!parameters_.contains(p)) return CommandError::UnknownOperand;
    }
    return CommandError::None;
}

// A region gets its area exactly one way. An outline names edges and no
// operands; a composite names operands and no edges. A record populating both
// would leave "what is this region's area" with two answers and no rule for
// which wins, which is the ambiguity a single field with two arms exists to
// prevent.
//
// What a region may not do is have too few parts, and that is deliberate. A
// deletion shrinks what it cannot satisfy — an outline down to no edges, a
// composite down to one operand or none — and a region too thin to enclose
// anything renders in the broken-diagnostic state rather than vanishing.
// Refusing thin records here would mean a deletion could only proceed by taking
// the region with it, which is the silent discard PRINCIPLES rules out. Whether
// a region is whole enough to draw is one question asked in one place, and that
// place is regionState(), not the validator.
CommandError Document::validate(const RegionRecord &r) const {
    if(r.op == CompositeOp::Outline) {
        if(!r.operands.empty()) return CommandError::WrongSignature;
        for(EntityId e : r.boundary) {
            const EntityRecord *er = entities_.find(e);
            if(er == nullptr) return CommandError::UnknownOperand;
            // Points cannot bound an area, and construction geometry is
            // excluded from regions by role.
            if(!entityInfo(er->kind).boundaryCapable) return CommandError::UnknownOperand;
        }
    } else {
        if(!r.boundary.empty()) return CommandError::WrongSignature;
        for(size_t i = 0; i < r.operands.size(); i++) {
            const RegionId o = r.operands[i];
            if(o == r.id) return CommandError::CyclicParameter;
            if(!regions_.contains(o)) return CommandError::UnknownOperand;
            // And named once. A composite naming the same operand twice is the
            // exclusivity rule below violated within one record rather than
            // across two: subtract renders it as A−A, lifting it back out has
            // two answers, and no surface can produce it — so the command layer
            // is the only thing that could have.
            for(size_t j = i + 1; j < r.operands.size(); j++) {
                if(r.operands[j] == o) return CommandError::HasDependents;
            }
        }
        // An operand belongs to at most one composite. Two composites over one
        // operand would draw it twice and give lift-it-back-out two answers.
        for(RegionId o : r.operands) {
            for(const RegionRecord &other : regions_.records()) {
                if(other.id == r.id) continue;
                if(std::find(other.operands.begin(), other.operands.end(), o) !=
                   other.operands.end()) {
                    return CommandError::HasDependents;
                }
            }
        }
        if(r.id.valid() && wouldCycleRegions(regions_, r)) return CommandError::CyclicParameter;
    }
    if(r.style.valid() && !styles_.contains(r.style)) return CommandError::UnknownOperand;
    if(r.layer.valid() && !layers_.contains(r.layer)) return CommandError::UnknownOperand;
    return CommandError::None;
}

CommandError Document::validate(const TagRecord &r) const {
    for(EntityId e : r.entities) {
        if(!entities_.contains(e)) return CommandError::UnknownOperand;
    }
    for(ConstraintId c : r.constraints) {
        if(!constraints_.contains(c)) return CommandError::UnknownOperand;
    }
    return CommandError::None;
}

CommandError Document::validate(const GroupRecord &r) const {
    for(EntityId e : r.members) {
        if(!entities_.contains(e)) return CommandError::UnknownOperand;
    }
    return CommandError::None;
}

// A style's quantities are slots like any other, so they can name a parameter
// and are checked like any other — one named width driving every stroke in the
// document is the slot thread's payoff, not a special case. The colours are the
// only fields here that are not references.
CommandError Document::validate(const StyleRecord &r) const {
    for(const Slot *s : {&r.strokeWidth, &r.opacity}) {
        for(ParameterId p : s->references()) {
            if(!parameters_.contains(p)) return CommandError::UnknownOperand;
        }
    }
    return CommandError::None;
}

// A parameter may not reach itself, and every name it reads must exist.
CommandError Document::validate(const ParameterRecord &r) const {
    for(ParameterId p : r.value.references()) {
        if(p == r.id) return CommandError::CyclicParameter;
        if(!parameters_.contains(p)) return CommandError::UnknownOperand;
    }
    if(r.id.valid() && wouldCycle(parameters_, r.id, r.value)) {
        return CommandError::CyclicParameter;
    }
    return CommandError::None;
}

// ---------------------------------------------------------------------------
// Apply
// ---------------------------------------------------------------------------

namespace {

// Add, remove and set against one table, with a validator the caller supplies.
// Templated so the twenty-four command alternatives collapse to three bodies.
template <typename Table, typename Record, typename Validate>
CommandResult applyAdd(Table &table, Record record, Validate &&validate) {
    const CommandError err = validate(record);
    if(err != CommandError::None) return CommandResult{err, 0};

    CommandResult result;
    if(record.id.valid()) {
        if(!table.addAt(std::move(record))) return CommandResult{CommandError::IdCollision, 0};
    } else {
        result.allocated = table.add(std::move(record)).value();
    }
    return result;
}

template <typename Table, typename IdType>
CommandResult applyRemove(Table &table, IdType id) {
    if(!table.remove(id)) return CommandResult{CommandError::NoSuchRecord, 0};
    return CommandResult{};
}

template <typename Table, typename Record, typename Validate>
CommandResult applySet(Table &table, Record record, Validate &&validate) {
    if(!table.contains(record.id)) return CommandResult{CommandError::NoSuchRecord, 0};
    const CommandError err = validate(record);
    if(err != CommandError::None) return CommandResult{err, 0};
    table.set(std::move(record));
    return CommandResult{};
}

constexpr auto alwaysValid = [](const auto &) { return CommandError::None; };

}  // namespace

CommandResult Document::apply(const Command &command) {
    auto entityCheck = [this](const EntityRecord &r) { return validate(r); };
    auto constraintCheck = [this](const ConstraintRecord &r) { return validate(r); };
    auto regionCheck = [this](const RegionRecord &r) { return validate(r); };
    auto tagCheck = [this](const TagRecord &r) { return validate(r); };
    auto groupCheck = [this](const GroupRecord &r) { return validate(r); };
    auto styleCheck = [this](const StyleRecord &r) { return validate(r); };
    auto parameterCheck = [this](const ParameterRecord &r) { return validate(r); };

    return std::visit(
        [&](const auto &c) -> CommandResult {
            using C = std::decay_t<decltype(c)>;

            if constexpr(std::is_same_v<C, AddRecord<EntityRecord>>) {
                return applyAdd(entities_, c.record, entityCheck);
            } else if constexpr(std::is_same_v<C, RemoveRecord<EntityRecord>>) {
                // Referential integrity is a model invariant, not a caller's
                // responsibility: persist, topology and the solver translation
                // all assume no reference dangles. deletionStep() is how a
                // deletion is expressed, and it is always available, so this
                // refusal never reaches the user as "cannot delete: in use".
                if(!dependentsOf(*this, c.id).empty()) {
                    return CommandResult{CommandError::HasDependents, 0};
                }
                return applyRemove(entities_, c.id);
            } else if constexpr(std::is_same_v<C, SetRecord<EntityRecord>>) {
                return applySet(entities_, c.record, entityCheck);

            } else if constexpr(std::is_same_v<C, AddRecord<ConstraintRecord>>) {
                return applyAdd(constraints_, c.record, constraintCheck);
            } else if constexpr(std::is_same_v<C, RemoveRecord<ConstraintRecord>>) {
                // A tag names the relations that define it, so a relation is
                // depended on in its own right. Refusing here rather than
                // letting the reference dangle is what keeps the model from
                // giving three answers about the same state: tagState models a
                // shrunk tag as broken-but-legal, validate refuses a dangling
                // reference, and the loader refuses the file — a document that
                // saves and will not open. The constraint overload of
                // deletionStep() shrinks the tags first and is how every removal
                // path expresses itself.
                if(!tagsOver(*this, c.id).empty()) {
                    return CommandResult{CommandError::HasDependents, 0};
                }
                return applyRemove(constraints_, c.id);
            } else if constexpr(std::is_same_v<C, SetRecord<ConstraintRecord>>) {
                return applySet(constraints_, c.record, constraintCheck);

            } else if constexpr(std::is_same_v<C, AddRecord<RegionRecord>>) {
                return applyAdd(regions_, c.record, regionCheck);
            } else if constexpr(std::is_same_v<C, RemoveRecord<RegionRecord>>) {
                // Same refusal as an entity removal: a composite naming a
                // region that is gone would leave the per-frame path walk with
                // nothing to evaluate. The region overload of deletionStep()
                // shrinks the composites first, which is how lifting an operand
                // back out is expressed.
                if(!compositesOver(*this, c.id).empty()) {
                    return CommandResult{CommandError::HasDependents, 0};
                }
                return applyRemove(regions_, c.id);
            } else if constexpr(std::is_same_v<C, SetRecord<RegionRecord>>) {
                return applySet(regions_, c.record, regionCheck);

            } else if constexpr(std::is_same_v<C, AddRecord<TagRecord>>) {
                return applyAdd(tags_, c.record, tagCheck);
            } else if constexpr(std::is_same_v<C, RemoveRecord<TagRecord>>) {
                return applyRemove(tags_, c.id);
            } else if constexpr(std::is_same_v<C, SetRecord<TagRecord>>) {
                return applySet(tags_, c.record, tagCheck);

            } else if constexpr(std::is_same_v<C, AddRecord<StyleRecord>>) {
                return applyAdd(styles_, c.record, styleCheck);
            } else if constexpr(std::is_same_v<C, RemoveRecord<StyleRecord>>) {
                // The same refusal as everywhere else a reference could dangle.
                // Load validation checks style references, so a record left
                // naming a style that is gone is a document that serializes and
                // will not deserialize. The style overload of deletionStep()
                // nulls the references first.
                if(!dependentsOf(*this, c.id).empty()) {
                    return CommandResult{CommandError::HasDependents, 0};
                }
                return applyRemove(styles_, c.id);
            } else if constexpr(std::is_same_v<C, SetRecord<StyleRecord>>) {
                return applySet(styles_, c.record, styleCheck);

            } else if constexpr(std::is_same_v<C, AddRecord<LayerRecord>>) {
                return applyAdd(layers_, c.record, alwaysValid);
            } else if constexpr(std::is_same_v<C, RemoveRecord<LayerRecord>>) {
                // And the same for a layer. The base layer is what its
                // occupants move to, which is why this refusal never reaches
                // the user as "cannot delete: in use" — there is always
                // somewhere to put them.
                if(!dependentsOf(*this, c.id).empty()) {
                    return CommandResult{CommandError::HasDependents, 0};
                }
                return applyRemove(layers_, c.id);
            } else if constexpr(std::is_same_v<C, SetRecord<LayerRecord>>) {
                return applySet(layers_, c.record, alwaysValid);

            } else if constexpr(std::is_same_v<C, AddRecord<GroupRecord>>) {
                return applyAdd(groups_, c.record, groupCheck);
            } else if constexpr(std::is_same_v<C, RemoveRecord<GroupRecord>>) {
                return applyRemove(groups_, c.id);
            } else if constexpr(std::is_same_v<C, SetRecord<GroupRecord>>) {
                return applySet(groups_, c.record, groupCheck);

            } else if constexpr(std::is_same_v<C, AddRecord<ParameterRecord>>) {
                return applyAdd(parameters_, c.record, parameterCheck);
            } else if constexpr(std::is_same_v<C, RemoveRecord<ParameterRecord>>) {
                // Same refusal as an entity removal, for the same reason: a
                // slot left reading a name that is gone evaluates to nullopt,
                // and the solver translation would drive that dimension to
                // zero. The parameter overload of deletionStep() is how the
                // removal is expressed.
                if(!dependentsOf(*this, c.id).empty()) {
                    return CommandResult{CommandError::HasDependents, 0};
                }
                return applyRemove(parameters_, c.id);
            } else {
                return applySet(parameters_, c.record, parameterCheck);
            }
        },
        command);
}

// ---------------------------------------------------------------------------
// Inversion
// ---------------------------------------------------------------------------

namespace {

// The three inverses, in one place so they cannot drift apart:
//   Add(r)    -> Remove(r.id)   (the id the add will occupy)
//   Remove(i) -> Add(oldRecord) (reinstated at the same id, never a fresh one)
//   Set(r)    -> Set(oldRecord)
// Remove's inverse is why IDs are never recycled: reinstating at a recycled id
// would rebind whatever had taken it in the meantime.
template <typename Table, typename Record>
std::optional<Command> invertAdd(const Table &table, const Record &record) {
    if(record.id.valid()) {
        if(table.contains(record.id)) return std::nullopt;  // would be refused
        return Command{RemoveRecord<Record>{record.id}};
    }
    // The id the allocator is about to hand out. Inversion is computed before
    // applying, so this is a prediction, and it holds because allocation is
    // strictly monotonic and single-threaded.
    return Command{RemoveRecord<Record>{typename Record::IdType(table.allocator().next())}};
}

template <typename Table, typename IdType, typename Record>
std::optional<Command> invertRemove(const Table &table, IdType id, const Record *) {
    const Record *old = table.find(id);
    if(old == nullptr) return std::nullopt;
    return Command{AddRecord<Record>{*old}};
}

template <typename Table, typename Record>
std::optional<Command> invertSet(const Table &table, const Record &record) {
    const Record *old = table.find(record.id);
    if(old == nullptr) return std::nullopt;
    return Command{SetRecord<Record>{*old}};
}

}  // namespace

std::optional<Command> Document::invert(const Command &command) const {
    return std::visit(
        [&](const auto &c) -> std::optional<Command> {
            using C = std::decay_t<decltype(c)>;

            if constexpr(std::is_same_v<C, AddRecord<EntityRecord>>) {
                return invertAdd(entities_, c.record);
            } else if constexpr(std::is_same_v<C, RemoveRecord<EntityRecord>>) {
                return invertRemove(entities_, c.id, static_cast<const EntityRecord *>(nullptr));
            } else if constexpr(std::is_same_v<C, SetRecord<EntityRecord>>) {
                return invertSet(entities_, c.record);

            } else if constexpr(std::is_same_v<C, AddRecord<ConstraintRecord>>) {
                return invertAdd(constraints_, c.record);
            } else if constexpr(std::is_same_v<C, RemoveRecord<ConstraintRecord>>) {
                return invertRemove(constraints_, c.id,
                                    static_cast<const ConstraintRecord *>(nullptr));
            } else if constexpr(std::is_same_v<C, SetRecord<ConstraintRecord>>) {
                return invertSet(constraints_, c.record);

            } else if constexpr(std::is_same_v<C, AddRecord<RegionRecord>>) {
                return invertAdd(regions_, c.record);
            } else if constexpr(std::is_same_v<C, RemoveRecord<RegionRecord>>) {
                return invertRemove(regions_, c.id, static_cast<const RegionRecord *>(nullptr));
            } else if constexpr(std::is_same_v<C, SetRecord<RegionRecord>>) {
                return invertSet(regions_, c.record);

            } else if constexpr(std::is_same_v<C, AddRecord<TagRecord>>) {
                return invertAdd(tags_, c.record);
            } else if constexpr(std::is_same_v<C, RemoveRecord<TagRecord>>) {
                return invertRemove(tags_, c.id, static_cast<const TagRecord *>(nullptr));
            } else if constexpr(std::is_same_v<C, SetRecord<TagRecord>>) {
                return invertSet(tags_, c.record);

            } else if constexpr(std::is_same_v<C, AddRecord<StyleRecord>>) {
                return invertAdd(styles_, c.record);
            } else if constexpr(std::is_same_v<C, RemoveRecord<StyleRecord>>) {
                return invertRemove(styles_, c.id, static_cast<const StyleRecord *>(nullptr));
            } else if constexpr(std::is_same_v<C, SetRecord<StyleRecord>>) {
                return invertSet(styles_, c.record);

            } else if constexpr(std::is_same_v<C, AddRecord<LayerRecord>>) {
                return invertAdd(layers_, c.record);
            } else if constexpr(std::is_same_v<C, RemoveRecord<LayerRecord>>) {
                return invertRemove(layers_, c.id, static_cast<const LayerRecord *>(nullptr));
            } else if constexpr(std::is_same_v<C, SetRecord<LayerRecord>>) {
                return invertSet(layers_, c.record);

            } else if constexpr(std::is_same_v<C, AddRecord<GroupRecord>>) {
                return invertAdd(groups_, c.record);
            } else if constexpr(std::is_same_v<C, RemoveRecord<GroupRecord>>) {
                return invertRemove(groups_, c.id, static_cast<const GroupRecord *>(nullptr));
            } else if constexpr(std::is_same_v<C, SetRecord<GroupRecord>>) {
                return invertSet(groups_, c.record);

            } else if constexpr(std::is_same_v<C, AddRecord<ParameterRecord>>) {
                return invertAdd(parameters_, c.record);
            } else if constexpr(std::is_same_v<C, RemoveRecord<ParameterRecord>>) {
                return invertRemove(parameters_, c.id,
                                    static_cast<const ParameterRecord *>(nullptr));
            } else {
                return invertSet(parameters_, c.record);
            }
        },
        command);
}

std::optional<double> Document::evaluate(const Slot &slot) const {
    const TableParameterEnv env(parameters_);
    return slot.evaluate(&env);
}

bool operator==(const Document &a, const Document &b) {
    return a.entities_ == b.entities_ && a.constraints_ == b.constraints_ &&
           a.regions_ == b.regions_ && a.tags_ == b.tags_ && a.styles_ == b.styles_ &&
           a.layers_ == b.layers_ && a.groups_ == b.groups_ &&
           a.parameters_ == b.parameters_ && a.usage_ == b.usage_ &&
           a.unknown_ == b.unknown_;
}

bool sameRecords(const Document &a, const Document &b) {
    return a.entities().records() == b.entities().records() &&
           a.constraints().records() == b.constraints().records() &&
           a.regions().records() == b.regions().records() &&
           a.tags().records() == b.tags().records() &&
           a.styles().records() == b.styles().records() &&
           a.layers().records() == b.layers().records() &&
           a.groups().records() == b.groups().records() &&
           a.parameters().records() == b.parameters().records() &&
           a.unknownRecords() == b.unknownRecords();
}

Dependents dependentsOf(const Document &doc, EntityId id) {
    Dependents out;
    for(const ConstraintRecord &c : doc.constraints().records()) {
        // Bound rather than required: a segment named as someone's reference
        // axis is depended on, and deleting it has to take the relation with it.
        const size_t n = boundOperandCount(c);
        for(size_t i = 0; i < n; i++) {
            if(c.operands[i] == id) {
                out.constraints.push_back(c.id);
                break;
            }
        }
    }
    for(const RegionRecord &r : doc.regions().records()) {
        if(std::find(r.boundary.begin(), r.boundary.end(), id) != r.boundary.end()) {
            out.regions.push_back(r.id);
        }
    }
    for(const TagRecord &t : doc.tags().records()) {
        if(std::find(t.entities.begin(), t.entities.end(), id) != t.entities.end()) {
            out.tags.push_back(t.id);
        }
    }
    for(const GroupRecord &g : doc.groups().records()) {
        if(std::find(g.members.begin(), g.members.end(), id) != g.members.end()) {
            out.groups.push_back(g.id);
        }
    }
    for(const EntityRecord &e : doc.entities().records()) {
        for(size_t i = 0; i < entityInfo(e.kind).pointCount; i++) {
            if(e.points[i] == id) {
                out.entities.push_back(e.id);
                break;
            }
        }
    }
    return out;
}

namespace {

// Both attachments walk the same two tables, so the walk is written once and
// told which field to read.
template <typename Id, typename Field>
AttachmentDependents attachmentsOf(const Document &doc, Id id, Field field) {
    AttachmentDependents out;
    if(!id.valid()) return out;  // the base layer and the null style are not records
    for(const EntityRecord &e : doc.entities().records()) {
        if(field(e) == id) out.entities.push_back(e.id);
    }
    for(const RegionRecord &r : doc.regions().records()) {
        if(field(r) == id) out.regions.push_back(r.id);
    }
    return out;
}

// And so does the reassignment, which is the same edit either way: null the
// field and leave every other byte of the record alone.
template <typename Id, typename Field>
std::vector<Command> detachStep(const Document &doc, Id id, Field field) {
    std::vector<Command> out;
    const AttachmentDependents deps = attachmentsOf(doc, id, field);
    for(EntityId e : deps.entities) {
        EntityRecord r = *doc.entities().find(e);
        field(r) = Id();
        out.push_back(SetRecord<EntityRecord>{std::move(r)});
    }
    for(RegionId g : deps.regions) {
        RegionRecord r = *doc.regions().find(g);
        field(r) = Id();
        out.push_back(SetRecord<RegionRecord>{std::move(r)});
    }
    return out;
}

constexpr auto layerField = [](auto &r) -> auto & { return r.layer; };
constexpr auto styleField = [](auto &r) -> auto & { return r.style; };

}  // namespace

AttachmentDependents dependentsOf(const Document &doc, LayerId id) {
    return attachmentsOf(doc, id, layerField);
}

AttachmentDependents dependentsOf(const Document &doc, StyleId id) {
    return attachmentsOf(doc, id, styleField);
}

std::vector<Command> deletionStep(const Document &doc, LayerId id) {
    std::vector<Command> out = detachStep(doc, id, layerField);
    out.push_back(RemoveRecord<LayerRecord>{id});
    return out;
}

std::vector<Command> deletionStep(const Document &doc, StyleId id) {
    std::vector<Command> out = detachStep(doc, id, styleField);
    out.push_back(RemoveRecord<StyleRecord>{id});
    return out;
}

std::vector<TagId> tagsOver(const Document &doc, ConstraintId id) {
    std::vector<TagId> out;
    for(const TagRecord &t : doc.tags().records()) {
        if(std::find(t.constraints.begin(), t.constraints.end(), id) != t.constraints.end()) {
            out.push_back(t.id);
        }
    }
    return out;
}

std::vector<RegionId> compositesOver(const Document &doc, RegionId id) {
    std::vector<RegionId> out;
    for(const RegionRecord &r : doc.regions().records()) {
        if(std::find(r.operands.begin(), r.operands.end(), id) != r.operands.end()) {
            out.push_back(r.id);
        }
    }
    return out;
}

// Lifting an operand out of every composite that names it, then removing it.
//
// The composite shrinks rather than dying, for the same reason a group does: it
// is still a combination of the operands it has left, and a composite thinned
// below what it can combine renders broken rather than evaporating. The
// operands it kept become visible in their own right again, which is the
// non-destructive half of the boolean promise made good.
std::vector<Command> deletionStep(const Document &doc, RegionId id) {
    std::vector<Command> out;
    for(RegionId c : compositesOver(doc, id)) {
        RegionRecord shrunk = *doc.regions().find(c);
        shrunk.operands.erase(
            std::remove(shrunk.operands.begin(), shrunk.operands.end(), id),
            shrunk.operands.end());
        out.push_back(SetRecord<RegionRecord>{std::move(shrunk)});
    }
    out.push_back(RemoveRecord<RegionRecord>{id});
    return out;
}

ParameterDependents dependentsOf(const Document &doc, ParameterId id) {
    ParameterDependents out;
    auto reads = [id](const Slot &s) {
        const std::vector<ParameterId> refs = s.references();
        return std::find(refs.begin(), refs.end(), id) != refs.end();
    };
    for(const ConstraintRecord &c : doc.constraints().records()) {
        if(reads(c.value)) out.constraints.push_back(c.id);
    }
    for(const StyleRecord &s : doc.styles().records()) {
        if(reads(s.strokeWidth) || reads(s.opacity)) out.styles.push_back(s.id);
    }
    for(const ParameterRecord &p : doc.parameters().records()) {
        if(p.id != id && reads(p.value)) out.parameters.push_back(p.id);
    }
    return out;
}

// Freeze every referring slot, then remove. Freezing is one pass because a
// frozen slot reads nothing: the parameters that referred to this one become
// constants themselves rather than a second generation of dependents.
std::vector<Command> deletionStep(const Document &doc, ParameterId id) {
    std::vector<Command> out;
    const ParameterDependents deps = dependentsOf(doc, id);

    // Evaluated against the document as it stands, before any freeze lands, so
    // a chain (b = a * 2, c = b + 1) freezes every link at the value it holds
    // now rather than at whatever a half-applied step would have produced.
    // Only the slots that actually read the doomed name freeze. A record can
    // carry two slots — a style's width and its opacity — and flattening the one
    // that named a different parameter would lose provenance the user never
    // asked to delete.
    auto frozen = [&doc, id](const Slot &s) {
        const std::vector<ParameterId> refs = s.references();
        if(std::find(refs.begin(), refs.end(), id) == refs.end()) return s;
        return Slot(doc.evaluate(s).value_or(0.0));
    };

    for(ConstraintId c : deps.constraints) {
        ConstraintRecord r = *doc.constraints().find(c);
        r.value = frozen(r.value);
        out.push_back(SetRecord<ConstraintRecord>{std::move(r)});
    }
    for(StyleId s : deps.styles) {
        StyleRecord r = *doc.styles().find(s);
        r.strokeWidth = frozen(r.strokeWidth);
        r.opacity = frozen(r.opacity);
        out.push_back(SetRecord<StyleRecord>{std::move(r)});
    }
    for(ParameterId p : deps.parameters) {
        ParameterRecord r = *doc.parameters().find(p);
        r.value = frozen(r.value);
        out.push_back(SetRecord<ParameterRecord>{std::move(r)});
    }
    out.push_back(RemoveRecord<ParameterRecord>{id});
    return out;
}

// Depth-first over the dependency graph, emitting each victim after everything
// that depends on it. Geometry recurses (deleting a point takes the segments
// built on it, which take the constraints on those); relations are leaves.
std::vector<Command> deletionStep(const Document &doc, EntityId id) {
    const EntityId one[] = {id};
    return deletionStep(doc, one);
}

std::vector<Command> deletionStep(const Document &doc, std::span<const EntityId> ids) {
    return deletionStep(doc, ids, std::span<const ConstraintId>());
}

std::vector<Command> deletionStep(const Document &doc, std::span<const ConstraintId> ids) {
    return deletionStep(doc, std::span<const EntityId>(), ids);
}

std::vector<Command> deletionStep(const Document &doc, std::span<const EntityId> ids,
                                  std::span<const ConstraintId> relations) {
    std::vector<Command> out;
    std::vector<EntityId> seenEntities;
    std::vector<ConstraintId> seenConstraints;
    // Every entity the cascade actually removes, dependents before what they
    // are built on. Held back rather than emitted inline because the group
    // shrinks below have to be computed over the whole set and applied before
    // any of it goes.
    std::vector<EntityId> doomed;

    auto once = [](auto &seen, auto value) {
        if(std::find(seen.begin(), seen.end(), value) != seen.end()) return false;
        seen.push_back(value);
        return true;
    };

    // Recursion depth is bounded by the geometry chain — a point, the segments
    // on it, nothing deeper — so an explicit stack would buy nothing here.
    auto collect = [&](EntityId victim, auto &&self) -> void {
        if(!once(seenEntities, victim)) return;
        if(doc.entities().find(victim) == nullptr) return;

        const Dependents deps = dependentsOf(doc, victim);
        for(EntityId e : deps.entities) self(e, self);
        for(ConstraintId c : deps.constraints) once(seenConstraints, c);
        doomed.push_back(victim);
    };
    for(EntityId id : ids) collect(id, collect);

    // The relations the user named, alongside the ones the geometry took with
    // it. Seeded into the same set rather than removed in a pass of their own,
    // so a tag listing one of each shrinks once — two passes would each compute
    // a shrink from the unshrunk record and the later would restore what the
    // earlier dropped.
    for(ConstraintId id : relations) {
        if(doc.constraints().find(id) == nullptr) continue;
        once(seenConstraints, id);
    }

    // Regions and tags shrink, and the shrinks go first.
    //
    // A region that lost a boundary edge no longer encloses anything, and it
    // says so: it keeps the edges it has and renders in the broken-diagnostic
    // state, which is what PRINCIPLES asks for in place of either blocking the
    // deletion or silently discarding the fill. Undo puts the edge back and the
    // region is whole again in one step, because the shrink is an ordinary
    // whole-record set whose inverse is exact.
    //
    // Emitted before the removals rather than after, so no record ever names
    // something that is already gone — not even between two commands of the
    // same step.
    auto doomedEntity = [&](EntityId e) {
        return std::find(doomed.begin(), doomed.end(), e) != doomed.end();
    };
    auto doomedConstraint = [&](ConstraintId c) {
        return std::find(seenConstraints.begin(), seenConstraints.end(), c) !=
               seenConstraints.end();
    };

    for(const TagRecord &t : doc.tags().records()) {
        TagRecord shrunk = t;
        shrunk.entities.erase(
            std::remove_if(shrunk.entities.begin(), shrunk.entities.end(), doomedEntity),
            shrunk.entities.end());
        shrunk.constraints.erase(
            std::remove_if(shrunk.constraints.begin(), shrunk.constraints.end(),
                           doomedConstraint),
            shrunk.constraints.end());
        if(shrunk != t) out.push_back(SetRecord<TagRecord>{std::move(shrunk)});
    }

    for(const RegionRecord &r : doc.regions().records()) {
        RegionRecord shrunk = r;
        shrunk.boundary.erase(
            std::remove_if(shrunk.boundary.begin(), shrunk.boundary.end(), doomedEntity),
            shrunk.boundary.end());
        if(shrunk != r) out.push_back(SetRecord<RegionRecord>{std::move(shrunk)});
    }

    // A group loses the member, never the grouping. Membership is organization
    // and nothing reads it as structure, so a group that lost one entity still
    // names the others correctly — where a region that lost a boundary edge no
    // longer bounds anything. Same reasoning as the parameter case: only what
    // the user deleted is lost. The group survives even emptied, because
    // deciding a named container has outlived its purpose is the user's call
    // and deleting it is an action they already have.
    //
    // Computed over the whole doomed set, in one record per group. Emitting one
    // per victim would have each drop a different member and the last would win,
    // restoring the ones before it.
    for(const GroupRecord &g : doc.groups().records()) {
        GroupRecord shrunk = g;
        shrunk.members.erase(
            std::remove_if(shrunk.members.begin(), shrunk.members.end(),
                           [&](EntityId m) {
                               return std::find(doomed.begin(), doomed.end(), m) != doomed.end();
                           }),
            shrunk.members.end());
        if(shrunk.members.size() != g.members.size()) {
            out.push_back(SetRecord<GroupRecord>{shrunk});
        }
    }

    for(ConstraintId c : seenConstraints) out.push_back(RemoveRecord<ConstraintRecord>{c});

    // Entities last: a removal is refused while anything still names it, and by
    // here nothing does.
    for(EntityId victim : doomed) out.push_back(RemoveRecord<EntityRecord>{victim});

    return out;
}

}  // namespace paroculus
