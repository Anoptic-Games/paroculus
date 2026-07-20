// The action surface, as projections.
//
// Menus, the context strip, the command palette, keyboard dispatch and the
// scripting harness are all projections of one action table. That is the
// property this file exists to keep true: it computes what each surface shows
// and nothing else, so no surface has to decide for itself what applies. A
// surface that reasoned about applicability on its own could offer what the
// model would refuse, and the bug would look like a model bug rather than a UI
// one.
//
// Toolkit-free, like the rest of interact. What a strip entry looks like is the
// shell's business; which entries exist and in what order is not, because that
// is a policy and a policy inside a Qt event handler is a policy no headless
// test can reach.
//
// Surface discipline, from PRINCIPLES: the persistent surfaces are spatially
// stable — the palette does not reshuffle with context, and inapplicable
// actions dim rather than vanish — while context sensitivity arrives additively
// through the transient strip near the work. Ranking within the strip is
// contextual; placement of the permanent furniture is not. The two functions
// below are exactly that pair, and the difference between them is the whole
// discipline.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "interact/impose.h"
#include "interact/registry.h"

namespace paroculus {

class Session;

// One thing a surface can show.
struct SurfaceEntry {
    const Action *action = nullptr;
    // Filled in for the surfaces that invoke with arguments. The strip offers
    // one entry per reading of an ambiguous relation, so the entry carries the
    // reading rather than making the shell work it out again.
    ActionArguments arguments;

    // What to show. Copied rather than referenced because an ambiguous relation
    // needs its reading named — "length ratio (A:B)" and "length ratio (B:A)"
    // are two entries over one action.
    std::string title;

    // Whether it can be run right now. Inapplicable entries are returned rather
    // than filtered out by paletteEntries, because the permanent surfaces dim
    // what does not apply instead of hiding it: a command that vanishes is a
    // command the user cannot learn.
    bool applicable = false;

    // Ranking, higher first. Meaningful in the strip and always zero in the
    // palette, which does not rank.
    double score = 0.0;
};

// What the transient strip near the work shows for the current selection.
//
// Contextual and additive: the relations this selection admits, ranked by what
// this document has reached for before, one entry per reading. Truncated to the
// policy's limit, with the remainder reachable through the palette — which is
// the surface that shows everything and ranks nothing.
//
// Empty for an empty selection. The strip is about what is selected; with
// nothing selected there is nothing contextual to say, and filling it with
// general commands would make the transient surface a second permanent one.
std::vector<SurfaceEntry> stripEntries(const Session &session);

// The whole catalogue, in the table's own stable order, filtered by `query`.
//
// Order is the table's and never the ranking's. The palette is permanent
// furniture: its entries do not reshuffle with context, because muscle memory
// is worth more there than relevance, and a user who learned where a command
// sits should find it there next time.
//
// query: matched case-insensitively against the title and the action's name,
//   as a subsequence rather than a substring — "prl" finds "Parallel" — which
//   is what makes a palette faster than a menu. An empty query matches
//   everything.
std::vector<SurfaceEntry> paletteEntries(const Session &session, std::string_view query);

// Whether `query` matches `text` as a case-insensitive subsequence. Exposed
// because it is the palette's whole search policy and therefore worth pinning
// by test rather than rediscovering from behaviour.
bool matchesQuery(std::string_view text, std::string_view query);

// How an ambiguous relation's reading is named: "A:B" over the operands, in the
// order the assignment puts them. What the surface shows when it asks which way
// round, and what the preview then answers.
std::string describeAssignment(const Document &doc, ConstraintKind kind,
                               const RoleAssignment &assignment);

}  // namespace paroculus
