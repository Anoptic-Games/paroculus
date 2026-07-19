#include "interact/session.h"

#include <algorithm>

#include "interact/script.h"

namespace paroculus {
namespace {

// Every Nth drag frame runs the extra hard-pin solve that names the resisting
// constraints. A counter rather than a clock, so a scripted gesture produces
// the same diagnoses on every machine and every run.
constexpr int RESISTANCE_INTERVAL = 4;

}  // namespace

Session::Session(Document &doc, UndoJournal &journal)
    : doc_(&doc), journal_(&journal), topology_(doc) {
    refresh();
}

void Session::setViewport(const Viewport &viewport) {
    viewport_ = viewport;
    // Recorded as a step rather than as file-level preamble: a session that
    // pans or zooms mid-gesture ran under several views, and replaying its
    // screen coordinates under only the first one would put every event after
    // the change somewhere else.
    if(recorder_ != nullptr) recorder_->viewport(viewport);
}

Pose Session::pose() const {
    // Three layers, and the order is the whole story: the document's committed
    // seeds, the solved result over them, the in-flight drag over that.
    Pose pose(*doc_);
    pose.overlay(settled_.params());
    if(drag_) pose.overlay(drag_->context().params());
    return pose;
}

void Session::refresh() {
    topology_.markDirty();

    // Solved geometry is a derived cache, so it is kept here and never written
    // back as seeds. Committing it on open would dirty a document nobody
    // edited — and because a Newton solve is not a fixpoint in the last bits,
    // opening the same file twice would produce two different files. Seeds are
    // authored intent and branch choice; they change when the user edits.
    settled_ = SolveContext::forWholeDocument(*doc_);
    if(!settled_.empty()) {
        SolveOptions options;
        options.diagnoseFailures = false;
        const SolveOutcome outcome = solve(*doc_, settled_, options);
        // The readout follows the document, so an undo or a deletion updates it
        // rather than leaving a stale number on screen.
        presentation_.dof = outcome.dof;
        presentation_.status = outcome.status;
        // A failed solve leaves the parameters at the seeds it was handed, so
        // an overlay of them would only restate the document. Drop it and show
        // what the document says, which is the honest thing to show.
        if(!outcome.ok()) settled_ = SolveContext();
    }

    index_.rebuild(pose());
}

void Session::setTool(ToolKind kind) {
    if(recorder_ != nullptr) recorder_->tool(kind);
    // Switching abandons what the previous tool had in flight. Nothing was
    // committed while it was in flight, so there is nothing to roll back.
    tool_.reset();
    numeric_.cancel();
    imposePending_ = false;
    confirmedOffers_.clear();
    pendingStartSnaps_.clear();
    presentation_.snapCandidates.clear();
    presentation_.inferred.clear();
    haveLastCursor_ = false;
    switch(kind) {
        case ToolKind::Line:      tool_ = std::make_unique<LineTool>(); break;
        case ToolKind::Circle:    tool_ = std::make_unique<CircleTool>(); break;
        case ToolKind::Arc:       tool_ = std::make_unique<ArcTool>(); break;
        case ToolKind::Rectangle: tool_ = std::make_unique<RectangleTool>(); break;
        case ToolKind::Select:    break;
    }
    presentation_.tool = kind;
    // A creation tool is verb-noun and owns the pointer, so a selection left
    // over from before would only be a set the user cannot act on without
    // leaving the tool first.
    if(tool_) selection_.clear();
    refreshToolPresentation();
}

void Session::refreshToolPresentation() {
    if(!tool_) {
        presentation_.toolPreview = ToolPreview();
        presentation_.toolParameters.clear();
        presentation_.numericActive = false;
        presentation_.numericText.clear();
        return;
    }
    presentation_.toolPreview = tool_->preview();
    const std::span<const ToolParameter> parameters = tool_->parameters();
    presentation_.toolParameters.assign(parameters.begin(), parameters.end());
    presentation_.numericActive = numeric_.active();
    presentation_.numericTarget = numeric_.target();
    presentation_.numericText = numeric_.text();
}

SnapResult Session::inferAt(Point cursor) const {
    SnapRequest request;
    request.cursor = cursor;
    if(tool_) {
        const ToolPreview p = tool_->preview();
        request.haveAnchor = p.active;
        request.anchor = p.from;
        request.anchorEntity = p.fromEntity;
    }
    request.recent = recentSnaps_;
    request.confirmed = confirmedOffers_;
    return snap(*doc_, pose(), index_, viewport_.view, request, snapPolicy_);
}

std::vector<GlyphMark> Session::glyphs() const {
    GlyphContext context;
    context.selected = selection_.items();
    context.hovered = presentation_.hovered;
    context.fresh = presentation_.inferred;
    context.cursor = lastCursor_;
    context.haveCursor = haveLastCursor_;

    const Pose current = pose();
    std::vector<GlyphMark> out = visibleGlyphs(*doc_, current, viewport_.view, viewport_.width,
                                               viewport_.height, context, glyphPolicy_);

    // Ghosts ride above the budget: a relation about to be declared is exactly
    // the one the user needs to see, and there are only ever a handful.
    if(tool_) {
        const ToolPreview preview = tool_->preview();
        const std::vector<GlyphMark> ghosts =
            ghostGlyphs(presentation_.snapCandidates, preview.active ? preview.to : lastCursor_,
                        preview.active, preview.from);
        out.insert(out.end(), ghosts.begin(), ghosts.end());
    }
    return out;
}


void Session::type(char c) {
    if(recorder_ != nullptr) recorder_->type(c);
    if(!tool_ || tool_->parameters().empty()) return;
    if(!numeric_.active()) numeric_.begin(0);
    numeric_.type(c);
    refreshToolPresentation();
}

void Session::numericBackspace() {
    if(recorder_ != nullptr) recorder_->numeric(ScriptStep::Kind::NumericBackspace);
    numeric_.backspace();
    refreshToolPresentation();
}

void Session::numericCancel() {
    if(recorder_ != nullptr) recorder_->numeric(ScriptStep::Kind::NumericCancel);
    numeric_.cancel();
    refreshToolPresentation();
}

void Session::numericAdvance() {
    if(recorder_ != nullptr) recorder_->numeric(ScriptStep::Kind::NumericAdvance);
    if(!tool_) return;
    const size_t count = tool_->parameters().size();
    if(count == 0) return;
    if(!numeric_.active()) {
        numeric_.begin(0);
    } else {
        numeric_.retarget((numeric_.target() + 1) % count);
    }
    refreshToolPresentation();
}

void Session::numericResolve(bool impose) {
    if(recorder_ != nullptr) {
        recorder_->numeric(impose ? ScriptStep::Kind::NumericImpose
                                  : ScriptStep::Kind::NumericResolve);
    }
    if(!tool_ || !numeric_.active()) return;
    const std::optional<double> value = numeric_.value();
    if(!value) return;

    const size_t target = numeric_.target();
    // Exactly, not nearly. The whole reason this entrance exists is that a drag
    // cannot land on a number.
    if(!tool_->setParameter(target, *value)) return;
    numeric_.cancel();

    // Held until the placement commits, because the dimension has to name
    // entities that do not exist until then.
    imposePending_ = impose;
    imposeTarget_ = target;
    imposeValue_ = *value;

    refreshToolPresentation();
}

void Session::confirmOffer(size_t index) {
    if(recorder_ != nullptr) recorder_->confirm(index);
    if(!tool_) return;
    const std::vector<SnapCandidate> offers = presentation_.offers();
    if(index >= offers.size()) return;

    const std::pair<SnapKind, EntityId> key{offers[index].kind, offers[index].target};
    if(std::find(confirmedOffers_.begin(), confirmedOffers_.end(), key) !=
       confirmedOffers_.end()) {
        return;
    }
    confirmedOffers_.push_back(key);

    // Re-run inference so the ghost moves to where the confirmed relation puts
    // it immediately, rather than at the next mouse move. Preview shows truth,
    // and the truth changed the moment the user pressed the key.
    if(haveLastCursor_) {
        const SnapResult inference = inferAt(lastCursor_);
        presentation_.snapCandidates = inference.candidates;
        tool_->move(*doc_, inference.placement);
        refreshToolPresentation();
    }
}

void Session::declineInference(size_t index) {
    if(recorder_ != nullptr) recorder_->decline(index);
    if(index >= presentation_.inferred.size()) return;
    const ConstraintId id = presentation_.inferred[index];
    // The constraint may already be gone — undone, or declined twice by a
    // script written against a different document. Silently doing nothing is
    // right; refusing loudly would make replay brittle for no gain.
    if(!doc_->constraints().contains(id)) return;

    // Its own step, so the decline is itself undoable and the placement it came
    // from stays intact.
    if(journal_->applyStep(*doc_, "decline", RemoveRecord<ConstraintRecord>{id}) ==
       CommandError::None) {
        presentation_.inferred.erase(presentation_.inferred.begin() +
                                     static_cast<std::ptrdiff_t>(index));
        refresh();
    }
}

void Session::rememberSnaps(const std::vector<SnapCandidate> &committed) {
    for(const SnapCandidate &c : committed) {
        const auto it = std::find(recentSnaps_.begin(), recentSnaps_.end(), c.kind);
        if(it != recentSnaps_.end()) recentSnaps_.erase(it);
        recentSnaps_.insert(recentSnaps_.begin(), c.kind);
    }
    if(recentSnaps_.size() > snapPolicy_.recentDepth) recentSnaps_.resize(snapPolicy_.recentDepth);
}

void Session::runTool(ToolOutput output, std::vector<ConstraintId> inferred) {
    if(output.commands.empty()) {
        refreshToolPresentation();
        return;
    }
    // Geometry and its inferences go in as one step. Undo removes the placement
    // and what it declared together, because they are one gesture; declining a
    // single inference is the finer step, and it is a separate action.
    //
    // One placement is one undo step. The tool is told it landed only if the
    // journal accepted every command, so a refused step cannot leave the tool
    // chaining off geometry the document does not have.
    const CommandError error =
        journal_->applyStep(*doc_, std::move(output.label), std::move(output.commands));
    if(error == CommandError::None) {
        tool_->committed();
        // No silent changes: what was declared is named at the moment it is
        // declared, rather than discovered later by hovering.
        presentation_.inferred = std::move(inferred);
        refresh();
    }
    refreshToolPresentation();
}

void Session::handle(const PointerEvent &event) {
    if(recorder_ != nullptr) recorder_->pointer(event);
    presentation_.rippledOffScreen = false;

    if(tool_) {
        // A creation tool owns the pointer outright: no hit testing, no
        // marquee, no drag. Verb-noun means the noun does not exist yet, so
        // there is nothing under the cursor for the pointer to mean instead.
        //
        // Preview shows truth. One inference call feeds the ghost on a move and
        // the declarations on a press, so a previewed candidate set that
        // differs from the committed one is not a reachable state — it is one
        // code path, not two that have to be kept in agreement.
        switch(event.action) {
            case PointerAction::Press: {
                if(event.button != Button::Left) return;
                const SnapResult inference = inferAt(event.document);
                presentation_.snapCandidates = inference.candidates;

                ToolOutput out = tool_->press(*doc_, inference.placement);
                if(out.commands.empty()) {
                    // The click that opens a chain declares nothing yet: the
                    // point it binds has no id until a segment justifies it.
                    // Hold the relations for one click rather than dropping
                    // them, or starting a run on an existing corner would
                    // silently place a free point on top of it.
                    pendingStartSnaps_ = inference.autoCommitted();
                    refreshToolPresentation();
                    return;
                }

                // Constraint ids are claimed the same way the tool claims entity
                // ids, so what inference declared can be named exactly rather
                // than recovered afterwards by guessing which records are new.
                //
                // Past whatever the tool already claimed: a macro emits its own
                // constraints from the same allocator, and starting again at the
                // watermark would collide with them. applyStep is all-or-nothing,
                // so that collision does not lose one relation — it loses the
                // whole shape.
                uint32_t nextConstraint = doc_->constraints().allocator().next();
                for(const Command &existing : out.commands) {
                    if(const auto *add = std::get_if<AddRecord<ConstraintRecord>>(&existing)) {
                        nextConstraint = std::max(nextConstraint, add->record.id.value() + 1);
                    }
                }
                std::vector<SnapCandidate> committed;
                std::vector<ConstraintId> inferred;
                auto declare = [&](const SnapCandidate &c, EntityId point, EntityId segment) {
                    std::optional<ConstraintRecord> r = constraintFor(c, point, segment);
                    if(!r) return;
                    r->id = ConstraintId(nextConstraint++);
                    inferred.push_back(r->id);
                    out.commands.push_back(AddRecord<ConstraintRecord>{*r});
                    committed.push_back(c);
                };

                if(out.placedStart.valid()) {
                    for(const SnapCandidate &c : pendingStartSnaps_) {
                        declare(c, out.placedStart, EntityId());
                    }
                }
                for(const SnapCandidate &c : inference.autoCommitted()) {
                    declare(c, out.placedPoint, out.placedSegment);
                }
                // A typed value that was imposed becomes a driving dimension
                // in the same step as the geometry it measures, so undo takes
                // back the placement and its dimension together.
                if(imposePending_) {
                    if(const std::optional<ConstraintRecord> dimension =
                           tool_->dimensionFor(imposeTarget_, imposeValue_, out)) {
                        ConstraintRecord r = *dimension;
                        r.id = ConstraintId(nextConstraint++);
                        inferred.push_back(r.id);
                        out.commands.push_back(AddRecord<ConstraintRecord>{r});
                    }
                    imposePending_ = false;
                }

                pendingStartSnaps_.clear();
                // A confirmation is about one placement, not about the tool.
                confirmedOffers_.clear();
                runTool(std::move(out), std::move(inferred));
                rememberSnaps(committed);
                return;
            }
            case PointerAction::Move: {
                lastCursor_ = event.document;
                haveLastCursor_ = true;
                const SnapResult inference = inferAt(event.document);
                presentation_.snapCandidates = inference.candidates;
                tool_->move(*doc_, inference.placement);
                refreshToolPresentation();
                return;
            }
            case PointerAction::Release:
                return;
        }
        return;
    }

    const Pose current = pose();

    switch(event.action) {
        case PointerAction::Move: {
            if(pressActive_) {
                const double travelled = (event.screen - pressScreen_).norm();
                // A sloppy click stays a click until it has travelled far
                // enough to mean something else.
                if(!dragStarted_ && travelled > policy_.dragThreshold) {
                    dragStarted_ = true;
                    if(pressed_.valid()) beginDrag(pressed_, event.document);
                }
                if(drag_) {
                    updateDrag(event.document);
                } else if(dragStarted_) {
                    presentation_.marqueeActive = true;
                    presentation_.marqueeTo = event.screen;
                }
                return;
            }
            const std::optional<Hit> hit =
                hitTest(current, index_, viewport_.view, event.screen, policy_,
                        selection_.items());
            presentation_.hovered = hit ? hit->entity : EntityId();
            return;
        }

        case PointerAction::Press: {
            if(event.button != Button::Left) return;
            pressActive_ = true;
            dragStarted_ = false;
            pressScreen_ = event.screen;
            presentation_.marqueeFrom = event.screen;
            presentation_.marqueeTo = event.screen;

            const std::optional<Hit> hit =
                hitTest(current, index_, viewport_.view, event.screen, policy_,
                        selection_.items());
            pressed_ = hit ? hit->entity : EntityId();

            if(!pressed_.valid()) {
                // Empty space: a click clears, a drag becomes a marquee.
                if(!has(event.modifiers, Modifier::Shift)) selection_.clear();
                return;
            }
            if(has(event.modifiers, Modifier::Shift)) {
                selection_.toggle(pressed_);
            } else if(!selection_.contains(pressed_)) {
                // Clicking outside the selection replaces it; clicking inside
                // keeps it, so a multi-selection can be dragged as one thing.
                selection_.set(connectedRun(*doc_, topology_, pressed_));
            }
            return;
        }

        case PointerAction::Release: {
            if(event.button != Button::Left) return;
            if(drag_) {
                endDrag();
            } else if(dragStarted_ && presentation_.marqueeActive) {
                std::vector<EntityId> caught =
                    marquee(current, viewport_.view, presentation_.marqueeFrom, event.screen);
                if(has(event.modifiers, Modifier::Shift)) {
                    for(EntityId id : caught) selection_.add(id);
                } else {
                    selection_.set(std::move(caught));
                }
            }
            pressActive_ = false;
            dragStarted_ = false;
            pressed_ = EntityId();
            presentation_.marqueeActive = false;
            return;
        }
    }
}

void Session::handle(Key key, Modifier modifiers) {
    if(recorder_ != nullptr) recorder_->key(key, modifiers);
    presentation_.rippledOffScreen = false;
    switch(key) {
        case Key::Escape:
            // Inside a tool, Esc first ends what is in flight and only then
            // leaves the tool. Two presses get home from anywhere, and
            // selection is where home is.
            if(numeric_.active()) {
                // Esc ascends one level at a time: it takes back the typed
                // field before it takes back the placement.
                numericCancel();
                return;
            }
            if(tool_) {
                // Ending the chain abandons the placement, and confirmations
                // belong to the placement.
                confirmedOffers_.clear();
                pendingStartSnaps_.clear();
                if(!tool_->escape()) setTool(ToolKind::Select);
                refreshToolPresentation();
                return;
            }
            // Esc ascends a level of depth, and clears once at the home state.
            // Selection is where Esc always eventually lands.
            if(!selection_.ascend(*doc_, topology_)) selection_.clear();
            return;

        case Key::Delete:
            deleteSelection();
            return;

        case Key::Undo:
            if(journal_->undo(*doc_)) refresh();
            return;

        case Key::Redo:
            if(journal_->redo(*doc_)) refresh();
            return;

        case Key::Enter:
            // One extra key turns the measurement into a declaration.
            numericResolve(has(modifiers, Modifier::Shift));
            return;

        case Key::Tab:
            numericAdvance();
            return;
    }
    (void)modifiers;
}

void Session::beginDrag(EntityId grabbed, Point cursor) {
    drag_ = DragSession::begin(*doc_, topology_, grabbed, policy_);
    if(!drag_) return;
    presentation_.dragging = true;
    updateCount_ = 0;
    updateDrag(cursor);
}

void Session::updateDrag(Point cursor) {
    if(!drag_) return;
    const bool diagnose = (updateCount_++ % RESISTANCE_INTERVAL) == 0;
    const DragUpdate update = drag_->update(*doc_, viewport_, cursor, diagnose);

    presentation_.saturated = update.saturated;
    presentation_.rippledOffScreen = update.rippledOffScreen;
    presentation_.solveMicroseconds = update.microseconds;
    // Attribution persists between diagnosis frames, so the highlight does not
    // strobe at the interval rather than at the resistance.
    if(diagnose || !update.saturated) presentation_.resisting = update.resisting;
}

void Session::endDrag() {
    if(!drag_) return;
    // Release commits what is on screen. Nothing springs back, and the whole
    // gesture is one undo step.
    std::vector<Command> commit = drag_->commit(*doc_);
    drag_.reset();
    presentation_.dragging = false;
    presentation_.saturated = false;
    presentation_.resisting.clear();

    if(!commit.empty()) journal_->applyStep(*doc_, "drag", std::move(commit));
    refresh();
}

void Session::deleteSelection() {
    presentation_.deletedEntities = 0;
    presentation_.deletedRelations = 0;
    if(selection_.empty()) return;

    // One gesture, one undo step, whatever it took with it.
    //
    // Each cascade is computed against the document as it stands, so two
    // selected entities sharing a dependent name it twice. The second removal
    // would be refused and roll the whole step back, so dedupe by
    // (command alternative, record id) before applying.
    std::vector<std::pair<size_t, uint32_t>> seen;
    std::vector<Command> step;

    auto identify = [](const Command &c) {
        return std::visit(
            [](const auto &command) -> uint32_t {
                using C = std::decay_t<decltype(command)>;
                if constexpr(requires { command.id; }) {
                    return command.id.value();
                } else {
                    return command.record.id.value();
                }
            },
            c);
    };

    for(EntityId id : selection_.items()) {
        for(const Command &c : deletionStep(*doc_, id)) {
            const std::pair<size_t, uint32_t> key{c.index(), identify(c)};
            if(std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
            seen.push_back(key);
            step.push_back(c);
        }
    }
    if(step.empty()) return;

    // Counts, not a confirmation dialog. The user is told what went, after it
    // went, and undo is one keystroke away.
    for(const Command &c : step) {
        if(std::holds_alternative<RemoveRecord<EntityRecord>>(c)) {
            presentation_.deletedEntities++;
        } else {
            presentation_.deletedRelations++;
        }
    }

    if(journal_->applyStep(*doc_, "delete", std::move(step)) == CommandError::None) {
        selection_.clear();
        refresh();
    } else {
        presentation_.deletedEntities = 0;
        presentation_.deletedRelations = 0;
    }
}

}  // namespace paroculus
