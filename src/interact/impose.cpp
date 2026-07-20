#include "interact/impose.h"

#include <algorithm>
#include <cmath>

#include "solve/solve.h"

namespace paroculus {
namespace {

// The entity kinds a set of ids resolves to, in the order given. Absent when
// any of them is not live, because an assignment naming a deleted entity is not
// an assignment the surface may offer.
std::optional<std::vector<EntityKind>> kindsOf(const Document &doc,
                                               std::span<const EntityId> ids) {
    std::vector<EntityKind> out;
    out.reserve(ids.size());
    for(EntityId id : ids) {
        const EntityRecord *r = doc.entities().find(id);
        if(r == nullptr) return std::nullopt;
        out.push_back(r->kind);
    }
    return out;
}

// Whether `ids` fills `kind`'s slots in exactly that order, required operands
// first and any optional ones after.
//
// Applicability over the required prefix alone is the taxonomy's rule and the
// reason one selected segment still offers horizontal. The optional tail is
// checked separately, because filling it is a different declaration rather than
// a better-formed version of the same one.
bool fillsSlots(const Document &doc, ConstraintKind kind, std::span<const EntityId> ids) {
    const ConstraintKindInfo &info = constraintInfo(kind);
    if(ids.size() < info.operandCount) return false;
    if(ids.size() > static_cast<size_t>(info.operandCount) + info.optionalOperands) return false;
    const auto kinds = kindsOf(doc, ids);
    if(!kinds) return false;
    for(size_t i = 0; i < kinds->size(); i++) {
        if(!accepts(info.operands[i], (*kinds)[i])) return false;
    }
    // An entity may fill only one slot: a segment is not parallel to itself,
    // and a point is not at a distance from itself.
    for(size_t i = 0; i < ids.size(); i++) {
        for(size_t j = i + 1; j < ids.size(); j++) {
            if(ids[i] == ids[j]) return false;
        }
    }
    return true;
}

// Whether a reading is the one that stands for its grouping.
//
// A grouped kind's slots are interchangeable within each group and the groups
// are interchangeable with each other, so most orderings of four segments say
// the same thing as some other ordering. Keeping only the ordering that is
// ascending within each group and ascending across them leaves exactly one
// reading per genuinely different pairing — three of them for equal-angle,
// rather than twenty-four rows saying three things.
bool canonicalGrouping(std::span<const EntityId> ids, size_t count, size_t group) {
    for(size_t start = 0; start < count; start += group) {
        for(size_t i = start + 1; i < start + group; i++) {
            if(!(ids[i - 1] < ids[i])) return false;
        }
        if(start + group < count && !(ids[start] < ids[start + group])) return false;
    }
    return true;
}

RoleAssignment makeAssignment(std::span<const EntityId> ids, uint8_t alternative) {
    RoleAssignment a;
    a.count = ids.size();
    for(size_t i = 0; i < ids.size() && i < MAX_OPERANDS; i++) a.operands[i] = ids[i];
    a.alternative = alternative;
    return a;
}

// Every distinct way `selection` fills `kind`'s slots, canonical first.
//
// The taxonomy decides how many survive, and it says two different things. A
// kind that reads one operand against the other — len(A)/len(B) — keeps both
// readings, because they are different declarations and the surface has to ask
// which was meant. A kind whose slots group keeps one reading per grouping:
// equal-angle over four segments is three declarations, not one, and not the
// twenty-four its permutations would suggest. Every other kind keeps one, since
// a coincidence between A and B is the coincidence between B and A and offering
// it twice would be offering the same relation under two names.
//
// The kept reading is the ID-lexicographically smallest, so it is a function of
// the selection rather than of the order it was clicked in.
}  // namespace

std::vector<RoleAssignment> assignmentsFor(const Document &doc, ConstraintKind kind,
                                           std::span<const EntityId> selection) {
    const ConstraintKindInfo &info = constraintInfo(kind);
    const size_t width = selection.size();
    if(width < info.operandCount) return {};
    if(width > static_cast<size_t>(info.operandCount) + info.optionalOperands) return {};

    // Permutations of at most four ids: 24 orderings, generated in
    // lexicographic order so the first valid one is the canonical one.
    std::vector<EntityId> ordered(selection.begin(), selection.end());
    std::sort(ordered.begin(), ordered.end());

    // Two reasons to walk past the first valid ordering, and they are different
    // questions. Order sensitivity is "does swapping these two change what it
    // says" — true of len(A)/len(B) and almost nothing else. Grouping is "which
    // of these belong together", which for equal-angle has three answers that a
    // kind reading its operands in one fixed order can never offer.
    const bool enumerate = info.orderSensitive || info.operandGroupSize > 0;

    std::vector<RoleAssignment> out;
    do {
        if(!fillsSlots(doc, kind, ordered)) continue;
        if(info.operandGroupSize > 0 &&
           !canonicalGrouping(ordered, info.operandCount, info.operandGroupSize)) {
            continue;
        }
        // Tangency holds at one end of the arc or the other, and the operands
        // cannot say which. Both forms are offered; everything else has one.
        for(uint8_t alt = 0; alt <= info.alternatives; alt++) {
            out.push_back(makeAssignment(ordered, alt));
        }
        if(!enumerate) break;
    } while(std::next_permutation(ordered.begin(), ordered.end()));

    return out;
}

std::vector<RelationOffer> relationsFor(const Document &doc,
                                        std::span<const EntityId> selection,
                                        const SurfacePolicy &policy) {
    std::vector<RelationOffer> out;
    if(selection.empty()) return out;

    for(const ConstraintKindInfo &info : CONSTRAINT_KINDS) {
        std::vector<RoleAssignment> assignments = assignmentsFor(doc, info.kind, selection);
        if(assignments.empty()) continue;

        RelationOffer offer;
        offer.kind = info.kind;
        offer.assignments = std::move(assignments);
        // Document-local, deterministic, and the whole of the context: how
        // often this document has reached for the kind, capped so habit tilts
        // the list rather than freezing it.
        const double uses = std::min(static_cast<double>(doc.usage().count(info.kind)),
                                     policy.usageCeiling);
        offer.score = uses * policy.usageWeight;
        out.push_back(std::move(offer));
    }

    // Stable, so equal scores keep taxonomy order — which is the tiebreak that
    // makes the ranking inspectable: a user who has imposed nothing sees the
    // catalogue's own order, not an arbitrary one.
    std::stable_sort(out.begin(), out.end(),
                     [](const RelationOffer &a, const RelationOffer &b) {
                         return a.score > b.score;
                     });
    return out;
}

std::optional<ConstraintRecord> candidateFor(const Pose &pose, ConstraintKind kind,
                                             const RoleAssignment &assignment,
                                             Strength strength) {
    const ConstraintKindInfo &info = constraintInfo(kind);
    if(assignment.count < info.operandCount) return std::nullopt;
    if(assignment.alternative > info.alternatives) return std::nullopt;

    ConstraintRecord r;
    r.kind = kind;
    r.operands = assignment.operands;
    r.alternative = assignment.alternative;
    // Reference is the same object with a toggle, which is how a measurement is
    // promoted to intent and demoted again at an over-constraint downgrade.
    r.driving = strength != Strength::Reference;

    if(info.valueArity == 1) {
        // Measured through the record rather than through the kind and operands,
        // so the form the assignment names is the form the value is captured in.
        // An angle's alternative is the supplement; capturing the default and
        // storing it on a record that declares the supplement declares something
        // untrue and the solver moves the drawing to make it so — which is the
        // exact opposite of what imposition promises.
        const std::optional<double> value = measure(pose, r);
        // No measurement means no record: a ratio against a zero-length segment
        // is not a relation with a bad value, it is not a relation.
        if(!value) return std::nullopt;
        r.value = Slot(*value);
    }
    return r;
}

ImpositionPreview previewCandidate(const Document &doc, const Topology &topology,
                                   const ConstraintRecord &candidate,
                                   const DiagnoseOptions &diagnose) {
    ImpositionPreview preview;
    preview.check = checkCandidate(doc, topology, candidate, diagnose);
    if(!preview.check.committable()) return preview;

    // The same fork the check ran on, solved again for its pose. Cheap because
    // a context holds one component's parameter spans rather than a document,
    // which is the representation decision previews, async solving and
    // creation-time validation all sit on.
    const size_t operandCount = boundOperandCount(candidate);
    const std::vector<EntityId> anchors(candidate.operands.begin(),
                                        candidate.operands.begin() + operandCount);
    SolveContext context = SolveContext::forComponents(doc, topology, anchors);
    if(context.empty()) return preview;

    const std::vector<SeedSpan> before = context.params();

    SolveOptions options;
    options.diagnoseFailures = false;
    ConstraintRecord driving = candidate;
    // Previewed as driving whatever the record says. A reference measurement
    // moves nothing by construction, so previewing one as itself would show a
    // still picture and promise nothing.
    driving.driving = true;
    options.extra.push_back(driving);
    if(!solve(doc, context, options).ok()) return preview;

    preview.pose = context.params();

    // How far the furthest point travels. Compared span by span against the
    // seeds the fork started from, so what is measured is the motion the
    // commit would cause and not the difference between two framings.
    for(const SeedSpan &after : preview.pose) {
        for(const SeedSpan &start : before) {
            if(start.entity != after.entity) continue;
            double sum = 0.0;
            for(size_t i = 0; i < MAX_ENTITY_PARAMS; i++) {
                const double d = after.seeds[i] - start.seeds[i];
                sum += d * d;
            }
            preview.motion = std::max(preview.motion, std::sqrt(sum));
            break;
        }
    }
    return preview;
}

}  // namespace paroculus
