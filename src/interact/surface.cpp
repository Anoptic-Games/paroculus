#include "interact/surface.h"

#include <algorithm>

#include "interact/session.h"

namespace paroculus {
namespace {

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// A short name for one operand: its kind and its id, which is the least a user
// needs to tell two segments apart when the surface asks which is which.
std::string nameOf(const Document &doc, EntityId id) {
    const EntityRecord *e = doc.entities().find(id);
    if(e == nullptr) return "?";
    return std::string(entityInfo(e->kind).name) + " " + std::to_string(id.value());
}

// What a kind's alternative form is called, for a surface offering both.
//
// Presentation, so a switch here rather than a column in the taxonomy — the same
// reason titles are derived from the name rather than stored beside it. What the
// forms *are* is the taxonomy's; what to call them is not.
std::string_view alternativeName(ConstraintKind kind, uint8_t alternative) {
    switch(kind) {
        case ConstraintKind::Tangent:
            return alternative == 0 ? " (at start)" : " (at end)";
        case ConstraintKind::Angle:
        case ConstraintKind::EqualAngle:
            return alternative == 0 ? "" : " (supplement)";
        default:
            return "";
    }
}

}  // namespace

bool matchesQuery(std::string_view text, std::string_view query) {
    if(query.empty()) return true;
    size_t at = 0;
    for(char c : text) {
        if(at < query.size() && lower(c) == lower(query[at])) at++;
    }
    return at == query.size();
}

std::string describeAssignment(const Document &doc, ConstraintKind kind,
                               const RoleAssignment &assignment) {
    const ConstraintKindInfo &info = constraintInfo(kind);
    std::string out;
    for(size_t i = 0; i < assignment.count; i++) {
        if(i) out += " : ";
        out += nameOf(doc, assignment.operands[i]);
    }
    // A kind with alternative forms has two readings that name the same
    // operands in the same order, so the operands alone would spell them
    // identically and the surface would offer the same words twice.
    if(info.alternatives > 0) out += alternativeName(kind, assignment.alternative);
    return out;
}

std::vector<SurfaceEntry> stripEntries(const Session &session) {
    std::vector<SurfaceEntry> out;
    const Document &doc = session.document();
    const SurfacePolicy &policy = session.surfacePolicy();

    for(const RelationOffer &offer : session.relationOffers()) {
        const Action *action = impositionAction(offer.kind, Strength::Impose);
        if(action == nullptr) continue;

        for(size_t i = 0; i < offer.assignments.size(); i++) {
            SurfaceEntry entry;
            entry.action = action;
            entry.applicable = true;  // relationsFor only returns what applies
            entry.score = offer.score;
            entry.title = std::string(action->title);
            // Only an ambiguous relation names its reading. Spelling out the
            // operands of an unambiguous one would be noise on every entry to
            // disambiguate the two that need it.
            if(offer.ambiguous()) {
                entry.title += " (" + describeAssignment(doc, offer.kind, offer.assignments[i]) + ")";
                entry.arguments.set("assignment", static_cast<double>(i));
            }
            out.push_back(std::move(entry));
        }
    }

    // The fill offers ride in the strip too, because they are contextual in
    // exactly the same way — they exist because of what is under the cursor —
    // and because the moment an outline closes is the moment the user is
    // thinking about filling it.
    auto pushIfApplicable = [&](std::string_view name) {
        const Action *action = findAction(name);
        if(action == nullptr) return;
        if(action->applicable != nullptr && !action->applicable(contextOf(session), *action)) {
            return;
        }
        SurfaceEntry entry;
        entry.action = action;
        entry.applicable = true;
        entry.title = std::string(action->title);
        // Above every relation, so the offer that just became available is not
        // buried under a catalogue the user was not asking about.
        entry.score = policy.usageCeiling * policy.usageWeight + 1.0;
        out.push_back(std::move(entry));
    };
    pushIfApplicable("region.make-solid");
    pushIfApplicable("region.heal-and-fill");
    pushIfApplicable("relation.walk-conflicts");
    pushIfApplicable("relation.toggle-driving");

    // The downgrade, offered where the refusal happened.
    //
    // A driving imposition that cannot hold leaves the reference measurement
    // available and the choice with the user — that is the whole of what stage 5
    // changed about a mechanism stage 4 already had. Without this the choice
    // existed and the offer did not: the user's one route to the measurement was
    // knowing the palette spells it with "(reference)" and going to look. Top of
    // the strip, because it is the answer to the thing that just happened.
    if(const auto &downgrade = session.presentation().downgrade) {
        if(const Action *action = impositionAction(downgrade->kind, Strength::Reference)) {
            SurfaceEntry entry;
            entry.action = action;
            entry.applicable = true;
            entry.title = std::string(action->title);
            if(downgrade->assignment != 0) {
                entry.arguments.set("assignment", static_cast<double>(downgrade->assignment));
            }
            entry.score = policy.usageCeiling * policy.usageWeight + 2.0;
            out.push_back(std::move(entry));
        }
    }

    std::stable_sort(out.begin(), out.end(),
                     [](const SurfaceEntry &a, const SurfaceEntry &b) {
                         return a.score > b.score;
                     });
    if(out.size() > policy.stripLimit) out.resize(policy.stripLimit);
    return out;
}

std::vector<SurfaceEntry> paletteEntries(const Session &session, std::string_view query) {
    const ActionContext context = contextOf(session);
    std::vector<SurfaceEntry> out;
    for(const Action &action : actions()) {
        if(!matchesQuery(action.title, query) && !matchesQuery(action.name, query)) continue;
        SurfaceEntry entry;
        entry.action = &action;
        entry.title = std::string(action.title);
        // Applicability is reported, never used to filter. Inapplicable
        // commands dim rather than vanish, so the catalogue is the same shape
        // every time it is opened and learning where something sits is worth
        // doing.
        entry.applicable =
            action.applicable == nullptr || action.applicable(context, action);
        out.push_back(std::move(entry));
    }
    return out;
}

}  // namespace paroculus
