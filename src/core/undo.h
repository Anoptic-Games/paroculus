// Command-sourced undo over declarations, with seeds.
//
// Replaying declarations alone can legally produce a different solution branch
// than the one the user was looking at, because a constraint system generically
// admits several and Newton picks the one nearest its seed. So every record
// carries the seeds of the entities it touched, and undo restores what was
// seen, not merely what was meant.
//
// Stage 1 has no solver, so the seed spans are recorded and round-tripped while
// staying empty. The shape is here from the first journal entry deliberately:
// retrofitting branch fidelity into an existing undo stream is the expensive
// path, and this is the cheap moment to pay for it.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/document.h"

namespace paroculus {

// One user gesture. Composite gestures — a placement plus the inferences it
// committed — bundle into a single record, which is what makes place-with-snaps
// one undo step while declining a single inference stays its own finer step.
struct UndoRecord {
    std::string label;
    std::vector<Command> forward;
    std::vector<Command> inverse;  // in the order they must be applied
};

// PLANS asked every undo record to carry the seeds of the components it
// touched, so that replaying declarations could not land on a different
// solution branch than the user was shown. That property holds, by a different
// mechanism than the one built for it: committing solved values is an ordinary
// SetRecord over the entity's seeds, so the branch rides the command stream and
// every inverse restores it exactly. The spans this record used to carry were
// never written, never read, and half-present state is what stage 8's async
// work would have trusted.

// Invariant: the document a journal is attached to is only ever mutated through
// that journal. A direct Document::apply behind the journal's back leaves the
// stack describing a document that no longer exists.
class UndoJournal {
public:
    // Applies every command as one undoable step, computing each inverse
    // against the state that command will actually see.
    //
    // All-or-nothing: if any command is refused, the ones already applied are
    // rolled back and nothing is journalled. A half-applied gesture is not a
    // state the user can reason about or undo out of.
    // Returns the first error, or None.
    CommandError applyStep(Document &doc, std::string label, std::vector<Command> commands);

    // Convenience for the overwhelmingly common single-command step.
    CommandError applyStep(Document &doc, std::string label, Command command);

    bool canUndo() const { return depth_ > 0; }
    bool canRedo() const { return depth_ < records_.size(); }

    // Returns false when there is nothing to undo.
    bool undo(Document &doc);
    bool redo(Document &doc);

    // Records above the cursor are dropped by the next step, exactly as a
    // linear history requires.
    size_t depth() const { return depth_; }
    const std::vector<UndoRecord> &records() const { return records_; }

    void clear();

private:
    std::vector<UndoRecord> records_;
    // Records below the cursor are applied; records at and above are undone.
    size_t depth_ = 0;
};

}  // namespace paroculus
