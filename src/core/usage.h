// What this document's author reaches for.
//
// Ranking among simultaneous offers is contextual and document-local: recent
// choices in this document weigh in, deterministically and inspectably, with no
// global learned magic. This is the whole of that context — a count per
// constraint kind, incremented when one is imposed.
//
// Ancillary and droppable, both words load-bearing. Nothing about what the
// document *means* depends on it: a file that loses the section opens to the
// same drawing with the strip ranked by taxonomy order alone. That is what
// lets it live outside the command layer, which is where it has to live —
// reaching for a relation is not an edit, and an undo that rewound the strip's
// ranking would be undoing something the user never did.
//
// A dense array rather than a map, so iteration order is the taxonomy's order
// and determinism is structural rather than remembered.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "core/taxonomy.h"

namespace paroculus {

class UsageHistory {
public:
    // One more use of `kind`. Saturates rather than wrapping: a count that rolled
    // over would silently demote the kind the user reaches for most.
    void note(ConstraintKind kind);

    uint32_t count(ConstraintKind kind) const {
        return counts_[static_cast<size_t>(kind)];
    }

    // Restores a count wholesale, which is what loading is. Not an edit: no
    // command, no journal entry, no inverse.
    void set(ConstraintKind kind, uint32_t count) {
        counts_[static_cast<size_t>(kind)] = count;
    }

    // The kinds with a nonzero count, in taxonomy order. What serialization
    // writes, and why a document that has imposed nothing writes no section at
    // all rather than twenty-two zeroes.
    std::vector<std::pair<ConstraintKind, uint32_t>> entries() const;

    bool empty() const;

    friend bool operator==(const UsageHistory &a, const UsageHistory &b) {
        return a.counts_ == b.counts_;
    }
    friend bool operator!=(const UsageHistory &a, const UsageHistory &b) { return !(a == b); }

private:
    std::array<uint32_t, CONSTRAINT_KINDS.size()> counts_{};
};

}  // namespace paroculus
