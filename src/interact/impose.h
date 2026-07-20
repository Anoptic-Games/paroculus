// Declaring intent about the present.
//
// Imposition is the noun-verb half of the tool: select geometry, name a
// relation, and the relation becomes true of what is already there. The rule
// that makes it trustworthy is that it moves nothing — an imposed distance
// becomes the distance already measured, an imposed ratio the ratio already
// held — so the geometry moves when a value is edited or something is dragged,
// never when the user says what was already so.
//
// Three things live here, and they are the same question asked at three
// removes:
//
//   what could be imposed   relationsFor, over the selection's signature
//   what it would say       candidateFor, capturing the value from the pose
//   what would happen       previewCandidate, a speculative solve that leaves
//                           the document byte-identical
//
// Nothing here mutates anything. Committing is Session's, because Session owns
// the journal and every visible change has to be undoable.
#pragma once

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "core/measure.h"
#include "core/pose.h"
#include "core/usage.h"
#include "interact/policies.h"
#include "solve/diagnose.h"

namespace paroculus {

// One way of reading a selection into a kind's operand slots.
//
// Roles are assigned here rather than by the taxonomy because the taxonomy
// answers what a kind accepts, not which of two segments the user meant as the
// numerator. Where the slots take different entity kinds the answer is forced
// and there is exactly one assignment; where they take the same kind and the
// relation reads one against the other, there are two and the surface asks.
struct RoleAssignment {
    std::array<EntityId, MAX_OPERANDS> operands{};
    size_t count = 0;
    // Which of the kind's alternative forms. Tangency and nothing else in v1.
    uint8_t alternative = 0;

    friend bool operator==(const RoleAssignment &a, const RoleAssignment &b) {
        return a.operands == b.operands && a.count == b.count && a.alternative == b.alternative;
    }
};

// One relation the current selection admits.
struct RelationOffer {
    ConstraintKind kind = ConstraintKind::Coincident;
    // Every distinct reading, most-canonical first. More than one means the
    // surface must ask which way round — with preview, since the two readings
    // are told apart by what they do rather than by what they are called.
    std::vector<RoleAssignment> assignments;
    // Ranking, higher first. Document-local and deterministic: how often this
    // document has reached for the kind, and nothing else.
    double score = 0.0;

    bool ambiguous() const { return assignments.size() > 1; }
};

// Every relation `selection` admits, ranked.
//
// selection: entity ids, in any order. The result does not depend on that
//   order — offers are a function of the signature and the document, so two
//   selections built by different routes rank alike.
// Returns kinds whose operand slots the selection can fill, ranked by this
//   document's usage and broken by taxonomy order. Deterministic: no hash map
//   is consulted and no clock is read.
std::vector<RelationOffer> relationsFor(const Document &doc,
                                        std::span<const EntityId> selection,
                                        const SurfacePolicy &policy = {});

// Every distinct way `selection` fills `kind`'s slots, canonical first.
//
// Empty means the kind does not apply, which is the applicability predicate the
// action surface consumes — the same one the model's own validation reads,
// since both go through the taxonomy's operand table. An action inapplicable in
// the model is therefore offerable by no surface, for free rather than by
// anyone remembering to keep two lists agreeing.
std::vector<RoleAssignment> assignmentsFor(const Document &doc, ConstraintKind kind,
                                           std::span<const EntityId> selection);

// The record `assignment` would commit, with its value captured from the pose.
//
// This is where movement-free lives. A valued kind takes the measurement that
// is already true, so committing it leaves the solver nothing to do; a kind
// with no value carries none and may well move geometry, which is the
// near-parallel case PRINCIPLES calls the exception that proves the rule.
//
// strength: Impose gives a driving constraint, Reference a driven measurement.
//   Measure builds the same driving record — it is applied and thrown away, and
//   what makes it measure-once is that Session never commits it.
// Returns nullopt when the geometry cannot be resolved or the measurement is
//   undefined, which is the same answer for both because both mean "there is no
//   record to make here".
std::optional<ConstraintRecord> candidateFor(const Pose &pose, ConstraintKind kind,
                                             const RoleAssignment &assignment,
                                             Strength strength);

// What committing `candidate` would do, without doing any of it.
//
// The document is immutable throughout: the candidate rides in as a speculative
// extra on a forked context, so there is no copy to keep in sync and no way for
// a hover to leave a trace. That is what makes previewing every offer on hover
// affordable and what makes preview-does-not-mutate a property rather than a
// discipline.
struct ImpositionPreview {
    CandidateCheck check;

    // The pose the geometry would take, as overlay spans. Empty when the
    // candidate cannot hold, because ghosting an infeasible state would be
    // showing the user somewhere the commit will not go.
    std::vector<SeedSpan> pose;

    // The furthest any point would travel, in document units. Zero is the
    // promise imposition makes; anything else is motion the surface has to
    // show, per no-silent-changes.
    double motion = 0.0;

    bool movementFree(double tolerance) const { return motion <= tolerance; }
};

ImpositionPreview previewCandidate(const Document &doc, const Topology &topology,
                                   const ConstraintRecord &candidate,
                                   const DiagnoseOptions &diagnose = {});

}  // namespace paroculus
