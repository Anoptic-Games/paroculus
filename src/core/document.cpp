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
    if(r.layer.valid() && !layers_.contains(r.layer)) return CommandError::UnknownOperand;
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
    for(size_t i = info.operandCount; i < MAX_OPERANDS; i++) {
        if(r.operands[i].valid()) return CommandError::WrongSignature;
    }
    if(!signatureMatches(r.kind, kinds)) return CommandError::WrongSignature;

    // A valued kind carries a slot; a valueless one carries none. Storing a
    // value on a coincidence would serialize a field nothing reads.
    const bool hasValue = !(r.value.isConstant() && r.value.constant() == 0.0);
    if(info.valueArity == 0 && hasValue) return CommandError::MissingValue;

    for(ParameterId p : r.value.references()) {
        if(!parameters_.contains(p)) return CommandError::UnknownOperand;
    }
    return CommandError::None;
}

CommandError Document::validate(const RegionRecord &r) const {
    if(r.boundary.empty()) return CommandError::UnknownOperand;
    for(EntityId e : r.boundary) {
        const EntityRecord *er = entities_.find(e);
        if(er == nullptr) return CommandError::UnknownOperand;
        // Points cannot bound an area, and construction geometry is excluded
        // from regions by role.
        if(!entityInfo(er->kind).boundaryCapable) return CommandError::UnknownOperand;
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

// A style's width is a slot like any other, so it can name a parameter and is
// checked like any other. Nothing else on the record is a reference.
CommandError Document::validate(const StyleRecord &r) const {
    for(ParameterId p : r.strokeWidth.references()) {
        if(!parameters_.contains(p)) return CommandError::UnknownOperand;
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
                return applyRemove(constraints_, c.id);
            } else if constexpr(std::is_same_v<C, SetRecord<ConstraintRecord>>) {
                return applySet(constraints_, c.record, constraintCheck);

            } else if constexpr(std::is_same_v<C, AddRecord<RegionRecord>>) {
                return applyAdd(regions_, c.record, regionCheck);
            } else if constexpr(std::is_same_v<C, RemoveRecord<RegionRecord>>) {
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
                return applyRemove(styles_, c.id);
            } else if constexpr(std::is_same_v<C, SetRecord<StyleRecord>>) {
                return applySet(styles_, c.record, styleCheck);

            } else if constexpr(std::is_same_v<C, AddRecord<LayerRecord>>) {
                return applyAdd(layers_, c.record, alwaysValid);
            } else if constexpr(std::is_same_v<C, RemoveRecord<LayerRecord>>) {
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
           a.parameters_ == b.parameters_ && a.unknown_ == b.unknown_;
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
        const size_t n = constraintInfo(c.kind).operandCount;
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
        if(reads(s.strokeWidth)) out.styles.push_back(s.id);
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
    auto frozen = [&doc](const Slot &s) { return Slot(doc.evaluate(s).value_or(0.0)); };

    for(ConstraintId c : deps.constraints) {
        ConstraintRecord r = *doc.constraints().find(c);
        r.value = frozen(r.value);
        out.push_back(SetRecord<ConstraintRecord>{std::move(r)});
    }
    for(StyleId s : deps.styles) {
        StyleRecord r = *doc.styles().find(s);
        r.strokeWidth = frozen(r.strokeWidth);
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
    std::vector<Command> out;
    std::vector<EntityId> seenEntities;
    std::vector<ConstraintId> seenConstraints;
    std::vector<RegionId> seenRegions;
    std::vector<TagId> seenTags;
    std::vector<GroupId> seenGroups;

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
        for(ConstraintId c : deps.constraints) {
            if(once(seenConstraints, c)) out.push_back(RemoveRecord<ConstraintRecord>{c});
        }
        for(RegionId r : deps.regions) {
            if(once(seenRegions, r)) out.push_back(RemoveRecord<RegionRecord>{r});
        }
        for(TagId t : deps.tags) {
            if(once(seenTags, t)) out.push_back(RemoveRecord<TagRecord>{t});
        }
        for(GroupId g : deps.groups) {
            if(once(seenGroups, g)) out.push_back(RemoveRecord<GroupRecord>{g});
        }
        out.push_back(RemoveRecord<EntityRecord>{victim});
    };
    collect(id, collect);

    return out;
}

}  // namespace paroculus
