#include "core/undo.h"

#include <utility>

namespace paroculus {

// Each inverse is computed against the state its own command will see, not
// against the state at the start of the step. A step that adds an entity and
// then constrains it has an inverse for the constraint that only exists once
// the entity does, so inverting up front would be wrong for every composite.
CommandError UndoJournal::applyStep(Document &doc, std::string label,
                                    std::vector<Command> commands) {
    if(commands.empty()) return CommandError::None;

    UndoRecord record;
    record.label = std::move(label);
    record.forward = std::move(commands);

    std::vector<Command> undoStack;  // inverses in application order
    for(Command &c : record.forward) {
        const std::optional<Command> inverse = doc.invert(c);
        const CommandResult result = inverse ? doc.apply(c)
                                             : CommandResult{CommandError::NoSuchRecord, 0};
        if(!result.ok()) {
            // Roll back what this step already did, newest first, so the
            // document's records are exactly as the caller found them. ID
            // watermarks stay where they moved to: an ID consumed by a failed
            // attempt is spent, because never-reuse outranks byte-identity.
            for(auto it = undoStack.rbegin(); it != undoStack.rend(); ++it) doc.apply(*it);
            return result.error;
        }
        // Pin the ID the add just took, so redo reinstates that record rather
        // than allocating a second one.
        pinAllocatedId(c, result.allocated);
        undoStack.push_back(*inverse);
    }

    // Stored newest-first, which is the order undo must apply them in.
    record.inverse.assign(undoStack.rbegin(), undoStack.rend());

    // A new step truncates any redo tail: history is linear.
    records_.resize(depth_);
    records_.push_back(std::move(record));
    depth_ = records_.size();
    return CommandError::None;
}

CommandError UndoJournal::applyStep(Document &doc, std::string label, Command command) {
    std::vector<Command> one;
    one.push_back(std::move(command));
    return applyStep(doc, std::move(label), std::move(one));
}

bool UndoJournal::undo(Document &doc) {
    if(!canUndo()) return false;
    const UndoRecord &record = records_[depth_ - 1];
    for(const Command &c : record.inverse) doc.apply(c);
    depth_--;
    return true;
}

bool UndoJournal::redo(Document &doc) {
    if(!canRedo()) return false;
    const UndoRecord &record = records_[depth_];
    for(const Command &c : record.forward) doc.apply(c);
    depth_++;
    return true;
}

void UndoJournal::recordSeedsAfter(std::vector<SeedSpan> seeds) {
    if(depth_ == 0) return;
    records_[depth_ - 1].seedsAfter = std::move(seeds);
}

void UndoJournal::clear() {
    records_.clear();
    depth_ = 0;
}

}  // namespace paroculus
