#include "interact/session.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "interact/loops.h"
#include "interact/registry.h"
#include "interact/script.h"

namespace paroculus {
namespace {

// Every Nth drag frame runs the extra hard-pin solve that names the resisting
// constraints. A counter rather than a clock, so a scripted gesture produces
// the same diagnoses on every machine and every run.
constexpr int RESISTANCE_INTERVAL = 4;

// Which of two component outcomes one readout should report.
//
// Redundant outranks plain okay because it is a warning the user is entitled
// to; anything that did not solve outranks both, because a component the
// document cannot satisfy is the thing that has to be visible even when every
// other component is fine.
SolveStatus moreSevere(SolveStatus a, SolveStatus b) {
    auto rank = [](SolveStatus s) {
        if(s == SolveStatus::Okay) return 0;
        if(s == SolveStatus::RedundantOkay) return 1;
        return 2;
    };
    return rank(b) > rank(a) ? b : a;
}

// The same rule over candidate verdicts, for a readout summarising several at
// once: a warning outranks fine, and a refusal outranks a warning.
CandidateVerdict moreSevere(CandidateVerdict a, CandidateVerdict b) {
    auto rank = [](CandidateVerdict v) {
        if(v == CandidateVerdict::Consistent) return 0;
        if(v == CandidateVerdict::Redundant) return 1;
        return 2;
    };
    return rank(b) > rank(a) ? b : a;
}

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

// Which invisible geometry took part in moving something the user can see.
//
// Hidden still constrains — that is the whole point of hiding rather than
// deleting — but a drawing that rearranges itself under the influence of
// something not on screen is exactly the haunted feeling the no-silent-changes
// policy exists to prevent. So the event is emitted on the narrow, checkable
// condition PLANS names: a relation binding both an invisible operand and a
// visible one, where the visible one moved.
//
// before, after: the same component's parameter spans either side of a solve.
// Accumulates into the presentation rather than replacing it, because one
// refresh solves every component in turn and each may implicate its own.
void Session::noteHiddenInfluence(const std::vector<SeedSpan> &before,
                                  const std::vector<SeedSpan> &after) {
    auto moved = [&](EntityId id) {
        const SeedSpan *a = nullptr;
        const SeedSpan *b = nullptr;
        for(const SeedSpan &s : before) {
            if(s.entity == id) a = &s;
        }
        for(const SeedSpan &s : after) {
            if(s.entity == id) b = &s;
        }
        return a != nullptr && b != nullptr && a->seeds != b->seeds;
    };
    // A segment has no parameters of its own, so "did it move" is a question
    // about the points defining it.
    auto stirred = [&](EntityId id) {
        if(moved(id)) return true;
        const EntityRecord *e = doc_->entities().find(id);
        if(e == nullptr) return false;
        for(size_t i = 0; i < entityInfo(e->kind).pointCount; i++) {
            if(moved(e->points[i])) return true;
        }
        return false;
    };

    for(const ConstraintRecord &c : doc_->constraints().records()) {
        if(!c.driving) continue;
        const size_t n = boundOperandCount(c);
        std::vector<EntityId> hidden;
        bool visibleMoved = false;
        for(size_t i = 0; i < n; i++) {
            const EntityId id = c.operands[i];
            if(isVisible(*doc_, id)) {
                visibleMoved = visibleMoved || stirred(id);
            } else {
                hidden.push_back(id);
            }
        }
        if(!visibleMoved || hidden.empty()) continue;
        for(EntityId id : hidden) {
            if(std::find(presentation_.hiddenInfluences.begin(),
                         presentation_.hiddenInfluences.end(),
                         id) == presentation_.hiddenInfluences.end()) {
                presentation_.hiddenInfluences.push_back(id);
            }
        }
    }
}

void Session::refresh() {
    topology_.markDirty();
    presentation_.hiddenInfluences.clear();

    // Solved geometry is a derived cache, so it is kept here and never written
    // back as seeds. Committing it on open would dirty a document nobody
    // edited — and because a Newton solve is not a fixpoint in the last bits,
    // opening the same file twice would produce two different files. Seeds are
    // authored intent and branch choice; they change when the user edits.
    //
    // Starts at the committed seeds and takes solved values back per component,
    // so a component that does not solve simply keeps what it had.
    settled_ = SolveContext::forWholeDocument(*doc_);

    // Per component, not as one system.
    //
    // A document is several independent systems, and one of them failing says
    // nothing about the others. Solving them together means one contradictory
    // relation blanks every healthy component's display back to its committed
    // seeds — the whole canvas stops showing solved geometry because one corner
    // of it is over-constrained. PRINCIPLES has the document stay editable with
    // geometry holding its last feasible solution, and there being only states
    // with more or less diagnostic adornment; a canvas-wide fallback is neither.
    //
    // It also keeps the whole-document solve off the per-edit path, which is
    // what the scale hypothesis wants anyway.
    SolveOptions options;
    options.diagnoseFailures = false;

    int dof = 0;
    SolveStatus worst = SolveStatus::Okay;
    bool solvedAnything = false;

    // Grouped in one pass. Asking the topology for each component's members
    // separately would scan the document once per component, which is quadratic
    // on exactly the documents this is meant to make cheap.
    const size_t components = topology_.componentCount();
    std::vector<std::vector<EntityId>> grouped(components);
    for(const EntityRecord &e : doc_->entities().records()) {
        const ComponentId id = topology_.componentOf(e.id);
        if(id != NO_COMPONENT && id < components) grouped[id].push_back(e.id);
    }

    for(std::vector<EntityId> &members : grouped) {
        if(members.empty()) continue;
        SolveContext component = SolveContext::forMembers(*doc_, std::move(members));
        if(component.empty()) continue;

        const std::vector<SeedSpan> before = component.params();
        const SolveOutcome outcome = solve(*doc_, component, options);
        worst = moreSevere(worst, outcome.status);
        if(!outcome.ok()) continue;  // this component holds its committed seeds

        noteHiddenInfluence(before, component.params());
        dof += outcome.dof;
        solvedAnything = true;
        for(const SeedSpan &solved : component.params()) {
            for(SeedSpan &into : settled_.params()) {
                if(into.entity == solved.entity) into.seeds = solved.seeds;
            }
        }
    }

    // The readout follows the document, so an undo or a deletion updates it
    // rather than leaving a stale number on screen. Summed over what solved:
    // a component that did not has no degrees of freedom worth reporting, and
    // the status is what says so.
    //
    // Unguarded by the component count: an emptied document has nothing to
    // solve, and freezing the last numbers on screen would report degrees of
    // freedom for geometry the user just deleted. Nothing has no freedom and
    // nothing is wrong with it.
    presentation_.dof = components == 0 ? 0 : (solvedAnything ? dof : -1);
    presentation_.status = worst;

    // Recomputed rather than accumulated: a region is broken because of what it
    // is now, and one that was mended by an undo has to stop saying so.
    presentation_.brokenRegions = brokenRegions(*doc_);
    presentation_.brokenTags = brokenTags(*doc_);

    index_.rebuild(pose());
}

void Session::setTool(ToolKind kind) {
    if(recorder_ != nullptr) recorder_->tool(kind);
    // Switching abandons what the previous tool had in flight. Nothing was
    // committed while it was in flight, so there is nothing to roll back.
    tool_.reset();
    numeric_.cancel();
    confirmedOffers_.clear();
    pendingSnaps_.clear();
    presentation_.snapCandidates.clear();
    presentation_.inferred.clear();
    presentation_.closedLoop.clear();
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
        // An anchor means a segment is in flight, not merely that a band is on
        // screen. Horizontal, parallel and the rest are properties of a
        // segment, and a circle's radius or a rectangle's diagonal is not one —
        // generating them there would offer relations about a line the commit
        // is never going to create.
        request.haveAnchor = p.active && p.willPlace.segment;
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
        // With nothing in flight the next click is the one that opens a shape,
        // and every creation tool opens with a single point — a line's first
        // end, a circle's centre, an arc's start, a rectangle's first corner.
        // So that is what the ghost promises, and it is exactly what
        // commitPlacement binds an opening click's held snaps against.
        //
        // Reporting no roles here instead read as "this click places nothing",
        // which is true of the command list and false of the gesture: the
        // relation waits in pendingSnaps_ and binds when the shape justifies
        // the point. The user aiming at a vertex saw no promise and got a
        // coincidence anyway, which is the recall half of WYSIWYG failing.
        const PlacementRoles opening{true, false, false};
        const std::vector<GlyphMark> ghosts =
            ghostGlyphs(presentation_.snapCandidates,
                        preview.active ? preview.placement : lastCursor_,
                        preview.active ? preview.willPlace : opening, preview.from);
        out.insert(out.end(), ghosts.begin(), ghosts.end());
    }
    return out;
}


void Session::type(char c) {
    if(recorder_ != nullptr) recorder_->type(c);

    // A drag of existing geometry has a numeric twin too, and it is the one
    // PRINCIPLES describes: start dragging a vertex, type 45, Enter. The field
    // opens on the first measurement the drag is adjusting; Tab picks another,
    // which is how "the length under adjustment" stops being ambiguous the
    // moment a vertex belongs to two segments.
    if(drag_) {
        if(dragDimensions_.empty()) return;
        if(!numeric_.active()) numeric_.begin(0);
        numeric_.type(c);
        refreshDragPresentation();
        return;
    }

    if(!tool_ || tool_->parameters().empty()) return;
    if(!numeric_.active()) numeric_.begin(0);
    numeric_.type(c);
    refreshToolPresentation();
}

void Session::numericBackspace() {
    if(recorder_ != nullptr) recorder_->numeric(ScriptStep::Kind::NumericBackspace);
    numeric_.backspace();
    if(drag_) {
        refreshDragPresentation();
        return;
    }
    refreshToolPresentation();
}

void Session::numericCancel() {
    if(recorder_ != nullptr) recorder_->numeric(ScriptStep::Kind::NumericCancel);
    applyNumericCancel();
}

void Session::applyNumericCancel() {
    numeric_.cancel();
    if(drag_) {
        refreshDragPresentation();
        return;
    }
    refreshToolPresentation();
}

void Session::numericAdvance() {
    if(recorder_ != nullptr) recorder_->numeric(ScriptStep::Kind::NumericAdvance);
    applyNumericAdvance();
}

void Session::applyNumericAdvance() {
    // Tab is what disambiguates a drag's measurements, so it cycles the
    // dimensions rather than a tool's fields whenever one is in flight.
    if(drag_) {
        if(dragDimensions_.empty()) return;
        if(!numeric_.active()) {
            numeric_.begin(0);
        } else {
            numeric_.retarget((numeric_.target() + 1) % dragDimensions_.size());
        }
        refreshDragPresentation();
        return;
    }
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
    applyNumericResolve(impose);
}

void Session::applyNumericResolve(bool impose) {
    if(drag_) {
        applyDragResolve(impose);
        return;
    }
    if(!tool_ || !numeric_.active()) return;
    const std::optional<double> value = numeric_.value();
    if(!value) return;

    const size_t target = numeric_.target();
    // Exactly, not nearly. The whole reason this entrance exists is that a drag
    // cannot land on a number.
    if(!tool_->setParameter(target, *value)) return;
    numeric_.cancel();

    // Where the digits put the placement. The tool has already moved itself
    // there, and this is the position the commit uses — asking the pointer
    // again would hand the placement back to the hand that could not hit the
    // number in the first place.
    const Point resolved = tool_->preview().placement;

    // Inference still runs, at the resolved position rather than at the
    // pointer's, but it may not move anything: a candidate is committed by the
    // placement landing on it, and this placement has landed where the digits
    // said. So only relations already true there are declared — a nearby vertex
    // is near, not touched, and declaring a coincidence to it would drag the
    // geometry off the value the user typed and lose it silently.
    const SnapResult inference = inferAt(resolved);
    presentation_.snapCandidates = inference.candidates;
    std::vector<SnapCandidate> declaring;
    for(const SnapCandidate &c : inference.autoCommitted()) {
        if(std::abs(c.placement.x - resolved.x) < 1e-9 &&
           std::abs(c.placement.y - resolved.y) < 1e-9) {
            declaring.push_back(c);
        }
    }

    std::optional<Imposition> imposition;
    if(impose) imposition = Imposition{target, *value};
    if(!commitPlacement(resolved, declaring, imposition)) refreshToolPresentation();
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
    // from stays intact. Through deletionStep because a declined relation may be
    // one a tag was built on — a rectangle's squaring relations are exactly what
    // decline removes — and the tag shrinks rather than being left naming a
    // record that is gone.
    const ConstraintId one[] = {id};
    if(journal_->applyStep(*doc_, "decline", deletionStep(*doc_, one)) == CommandError::None) {
        presentation_.inferred.erase(presentation_.inferred.begin() +
                                     static_cast<std::ptrdiff_t>(index));
        refresh();
    }
}

bool Session::commitPlacement(Point placement, const std::vector<SnapCandidate> &declaring,
                              const std::optional<Imposition> &impose) {
    if(!tool_) return false;
    ToolOutput out = tool_->press(*doc_, placement);
    if(out.commands.empty()) return false;

    // Constraint ids are claimed the same way the tool claims entity ids, so
    // what inference declared can be named exactly rather than recovered
    // afterwards by guessing which records are new.
    //
    // Past whatever the tool already claimed: a macro emits its own constraints
    // from the same allocator, and starting again at the watermark would
    // collide with them. applyStep is all-or-nothing, so that collision does not
    // lose one relation — it loses the whole shape.
    uint32_t nextConstraint = doc_->constraints().allocator().next();
    for(const Command &existing : out.commands) {
        if(const auto *add = std::get_if<AddRecord<ConstraintRecord>>(&existing)) {
            nextConstraint = std::max(nextConstraint, add->record.id.value() + 1);
        }
    }
    std::vector<SnapCandidate> committed;
    std::vector<ConstraintId> inferred;
    auto declare = [&](const SnapCandidate &c, const PlacementSubjects &subjects) {
        std::optional<ConstraintRecord> r = constraintFor(c, subjects);
        if(!r) return;
        r->id = ConstraintId(nextConstraint++);
        inferred.push_back(r->id);
        out.commands.push_back(AddRecord<ConstraintRecord>{*r});
        committed.push_back(c);
    };

    // The held clicks, each against the entity it turned out to create. Zipped
    // in order and only as far as both run: a tool that named fewer entities
    // than there were held clicks drops the surplus rather than binding them to
    // the wrong point.
    for(size_t i = 0; i < pendingSnaps_.size() && i < out.opened.size(); i++) {
        PlacementSubjects opened;
        opened.point = out.opened[i];
        for(const SnapCandidate &c : pendingSnaps_[i]) declare(c, opened);
    }
    for(const SnapCandidate &c : declaring) declare(c, out.placed);

    // A typed value that was imposed becomes a driving dimension in the same
    // step as the geometry it measures, so undo takes back the placement and its
    // dimension together.
    // All four together, never two of them. Setting the verdict and the
    // conflicting set while leaving attribution and the downgrade offer holding
    // whatever the previous imposition left there reports this placement's
    // verdict beside the last one's attribution — and a surface reading them as
    // one answer cannot tell.
    presentation_.impositionVerdict = CandidateVerdict::Consistent;
    presentation_.conflicting.clear();
    presentation_.conflictAttributed = false;
    presentation_.downgrade.reset();
    if(impose) {
        if(const std::optional<ConstraintRecord> dimension =
               tool_->dimensionFor(impose->target, impose->value, out)) {
            ConstraintRecord r = *dimension;

            // Over-constraint's first moment, as PRINCIPLES puts it: the
            // candidate is solved speculatively before it is committed.
            //
            // Against a copy with this step already applied, because the
            // dimension names entities the step is about to create and a
            // speculative constraint cannot be checked against a document that
            // does not yet hold its operands. One copy per imposition, which is
            // a keystroke rather than a frame — the no-copy rule is about the
            // interaction path, and this is not on it.
            Document speculative = *doc_;
            for(const Command &c : out.commands) speculative.apply(c);
            Topology speculativeTopology(speculative);
            const CandidateCheck check = checkCandidate(speculative, speculativeTopology, r);

            presentation_.impositionVerdict = check.verdict;
            presentation_.conflicting = check.conflicting;
            presentation_.conflictAttributed = check.attributed;
            // The downgrade PRINCIPLES names: a dimension that cannot hold is
            // added as a driven reference measurement rather than as a driving
            // constraint. The geometry still lands, the value is still recorded
            // and still visible, and nothing silently drives to a contradiction.
            //
            // Taken automatically here because stage 4 has no surface to offer
            // it on. Stage 5 turns this into the offer, with the conflicting set
            // highlighted — the choice moves to the user, the mechanism does
            // not change.
            if(!check.committable()) r.driving = false;

            r.id = ConstraintId(nextConstraint++);
            inferred.push_back(r.id);
            out.commands.push_back(AddRecord<ConstraintRecord>{r});
        }
    }

    pendingSnaps_.clear();
    // A confirmation is about one placement, not about the tool.
    confirmedOffers_.clear();
    // Recency is a record of what the user actually declared. A refused step
    // declared nothing, and letting it rank would promote a snap kind on the
    // strength of a placement the document never accepted.
    if(!runTool(std::move(out), std::move(inferred))) return false;
    rememberSnaps(committed);
    return true;
}

void Session::rememberSnaps(const std::vector<SnapCandidate> &committed) {
    for(const SnapCandidate &c : committed) {
        const auto it = std::find(recentSnaps_.begin(), recentSnaps_.end(), c.kind);
        if(it != recentSnaps_.end()) recentSnaps_.erase(it);
        recentSnaps_.insert(recentSnaps_.begin(), c.kind);
    }
    if(recentSnaps_.size() > snapPolicy_.recentDepth) recentSnaps_.resize(snapPolicy_.recentDepth);
}

bool Session::runTool(ToolOutput output, std::vector<ConstraintId> inferred) {
    // Kept before the commands are moved out, so closure can be asked about the
    // edge the placement created.
    const EntityId closureSeed = output.placed.segment.valid() ? output.placed.segment
                                 : output.placed.point.valid() ? output.placed.point
                                 : output.opened.empty()       ? EntityId()
                                                               : output.opened.front();

    if(output.commands.empty()) {
        refreshToolPresentation();
        return false;
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
    const bool landed = error == CommandError::None;
    if(landed) {
        tool_->committed();
        // No silent changes: what was declared is named at the moment it is
        // declared, rather than discovered later by hovering.
        presentation_.inferred = std::move(inferred);
        refresh();

        // Did that placement close an outline, or come near to it? Asked after
        // refresh, because the topology has to see the coincidence the
        // placement just declared before it can tell a closed run from a nearly
        // closed one — which is the whole distinction between the two offers.
        refreshLoopOffers(closureSeed);
    }
    refreshToolPresentation();
    return landed;
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

                if(!commitPlacement(inference.placement, inference.autoCommitted(),
                                    std::nullopt)) {
                    // A click that opens a shape declares nothing yet: the
                    // point it binds has no id until the shape justifies it.
                    // Hold the relations rather than dropping them, or starting
                    // a run on an existing corner would silently place a free
                    // point on top of it. Appended, because the next click may
                    // open too — an arc opens twice before it commits.
                    pendingSnaps_.push_back(inference.autoCommitted());
                    refreshToolPresentation();
                }
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

            // A relation is reachable from its mark, but only when the cursor is
            // genuinely nearer the mark than the geometry.
            //
            // "Adorners over geometry" is the priority policy, and taken as an
            // unconditional precedence here it is wrong: a mark sits a few
            // pixels off the vertex it annotates, well inside that vertex's own
            // hit radius, so a mark that always won would swallow the press that
            // starts a drag. Dragging is the primary probe for what can still
            // move; picking a relation is the occasional act. Comparing
            // distances keeps every mark reachable without spending the gesture
            // the tool is mostly used for.
            //
            // The set asked is the one on screen, so what is pickable is exactly
            // what is visible: the glyph budget decides both, and a mark the
            // overlay dropped is not a mark the user is aiming at.
            const std::vector<GlyphMark> marks = glyphs();
            const std::optional<GlyphHit> mark =
                hitGlyph(marks, viewport_.view, event.screen);
            if(mark && (!hit || mark->distance < hit->distance)) {
                const bool additive = has(event.modifiers, Modifier::Shift);
                // Geometry goes first, then the relation — Selection::set
                // clears both lists, so selecting the relation before clearing
                // the geometry would clear the relation too.
                if(!additive) selection_.set(std::vector<EntityId>{});
                selectConstraint(mark->constraint, additive);
                pressed_ = EntityId();
                return;
            }

            if(!pressed_.valid()) {
                // Empty space: a click clears, a drag becomes a marquee.
                if(!has(event.modifiers, Modifier::Shift)) selection_.clear();
                refreshSelectionOffers();
                return;
            }
            // A second click descends a level: shape to edges, edges to points.
            // There is no edit-mode wall — object versus component is depth,
            // and Esc walks back up.
            //
            // The selection is not replaced here. The click before this one
            // already made it, and re-selecting would reset the depth the
            // descent is about to add to, so a double-click on an already
            // descended shape would never reach the rung below.
            if(event.clicks >= 2) {
                selection_.descend(*doc_, topology_);
                refreshSelectionOffers();
                return;
            }
            if(has(event.modifiers, Modifier::Shift)) {
                selection_.toggle(pressed_);
            } else if(!selection_.contains(pressed_)) {
                // Clicking outside the selection replaces it; clicking inside
                // keeps it, so a multi-selection can be dragged as one thing.
                selection_.set(connectedRun(*doc_, topology_, pressed_));
            }
            refreshSelectionOffers();
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
                refreshSelectionOffers();
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
                // field before it takes back the placement. The keystroke is
                // already recorded, so this must not record a second step.
                applyNumericCancel();
                return;
            }
            // A drag in flight is the next thing Esc takes back.
            //
            // Cancelling is dropping the context: the drag's pose lives there
            // and nothing has been committed, so the geometry springs back to
            // its seeds by the same mechanism that showed it moving. The press
            // flags go too, or the gesture the user has already abandoned would
            // become a marquee on the next move and act on the release.
            if(drag_) {
                cancelDrag();
                return;
            }
            if(tool_) {
                // Ending the chain abandons the placement, and confirmations
                // belong to the placement.
                confirmedOffers_.clear();
                pendingSnaps_.clear();
                // The offers went with it. Left standing they ghost a relation
                // about a placement that no longer exists, until the next move
                // happens to recompute them — and a tool that keeps running
                // after Esc may not get one.
                presentation_.snapCandidates.clear();
                if(!tool_->escape()) setTool(ToolKind::Select);
                refreshToolPresentation();
                return;
            }
            // Esc ascends a level of depth, and clears once at the home state.
            // Selection is where Esc always eventually lands.
            if(!selection_.ascend(*doc_, topology_)) selection_.clear();
            refreshSelectionOffers();
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
            applyNumericResolve(has(modifiers, Modifier::Shift));
            return;

        case Key::Tab:
            applyNumericAdvance();
            return;
    }
    (void)modifiers;
}

void Session::refreshDragPresentation() {
    presentation_.toolParameters.clear();
    dragLabels_.clear();
    if(!drag_) {
        presentation_.numericActive = false;
        presentation_.numericText.clear();
        return;
    }

    dragDimensions_ = dragDimensions(*doc_, pose(), drag_->grabbed());

    // Named by what they measure, so two lengths off one vertex are told apart
    // by the segments they are lengths of rather than by their order.
    dragLabels_.reserve(dragDimensions_.size());
    for(const DragDimension &d : dragDimensions_) {
        dragLabels_.push_back(d.kind == ConstraintKind::Radius
                                  ? "radius"
                                  : "length " + std::to_string(d.subject.value()));
    }
    for(size_t i = 0; i < dragDimensions_.size(); i++) {
        presentation_.toolParameters.push_back(
            ToolParameter{dragLabels_[i].c_str(), dragDimensions_[i].value});
    }

    presentation_.numericActive = numeric_.active();
    presentation_.numericTarget = numeric_.target();
    presentation_.numericText = numeric_.text();
}

void Session::applyDragResolve(bool impose) {
    if(!drag_ || !numeric_.active()) return;
    const std::optional<double> value = numeric_.value();
    if(!value) return;
    const size_t target = numeric_.target();
    if(target >= dragDimensions_.size()) return;

    const ConstraintRecord dimension = dragDimensions_[target].recordAt(*value);
    // Exactly, not nearly. A drag cannot land on a number, which is the whole
    // reason this entrance exists — so the target stops being the cursor and
    // becomes the value.
    if(!drag_->resolve(*doc_, dimension)) {
        // The constraints cannot reach it. The pose is unchanged and the field
        // stays open, so the user can try another number rather than watching
        // the geometry go somewhere nobody asked for.
        refreshDragPresentation();
        return;
    }
    numeric_.cancel();

    std::vector<Command> step = drag_->commit(*doc_);
    // The numeric twin of a handle drag owes the same rewrite the pointer
    // release does. Without it the typed value is committed as geometry while
    // the dimension it was suppressing comes back on the next solve and pulls
    // the drawing straight off the number the user just confirmed.
    appendHandleRewrites(step);

    // Imposing pins the value as a driving dimension, in the same undo step as
    // the motion it measures — which is why no imposition outlives the gesture
    // that asked for it. Checked first: a dimension that cannot hold is
    // committed as a reference measurement rather than driving the document
    // into a contradiction.
    ConstraintId imposed;
    if(impose) {
        ConstraintRecord record = dimension;
        Document probe = *doc_;
        for(const Command &c : step) probe.apply(c);
        Topology probeTopology(probe);
        const CandidateCheck check = checkCandidate(probe, probeTopology, record);
        presentation_.impositionVerdict = check.verdict;
        presentation_.conflicting = check.conflicting;
        presentation_.conflictAttributed = check.attributed;
        // No offer on this path: the numeric one downgrades automatically,
        // because there the value is what the user was supplying and losing it
        // would be worse than driving nothing with it. Cleared rather than left,
        // or the strip would go on offering a measurement about a selection the
        // drag has moved past.
        presentation_.downgrade.reset();
        record.driving = check.committable();
        step.push_back(AddRecord<ConstraintRecord>{record});
    }

    drag_.reset();
    dragDimensions_.clear();
    presentation_.dragging = false;
    presentation_.saturated = false;
    presentation_.resisting.clear();

    // Whether the step landed decides what may be said about it. The
    // imposition is identified as the newest constraint record, which is only
    // the record this step added if this step added one — on a refusal the
    // newest is whatever was there before, and naming it would attribute the
    // dimension to an unrelated relation and note usage for a kind nobody
    // imposed.
    bool landed = true;
    if(!step.empty()) {
        landed = journal_->applyStep(*doc_, impose ? "drag to value" : "drag",
                                     std::move(step)) == CommandError::None;
    }
    refresh();
    if(landed && impose && !doc_->constraints().records().empty()) {
        imposed = doc_->constraints().records().back().id;
        doc_->noteUsage(dimension.kind);
    }
    presentation_.imposed = imposed;
    refreshDragPresentation();
}

void Session::beginDrag(EntityId grabbed, Point cursor) {
    // The whole selection comes along, and so does every group any of it
    // belongs to.
    //
    // The two travel differently and have to. A selected parameter is held —
    // the solver favours leaving it be, and what has to give gives elsewhere.
    // A grouped one is carried: a group usually joins geometry that is not
    // connected, so there is no equation to hold it and locality would keep it
    // still, so it translates rigidly with the grab instead. Both are defaults
    // rather than declarations: grouping two things says nothing about what the
    // document means, and the grouping survives the drag either way.
    std::vector<EntityId> held = selection_.items();
    if(std::find(held.begin(), held.end(), grabbed) == held.end()) held.push_back(grabbed);

    std::vector<EntityId> carried;
    for(const GroupRecord &g : doc_->groups().records()) {
        const bool touched = std::any_of(g.members.begin(), g.members.end(), [&](EntityId m) {
            return m == grabbed || std::find(held.begin(), held.end(), m) != held.end();
        });
        if(!touched) continue;
        for(EntityId m : g.members) {
            if(std::find(carried.begin(), carried.end(), m) == carried.end()) {
                carried.push_back(m);
            }
        }
    }

    // A rectangle's corner handle drives the dimensions holding its sides.
    //
    // Without this the handle is a lie: a rectangle the user has sized by typing
    // 120 into its width field has a driving distance on that edge, and grabbing
    // the corner saturates against it immediately — the handle is drawn, it can
    // be grabbed, and it cannot move. Driving is what a driving dimension does,
    // so the answer is not to weaken the relation but to let the handle edit its
    // value, which is a value edit and allowed to move the drawing.
    //
    // So the drag suppresses them, tracks freely, and commitDrag rewrites the
    // slots from where the corner landed. Only for a whole tag, and only for the
    // dimensions the tag's own sides carry: a distance the user imposed between
    // this rectangle and something else is not the rectangle's to rewrite, and it
    // resists exactly as it should.
    // Only when the handle is actually on offer.
    //
    // The affordance is the tag's, and a tag is named by naming what it is over —
    // the same rule that decides a fill is selected and the same one the overlay
    // draws handles by. Suppressing on corner membership alone would mean any
    // ordinary vertex drag of any rectangle in the document silently rewrote a
    // driving dimension the user had pinned, with no handle drawn and nothing
    // saying so. A drag is a handle drag exactly when the user can see the
    // handle.
    handleDimensions_.clear();
    for(TagId id : selectedTags()) {
        const std::optional<RectangleFrame> frame = rectangleFrame(*doc_, id);
        // Either end of any of its edges. A corner is two coincident points and
        // the frame names one of them; grabbing the other is grabbing the same
        // corner, and a handle that worked on one point of a pair and not the
        // other would be a handle that works half the time.
        if(!frame || !isRectangleCorner(*doc_, *frame, grabbed)) continue;
        for(EntityId edge : {frame->widthEdge, frame->heightEdge}) {
            const ConstraintId dimension = edgeDimension(*doc_, edge);
            const ConstraintRecord *c = doc_->constraints().find(dimension);
            if(c == nullptr) continue;
            // A value authored somewhere else is not the handle's to overwrite.
            // A width driven by a named document parameter is driven by that
            // parameter for every rectangle sharing it, so rewriting it here
            // would either move all of them or — writing a bare constant —
            // silently sever this one from the rest. The handle resists instead,
            // which is what a dimension the user authored elsewhere should do.
            if(!c->value.isConstant()) continue;
            handleDimensions_.push_back(dimension);
        }
    }

    drag_ = DragSession::begin(*doc_, topology_, grabbed, held, policy_, carried,
                               handleDimensions_);
    if(!drag_) return;
    presentation_.dragging = true;
    updateCount_ = 0;
    updateDrag(cursor);
    refreshDragPresentation();
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
    // The strip's numbers follow the geometry, so what the user is about to
    // type over is what they are looking at.
    refreshDragPresentation();
}

// Abandons a drag without committing it.
//
// Nothing to undo, because nothing was applied: the in-flight pose lived in the
// drag's own solve context and dropping it is the whole of the cancel. That is
// the same property that makes a drag cheap — the document is not touched until
// release — read from the other end.
void Session::cancelDrag() {
    if(!drag_) return;
    drag_.reset();
    dragDimensions_.clear();
    numeric_.cancel();
    presentation_.dragging = false;
    presentation_.saturated = false;
    presentation_.resisting.clear();
    presentation_.marqueeActive = false;
    pressActive_ = false;
    dragStarted_ = false;
    pressed_ = EntityId();
    refresh();
}

// The handle's half of the bargain: the drag ran with these dimensions
// suppressed so the geometry could move freely, and now the values follow it.
//
// Appended to the drag's own commit rather than journalled separately, so one
// undo takes back the resize and the numbers it wrote together — they are one
// gesture and splitting them would make undo take back a rectangle's size while
// leaving the dimension claiming otherwise.
//
// Called from both places a drag can end. The pointer release is the obvious
// one; the numeric twin is the one that is easy to miss, and missing it means a
// typed width is committed as geometry while the suppressed constraint comes
// straight back on the next solve and pulls the drawing off the number the user
// just confirmed.
//
// Measured from the pose the drag landed on rather than from the cursor: the
// cursor is where the hand was, and the corner is where the constraints that
// were *not* suppressed put it.
void Session::appendHandleRewrites(std::vector<Command> &commit) {
    if(!drag_ || handleDimensions_.empty()) {
        handleDimensions_.clear();
        return;
    }
    Pose landed(*doc_);
    landed.overlay(drag_->context().params());
    for(ConstraintId id : handleDimensions_) {
        const ConstraintRecord *c = doc_->constraints().find(id);
        if(c == nullptr) continue;
        const std::optional<Point> a = landed.point(c->operands[0]);
        const std::optional<Point> b = landed.point(c->operands[1]);
        if(!a || !b) continue;
        const double length = std::hypot(b->x - a->x, b->y - a->y);
        if(!(length > 0.0)) continue;
        ConstraintRecord next = *c;
        // A bare constant is safe here and only here: beginDrag refused to
        // suppress any dimension whose slot was an expression, so nothing with
        // provenance to sever ever reaches this line.
        next.value = Slot(length);
        if(next != *c) commit.push_back(SetRecord<ConstraintRecord>{next});
    }
    handleDimensions_.clear();
}

void Session::endDrag() {
    if(!drag_) return;
    // Release commits what is on screen. Nothing springs back, and the whole
    // gesture is one undo step.
    std::vector<Command> commit = drag_->commit(*doc_);

    appendHandleRewrites(commit);
    drag_.reset();
    dragDimensions_.clear();
    numeric_.cancel();
    presentation_.dragging = false;
    presentation_.saturated = false;
    presentation_.resisting.clear();

    if(!commit.empty()) journal_->applyStep(*doc_, "drag", std::move(commit));
    refresh();
}

// ---------------------------------------------------------------------------
// Imposition
// ---------------------------------------------------------------------------

std::vector<RelationOffer> Session::relationOffers() const {
    return relationsFor(*doc_, selection_.items(), surfacePolicy_);
}

void Session::select(std::vector<EntityId> ids) {
    selection_.set(std::move(ids));
    // The downgrade offer names a reading of the selection it was refused for,
    // so it means nothing about a different one. An offer that outlived its
    // selection would invoke a measurement over whatever is selected now.
    presentation_.downgrade.reset();
    refreshSelectionOffers();
}

void Session::selectConstraint(ConstraintId id, bool additive) {
    if(additive) {
        selection_.toggleConstraint(id);
    } else {
        selection_.setConstraints({id});
    }
}

std::optional<ImpositionPreview> Session::previewImposition(ConstraintKind kind,
                                                            size_t assignment,
                                                            std::optional<double> value) const {
    const std::vector<RoleAssignment> readings =
        assignmentsFor(*doc_, kind, selection_.items());
    if(assignment >= readings.size()) return std::nullopt;

    std::optional<ConstraintRecord> candidate =
        candidateFor(pose(), kind, readings[assignment], Strength::Impose);
    if(!candidate) return std::nullopt;
    // A supplied value replaces the captured one. That is a value edit rather
    // than an imposition, and the preview is where the user sees the difference
    // before paying for it.
    if(value && constraintInfo(kind).valueArity == 1) candidate->value = Slot(*value);

    return previewCandidate(*doc_, topology_, *candidate);
}

bool Session::commitCandidate(const ConstraintRecord &candidate, Strength strength,
                              size_t assignment, const ImpositionPreview &preview) {
    presentation_.impositionVerdict = preview.check.verdict;
    presentation_.conflicting = preview.check.conflicting;
    presentation_.conflictAttributed = preview.check.attributed;
    presentation_.impositionMotion = preview.motion;
    presentation_.downgrade.reset();
    presentation_.imposed = ConstraintId();

    if(strength != Strength::Reference && !preview.check.committable()) {
        // The choice moves to the user. Stage 4 downgraded silently because
        // there was no surface to ask on; here there is one, and committing a
        // reference measurement nobody asked for would be declaring something
        // other than what was requested.
        //
        // Named rather than flagged: the strip turns this into the entry that
        // invokes the measurement, so the offer sits where the refusal did.
        if(preview.check.verdict == CandidateVerdict::Inconsistent) {
            presentation_.downgrade = Presentation::Downgrade{candidate.kind, assignment};
        }
        return false;
    }

    // Measure-once records nothing. It applies the relation, keeps the geometry
    // that comes out, and throws the relation away — which is what "align these
    // now, remember nothing" is, and why it needs no record type of its own.
    if(strength == Strength::Measure) {
        if(preview.pose.empty()) return false;
        SolveContext moved = SolveContext::forWholeDocument(*doc_);
        for(SeedSpan &into : moved.params()) {
            for(const SeedSpan &from : preview.pose) {
                if(into.entity == from.entity) into.seeds = from.seeds;
            }
        }
        std::vector<Command> step = moved.commitCommands(*doc_);
        if(step.empty()) return true;  // it was already so; nothing to record
        if(journal_->applyStep(*doc_, "align", std::move(step)) != CommandError::None) {
            return false;
        }
        doc_->noteUsage(candidate.kind);
        refresh();
        return true;
    }

    ConstraintRecord record = candidate;
    record.driving = strength == Strength::Impose;
    if(journal_->applyStep(*doc_, "constrain", AddRecord<ConstraintRecord>{record}) !=
       CommandError::None) {
        return false;
    }
    // The journal allocated the id; find it by being the newest record of that
    // kind, which is what an add at a null id always produces.
    if(!doc_->constraints().records().empty()) {
        presentation_.imposed = doc_->constraints().records().back().id;
    }
    // Reaching for a relation is not an edit, so this rides outside the
    // journal: an undo takes back the constraint, not the fact that the user
    // once reached for it.
    doc_->noteUsage(candidate.kind);
    refresh();
    refreshLoopOffers(selection_.items().empty() ? EntityId() : selection_.items().front());
    return true;
}

void Session::recordAction(std::string_view name,
                           std::vector<std::pair<std::string, double>> arguments) {
    if(recorder_ != nullptr) recorder_->action(name, arguments);
}

bool Session::impose(ConstraintKind kind, Strength strength, size_t assignment,
                     std::optional<double> value) {
    if(const Action *action = impositionAction(kind, strength)) {
        std::vector<std::pair<std::string, double>> arguments;
        // Only what was asked for. Writing the defaults out would make every
        // recorded imposition carry two fields it did not need, and a
        // hand-edited script noisier to read than the gesture was to perform.
        if(assignment != 0) arguments.emplace_back("assignment", static_cast<double>(assignment));
        if(value) arguments.emplace_back("value", *value);
        recordAction(action->name, std::move(arguments));
    }

    const std::vector<RoleAssignment> readings =
        assignmentsFor(*doc_, kind, selection_.items());
    if(assignment >= readings.size()) return false;

    std::optional<ConstraintRecord> candidate =
        candidateFor(pose(), kind, readings[assignment], strength);
    if(!candidate) return false;
    if(value && constraintInfo(kind).valueArity == 1) candidate->value = Slot(*value);

    const ImpositionPreview preview = previewCandidate(*doc_, topology_, *candidate);
    return commitCandidate(*candidate, strength, assignment, preview);
}

bool Session::toggleDriving() {
    recordAction("relation.toggle-driving");
    if(selection_.constraints().empty()) return false;

    const Pose current = pose();
    CandidateVerdict promoted = CandidateVerdict::Consistent;
    std::vector<Command> step;
    for(ConstraintId id : selection_.constraints()) {
        const ConstraintRecord *r = doc_->constraints().find(id);
        if(r == nullptr) continue;
        ConstraintRecord flipped = *r;
        flipped.driving = !flipped.driving;

        // Promotion re-captures. A reference measurement is a live readout of
        // what the geometry is doing, so its slot is whatever it last drove at
        // and may be nothing like what it is showing. Promoting it to intent
        // means declaring what it is showing — which is imposition, and
        // imposition moves nothing. Carrying the stale slot forward instead
        // would make a toggle yank the drawing to a value the user was not
        // looking at.
        //
        // Demotion keeps the slot untouched: the value it was driving at is the
        // value it would drive at again, and losing it would make the toggle
        // one-way in everything but name.
        if(flipped.driving && constraintInfo(flipped.kind).valueArity == 1) {
            if(const std::optional<double> now = measure(current, flipped)) {
                flipped.value = Slot(*now);
            }
        }

        // Promotion is an imposition and is checked like one.
        //
        // Consistency cannot break — the value was just captured from the pose,
        // so it holds where the geometry already is — but redundancy can, and
        // redundancy is where later edits go to die. The imposition path raises
        // that flag for the identical declaration; a toggle that skipped it was
        // a quiet way to plant one. Checked against the document without this
        // measurement in it, since a driven record constrains nothing and
        // comparing the candidate to itself would call every promotion
        // redundant.
        if(flipped.driving) {
            Document without = *doc_;
            without.apply(RemoveRecord<ConstraintRecord>{id});
            Topology withoutTopology(without);
            const CandidateCheck check = checkCandidate(without, withoutTopology, flipped);
            promoted = moreSevere(promoted, check.verdict);
        }
        step.push_back(SetRecord<ConstraintRecord>{flipped});
    }
    if(step.empty()) return false;
    if(journal_->applyStep(*doc_, "toggle driving", std::move(step)) != CommandError::None) {
        return false;
    }
    // Flagged, never refused: a redundant relation is a warning the user is
    // entitled to rather than a fault today, and the toggle is theirs to make.
    presentation_.impositionVerdict = promoted;
    refresh();
    return true;
}

bool Session::selectConflicting() {
    recordAction("relation.walk-conflicts");
    if(presentation_.conflicting.empty()) return false;
    selection_.clear();
    selection_.setConstraints(presentation_.conflicting);
    return true;
}

// ---------------------------------------------------------------------------
// Regions
// ---------------------------------------------------------------------------

namespace {

// Whether some region already bounds exactly these edges.
//
// A set comparison rather than a sequence one: two walks of the same cycle can
// start at different edges and run the other way round, and they are the same
// boundary either way.
bool filledBy(const Document &doc, const std::vector<EntityId> &boundary) {
    for(const RegionRecord &r : doc.regions().records()) {
        if(r.op != CompositeOp::Outline) continue;
        if(r.boundary.size() != boundary.size()) continue;
        bool same = true;
        for(EntityId edge : boundary) {
            if(std::find(r.boundary.begin(), r.boundary.end(), edge) == r.boundary.end()) {
                same = false;
                break;
            }
        }
        if(same) return true;
    }
    return false;
}

}  // namespace

void Session::refreshLoopOffers(EntityId seed) {
    presentation_.closedLoop.clear();
    presentation_.healableLoop.clear();
    presentation_.healableGaps.clear();
    presentation_.healableWidestGap = 0.0;
    presentation_.crossing.reset();
    loopSeed_ = seed;
    if(!seed.valid()) return;

    if(const auto boundary = closedBoundaryContaining(*doc_, topology_, seed)) {
        // Unless it is already filled. The loop stays closed after make-solid,
        // so the offer stayed lit and a second press stacked an identical region
        // over the first — doubled alpha on screen, and an action that looks
        // like it did nothing. Compared as a set, because the ring walk may
        // enter the same cycle at a different edge.
        if(!filledBy(*doc_, *boundary)) presentation_.closedLoop = *boundary;
        return;
    }
    // How near is near enough is a property of the hand and the zoom, so the
    // pixel radius the snap policy holds is converted through the view rather
    // than being a document constant. A gap the user could not see at this zoom
    // is a gap they meant to close.
    const double tolerance = viewport_.view.toDocumentLength(snapPolicy_.pointRadius);
    if(const auto healable =
           healableLoopContaining(*doc_, topology_, pose(), seed, tolerance)) {
        presentation_.healableLoop = healable->boundary;
        presentation_.healableGaps = healable->gaps;
        presentation_.healableWidestGap = healable->widestGap;
        return;
    }

    // Neither offer stands, and the two ways that happens are different facts.
    // An area enclosed by crossing segments is enclosed by nothing the model can
    // name, and PLANS asks for that to be said rather than left as the same
    // silence as "these edges enclose nothing at all".
    //
    // Asked over the selection as well as the seed, because crossing edges are
    // exactly the ones no run walk joins: a selected pair can sit in two
    // components, and starting from either would never reach the other.
    std::vector<EntityId> among{seed};
    for(EntityId id : selection_.items()) {
        if(id != seed) among.push_back(id);
    }
    presentation_.crossing = crossingAmong(*doc_, topology_, pose(), among);
}

void Session::refreshSelectionOffers() {
    if(tool_) return;
    refreshLoopOffers(selection_.items().empty() ? EntityId() : selection_.items().front());
}

// The region a boundary becomes.
//
// The layer is the boundary's own, which is the only answer that keeps a fill
// and the outline defining it on the same layer — so hiding or locking that
// layer takes both, and the z-order the fill competes in is the one its geometry
// is drawn on. Taking the oldest layer record instead put every fill on the base
// layer whatever the user had drawn on, and the corpus could not see it because
// every corpus fill happened before any layer existed. The first edge's layer
// when they disagree: nothing stops a boundary crossing layers, and a fill has
// to be on exactly one.
//
// No style. A fill nobody styled reads as the outline it belongs to, which is
// what render draws for a null style; inheriting the lowest-ID style record
// meant the fill picked up a stroke style that was never about it.
static RegionRecord regionOver(const Document &doc, std::vector<EntityId> boundary) {
    RegionRecord r;
    r.boundary = std::move(boundary);
    for(EntityId edge : r.boundary) {
        if(const EntityRecord *e = doc.entities().find(edge)) {
            r.layer = e->layer;
            break;
        }
    }
    return r;
}

bool Session::makeSolid() {
    recordAction("region.make-solid");
    if(presentation_.closedLoop.empty()) return false;
    presentation_.filled = RegionId();

    const RegionRecord region = regionOver(*doc_, presentation_.closedLoop);
    if(journal_->applyStep(*doc_, "make solid", AddRecord<RegionRecord>{region}) !=
       CommandError::None) {
        return false;
    }
    if(!doc_->regions().records().empty()) {
        presentation_.filled = doc_->regions().records().back().id;
    }
    refresh();
    refreshLoopOffers(loopSeed_);
    return true;
}

bool Session::healAndFill() {
    recordAction("region.heal-and-fill");
    if(presentation_.healableLoop.empty()) return false;
    presentation_.filled = RegionId();

    HealableLoop loop;
    loop.boundary = presentation_.healableLoop;
    loop.gaps = presentation_.healableGaps;
    loop.widestGap = presentation_.healableWidestGap;

    // Healing and filling are one gesture and therefore one undo step: a user
    // who takes it back means the fill and the joints together, not the fill
    // and a drawing that has quietly moved.
    std::vector<Command> step = healingStep(*doc_, loop);
    if(step.empty()) return false;
    step.push_back(AddRecord<RegionRecord>{regionOver(*doc_, loop.boundary)});

    const Pose before = pose();
    if(journal_->applyStep(*doc_, "heal and fill", std::move(step)) != CommandError::None) {
        return false;
    }
    refresh();

    // The motion is the point of the action, so it is measured and reported
    // rather than assumed to be the gap it was offered as. The solver decides
    // how the epsilon is shared between the two joints; what was promised is
    // that nothing travels further than the widest gap.
    presentation_.impositionMotion = 0.0;
    const Pose after = pose();
    for(const EntityRecord &e : doc_->entities().records()) {
        const std::optional<Point> was = before.point(e.id);
        const std::optional<Point> now = after.point(e.id);
        if(!was || !now) continue;
        const double dx = now->x - was->x;
        const double dy = now->y - was->y;
        presentation_.impositionMotion =
            std::max(presentation_.impositionMotion, std::sqrt(dx * dx + dy * dy));
    }

    if(!doc_->regions().records().empty()) {
        presentation_.filled = doc_->regions().records().back().id;
    }
    refreshLoopOffers(loopSeed_);
    return true;
}

void Session::deleteSelection() {
    presentation_.deletedEntities = 0;
    presentation_.deletedRelations = 0;
    if(selection_.empty()) return;

    // One gesture, one undo step, whatever it took with it.
    //
    // The whole selection goes to one cascade rather than one cascade per
    // entity. Two selected edges of the same filled loop would otherwise
    // produce two shrinks of that region, each dropping a different edge, and
    // one of them would have to win — the wrong answer whichever it is.
    //
    // Relations are in that selection on the same terms geometry is. They are
    // selected through their glyphs and a walked conflict set is an ordinary
    // selection, so the gesture the conflict walk exists to enable — read the
    // set, delete the one that is wrong — is Delete on a constraint-only
    // selection, and it goes through the same cascade rather than a second path.
    std::vector<Command> step =
        deletionStep(*doc_, selection_.items(), selection_.constraints());
    if(step.empty()) return;

    // Counts, not a confirmation dialog. The user is told what went, after it
    // went, and undo is one keystroke away.
    //
    // Three numbers rather than two, because a deletion no longer only removes.
    // A region, tag or group that lost a member shrank rather than died, and
    // reporting a degradation as a deletion would say something went that is
    // still there — visibly, in its broken state.
    presentation_.degraded = 0;
    for(const Command &c : step) {
        if(std::holds_alternative<RemoveRecord<EntityRecord>>(c)) {
            presentation_.deletedEntities++;
        } else if(std::holds_alternative<RemoveRecord<ConstraintRecord>>(c)) {
            presentation_.deletedRelations++;
        } else {
            presentation_.degraded++;
        }
    }

    if(journal_->applyStep(*doc_, "delete", std::move(step)) == CommandError::None) {
        selection_.clear();
        refresh();
    } else {
        presentation_.deletedEntities = 0;
        presentation_.deletedRelations = 0;
        presentation_.degraded = 0;
    }
}

// ---------------------------------------------------------------------------
// Composition
// ---------------------------------------------------------------------------

namespace {

// The regions the selection reaches, in ID order.
//
// What counts as reached is core's regionSelected — the same question render
// asks to decide whether a fill draws as selected, so a highlighted fill and an
// actionable one are the same fill.
std::vector<RegionId> regionsIn(const Document &doc, const std::vector<EntityId> &selection) {
    std::vector<RegionId> out;
    for(const RegionRecord &r : doc.regions().records()) {
        if(!isTopLevelRegion(doc, r.id)) continue;
        if(regionSelected(doc, r, selection)) out.push_back(r.id);
    }
    return out;
}

}  // namespace

std::vector<RegionId> Session::selectedRegions() const {
    return regionsIn(*doc_, selection_.items());
}

// The layer an action acts on: the one named, else the one the selection is
// already on. Naming it is what makes every layer action headlessly invocable
// with an argument; defaulting to the selection's is what makes the keyboard
// path mean something without one.
LayerId Session::targetLayer(std::optional<double> named) const {
    if(named && *named > 0.0) return LayerId(static_cast<uint32_t>(*named));
    for(EntityId id : selection_.items()) {
        if(const EntityRecord *e = doc_->entities().find(id)) return e->layer;
    }
    return LayerId();
}

LayerId Session::topLayer() const {
    const LayerRecord *top = nullptr;
    for(const LayerRecord &l : doc_->layers().records()) {
        if(top == nullptr || l.order > top->order ||
           (l.order == top->order && top->id < l.id)) {
            top = &l;
        }
    }
    return top != nullptr ? top->id : LayerId();
}

bool Session::newLayer() {
    recordAction("layer.new");

    // Above everything, because a layer made now is one the user is about to
    // draw on. Ordering is signed and editable, so this is a starting position
    // rather than a policy.
    int32_t above = 0;
    for(const LayerRecord &l : doc_->layers().records()) above = std::max(above, l.order + 1);

    LayerRecord layer;
    layer.order = above;
    layer.name = "layer " + std::to_string(doc_->layers().size() + 1);
    if(journal_->applyStep(*doc_, "new layer", AddRecord<LayerRecord>{layer}) !=
       CommandError::None) {
        return false;
    }
    return true;
}

// Moving geometry between layers is organization and nothing else: no constraint
// is touched, no relation is dropped for crossing, and the partition does not
// notice. That is what "layers are organization, not semantics" costs to
// implement, which is nothing.
bool Session::assignLayer(LayerId layer) {
    recordAction("layer.assign", {{"layer", static_cast<double>(layer.value())}});
    if(selection_.empty()) return false;
    if(layer.valid() && doc_->layers().find(layer) == nullptr) return false;

    std::vector<Command> step;
    for(EntityId id : selection_.items()) {
        const EntityRecord *e = doc_->entities().find(id);
        if(e == nullptr || e->layer == layer) continue;
        EntityRecord moved = *e;
        moved.layer = layer;
        step.push_back(SetRecord<EntityRecord>{std::move(moved)});
    }
    if(step.empty()) return false;
    if(journal_->applyStep(*doc_, "assign layer", std::move(step)) != CommandError::None) {
        return false;
    }
    refresh();
    return true;
}

// Recorded under the name of the action that does it, never under a name of its
// own. A step naming something the registry does not have is a step replay
// cannot run, and the failure is silent — the script parses, the edit is gone.
bool Session::setLayerVisible(LayerId layer, bool visible) {
    recordAction(visible ? "layer.show" : "layer.hide",
                 {{"layer", static_cast<double>(layer.value())}});
    const LayerRecord *l = doc_->layers().find(layer);
    if(l == nullptr || l->visible == visible) return false;
    LayerRecord changed = *l;
    changed.visible = visible;
    if(journal_->applyStep(*doc_, "layer visibility", SetRecord<LayerRecord>{changed}) !=
       CommandError::None) {
        return false;
    }
    // Hiding does not re-solve anything — hidden still constrains, so the
    // geometry is unchanged — but the influence indication is computed from a
    // solve and has to be recomputed against the new visibility.
    refresh();
    return true;
}

bool Session::setLayerLocked(LayerId layer, bool locked) {
    recordAction(locked ? "layer.lock" : "layer.unlock",
                 {{"layer", static_cast<double>(layer.value())}});
    const LayerRecord *l = doc_->layers().find(layer);
    if(l == nullptr || l->locked == locked) return false;
    LayerRecord changed = *l;
    changed.locked = locked;
    if(journal_->applyStep(*doc_, "layer lock", SetRecord<LayerRecord>{changed}) !=
       CommandError::None) {
        return false;
    }
    // Locking removes unknowns, so the degree-of-freedom readout changes even
    // though nothing moved. Unlocking hands them back.
    refresh();
    return true;
}

bool Session::moveLayer(LayerId layer, int delta) {
    recordAction(delta < 0 ? "layer.lower" : "layer.raise",
                 {{"layer", static_cast<double>(layer.value())}});
    const LayerRecord *l = doc_->layers().find(layer);
    if(l == nullptr) return false;
    LayerRecord changed = *l;
    changed.order += delta;
    if(journal_->applyStep(*doc_, "layer order", SetRecord<LayerRecord>{changed}) !=
       CommandError::None) {
        return false;
    }
    // Nothing derived reads layer order today, so this buys nothing yet. It is
    // here because every other layer mutation refreshes and the one that does
    // not is the precedent the next one copies.
    refresh();
    return true;
}

// Union, subtract and intersect over live operands. Nothing is consumed: the
// operands stay exactly the records they were, keep their constraints, and go
// on being constrainable to each other — a hole staying concentric with its
// plate is an ordinary coincidence between two outlines that happen to be
// operands. What the composite adds is one record saying how they combine.
bool Session::composeRegions(CompositeOp op, bool reversed) {
    // Subtract records which way round it read the selection, because for it
    // that is a choice and not a consequence. Union and intersect take the
    // operands in the same order and say nothing, since neither can tell.
    if(op == CompositeOp::Subtract) {
        recordAction("region.subtract", {{"order", reversed ? 1.0 : 0.0}});
    } else {
        recordAction(std::string("region.") + compositeOpName(op));
    }
    if(op == CompositeOp::Outline) return false;

    std::vector<RegionId> operands = selectedRegions();
    if(operands.size() < 2) return false;

    // Operand order is what a subtract subtracts from, so it is decided by what
    // the user can see: the occlusion order, lowest first, so the default cuts
    // the upper region out of the lower one — the plate under the disc loses the
    // disc, which is what the picture looks like. ID order answered this before
    // and correlates with nothing on screen: it always cut the newer out of the
    // older, and there was no way to say the other thing. `reversed` is that
    // other way, and it rides on the action like an imposition's assignment
    // does. Ties break by ID so the reading is total, exactly as regionOrder's do.
    std::stable_sort(operands.begin(), operands.end(), [&](RegionId a, RegionId b) {
        const RegionRecord *ra = doc_->regions().find(a);
        const RegionRecord *rb = doc_->regions().find(b);
        if(ra == nullptr || rb == nullptr) return a < b;
        if(ra->z != rb->z) return ra->z < rb->z;
        return ra->id < rb->id;
    });
    if(reversed) std::reverse(operands.begin(), operands.end());

    const RegionRecord *first = doc_->regions().find(operands.front());
    if(first == nullptr) return false;

    RegionRecord composite;
    composite.op = op;
    composite.operands = std::move(operands);
    composite.layer = first->layer;
    composite.style = first->style;
    // On top of what it combines, so the composite is what the user sees where
    // the operands used to be.
    for(RegionId id : composite.operands) {
        if(const RegionRecord *r = doc_->regions().find(id)) {
            composite.z = std::max(composite.z, r->z);
        }
    }

    if(journal_->applyStep(*doc_, "compose regions", AddRecord<RegionRecord>{composite}) !=
       CommandError::None) {
        return false;
    }
    if(!doc_->regions().records().empty()) {
        presentation_.composed = doc_->regions().records().back().id;
    }
    refresh();
    return true;
}

// The inverse, and it is a real inverse: the composite record goes and the
// operands are exactly where they were, which is what "can be lifted back out"
// means when nothing was consumed to make it.
bool Session::liftComposite() {
    recordAction("region.lift");
    std::vector<Command> step;
    for(RegionId id : selectedRegions()) {
        const RegionRecord *r = doc_->regions().find(id);
        if(r == nullptr || r->op == CompositeOp::Outline) continue;
        step.push_back(RemoveRecord<RegionRecord>{id});
    }
    if(step.empty()) return false;
    if(journal_->applyStep(*doc_, "lift composite", std::move(step)) != CommandError::None) {
        return false;
    }
    presentation_.composed = RegionId();
    refresh();
    return true;
}

bool Session::togglePunch() {
    recordAction("region.punch");
    std::vector<Command> step;
    for(RegionId id : selectedRegions()) {
        RegionRecord r = *doc_->regions().find(id);
        r.punch = !r.punch;
        step.push_back(SetRecord<RegionRecord>{std::move(r)});
    }
    if(step.empty()) return false;
    if(journal_->applyStep(*doc_, "punch", std::move(step)) != CommandError::None) return false;
    refresh();
    return true;
}

bool Session::moveRegion(int delta) {
    recordAction(delta < 0 ? "region.lower" : "region.raise");
    std::vector<Command> step;
    for(RegionId id : selectedRegions()) {
        RegionRecord r = *doc_->regions().find(id);
        r.z += delta;
        step.push_back(SetRecord<RegionRecord>{std::move(r)});
    }
    if(step.empty()) return false;
    if(journal_->applyStep(*doc_, "region order", std::move(step)) != CommandError::None) {
        return false;
    }
    refresh();
    return true;
}

bool Session::groupSelection() {
    recordAction("group.create");
    if(selection_.size() < 2) return false;
    GroupRecord group;
    group.name = "group " + std::to_string(doc_->groups().size() + 1);
    group.members = selection_.items();
    return journal_->applyStep(*doc_, "group", AddRecord<GroupRecord>{std::move(group)}) ==
           CommandError::None;
}

// Dissolving takes the grouping and nothing else. A group owns no geometry, so
// there is nothing here that could be lost by ungrouping — the same property
// that makes a tag safe to dissolve.
bool Session::dissolveGroups() {
    recordAction("group.dissolve");
    std::vector<Command> step;
    for(const GroupRecord &g : doc_->groups().records()) {
        const bool touched = std::any_of(g.members.begin(), g.members.end(), [&](EntityId m) {
            return selection_.contains(m);
        });
        if(touched) step.push_back(RemoveRecord<GroupRecord>{g.id});
    }
    if(step.empty()) return false;
    return journal_->applyStep(*doc_, "dissolve group", std::move(step)) == CommandError::None;
}

// ---------------------------------------------------------------------------
// Structure operations
// ---------------------------------------------------------------------------

// Seeds, not the pose.
//
// A transform rewrites committed seeds, so its centre has to be a point in seed
// space or the cluster is translated as well as turned. Seeds are authored
// intent and are never written back from a solve, so any component the solver has
// moved — the normal case — has a pose that differs from them; taking the centre
// from the pose and applying it to the seeds rotates the seed cluster about a
// foreign point and the shape jumps instead of spinning in place.
//
// This is the same rule mirrorStep already states for its reflection axis, and
// for the same reason: a document edit is written against the committed
// geometry.
std::optional<Point> Session::transformCentre() const {
    const std::vector<EntityId> moved = transformClosure(*doc_, selection_.items());
    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
    bool any = false;
    for(EntityId id : moved) {
        const EntityRecord *e = doc_->entities().find(id);
        if(e == nullptr || e->kind != EntityKind::Point) continue;
        const std::optional<Point> p = Point{e->seeds[0], e->seeds[1]};
        if(!any) {
            minX = maxX = p->x;
            minY = maxY = p->y;
            any = true;
            continue;
        }
        minX = std::min(minX, p->x);
        maxX = std::max(maxX, p->x);
        minY = std::min(minY, p->y);
        maxY = std::max(maxY, p->y);
    }
    if(!any) return std::nullopt;
    return Point{0.5 * (minX + maxX), 0.5 * (minY + maxY)};
}

namespace {

// The worst distance between where a step put a point and where solving leaves
// it, over the moved set.
//
// What makes the axis question answerable by looking rather than by reading. A
// retargeted rotation lands on the rotated pose and stays there, so this is
// zero; a kept-axes rotation of an axis-constrained cluster is pulled back
// towards the document frame, and this is how far. The preview shows both
// numbers and the user picks.
double poseDrift(const Document &doc, const std::vector<EntityId> &moved) {
    Topology topology(doc);
    SolveOptions options;
    options.diagnoseFailures = false;
    double worst = 0.0;
    std::unordered_set<ComponentId> done;
    for(EntityId id : moved) {
        const ComponentId component = topology.componentOf(id);
        if(component == NO_COMPONENT || !done.insert(component).second) continue;
        SolveContext context = SolveContext::forComponent(doc, topology, id);
        if(context.empty()) continue;
        const std::vector<SeedSpan> before = context.params();
        if(!solve(doc, context, options).ok()) continue;
        for(const SeedSpan &after : context.params()) {
            const EntityRecord *e = doc.entities().find(after.entity);
            if(e == nullptr || e->kind != EntityKind::Point) continue;
            for(const SeedSpan &was : before) {
                if(was.entity != after.entity) continue;
                worst = std::max(worst, std::hypot(after.seeds[0] - was.seeds[0],
                                                   after.seeds[1] - was.seeds[1]));
            }
        }
    }
    return worst;
}

}  // namespace

// The preview both answers share.
//
// A document copy, which is the exception the no-copy rule already carries: the
// rule is about the interaction path, and a transform is a typed operation. It
// has to be a copy rather than a forked context because a transform emits
// records — a cluster frame is three of them — and a speculative context forks
// parameters, not a document.
Session::TransformPreview Session::previewRotate(double degrees, AxisAnswer axes) const {
    RotateOptions options;
    options.angle = degrees * 3.14159265358979323846 / 180.0;
    options.axes = axes;
    const std::optional<Point> centre = transformCentre();
    if(!centre) {
        TransformPreview p;
        p.error = TransformError::NothingToMove;
        return p;
    }
    options.centre = *centre;

    TransformPreview preview;
    const TransformStep step = rotateStep(*doc_, selection_.items(), options);
    preview.error = step.error;
    preview.moved = step.moved;
    const std::vector<EntityId> moved = transformClosure(*doc_, selection_.items());
    preview.axisConstraints = axisReferencedIn(*doc_, moved).size();
    if(!step.ok()) return preview;

    Document probe = *doc_;
    for(const Command &c : step.commands) probe.apply(c);
    preview.residual = poseDrift(probe, moved);

    Topology topology(probe);
    SolveOptions solveOptions;
    solveOptions.diagnoseFailures = false;
    SolveContext whole = SolveContext::forWholeDocument(probe);
    const SolveOutcome outcome = solve(probe, whole, solveOptions);
    preview.status = outcome.status;
    preview.dof = outcome.dof;
    return preview;
}

Session::TransformPreview Session::previewScale(double factor, ValueAnswer values) const {
    ScaleOptions options;
    options.factor = factor;
    options.values = values;
    const std::optional<Point> centre = transformCentre();
    if(!centre) {
        TransformPreview p;
        p.error = TransformError::NothingToMove;
        return p;
    }
    options.centre = *centre;

    TransformPreview preview;
    const TransformStep step = scaleStep(*doc_, selection_.items(), options);
    preview.error = step.error;
    preview.moved = step.moved;
    preview.straddling = step.straddling;
    const std::vector<EntityId> moved = transformClosure(*doc_, selection_.items());
    preview.absoluteDimensions = absoluteValuedIn(*doc_, moved).size();
    if(!step.ok()) return preview;

    Document probe = *doc_;
    for(const Command &c : step.commands) probe.apply(c);
    preview.residual = poseDrift(probe, moved);

    SolveOptions solveOptions;
    solveOptions.diagnoseFailures = false;
    SolveContext whole = SolveContext::forWholeDocument(probe);
    const SolveOutcome outcome = solve(probe, whole, solveOptions);
    preview.status = outcome.status;
    preview.dof = outcome.dof;
    return preview;
}

// Applies a transform step and reports what it did beyond moving things.
//
// One place, because rotate, scale and translate differ only in the step they
// hand over and every one of them owes the user the same report.
bool Session::applyTransform(const TransformStep &step, const char *label) {
    presentation_.structure.clear();
    presentation_.structure.transformError = step.error;
    if(!step.ok()) return false;
    if(step.commands.empty()) return false;

    std::vector<Command> commands = step.commands;
    if(journal_->applyStep(*doc_, label, std::move(commands)) != CommandError::None) {
        return false;
    }
    presentation_.structure.moved = step.moved;
    presentation_.structure.retargeted = step.retargeted;
    presentation_.structure.rescaled = step.rescaled;
    presentation_.structure.straddling = step.straddling;
    presentation_.structure.frame = step.frame;

    // A cluster frame joins the selection it was made for.
    //
    // Leaving it out is the quiet kind of wrong. The frame is now what the
    // cluster's horizontal means, so a duplicate that did not carry it would
    // find the axis relations straddling its boundary, drop all four, and hand
    // back a copy that is no longer square — correctly, by the copy rules, and
    // baffling to anyone watching. Selecting it says what happened and makes the
    // next operation act on the whole of what the user now has.
    if(step.frame.valid()) {
        std::vector<EntityId> selected = selection_.items();
        auto add = [&](EntityId id) {
            if(id.valid() && std::find(selected.begin(), selected.end(), id) == selected.end()) {
                selected.push_back(id);
            }
        };
        add(step.frame);
        if(const EntityRecord *frame = doc_->entities().find(step.frame)) {
            add(frame->points[0]);
            add(frame->points[1]);
        }
        selection_.set(std::move(selected));
    }
    refresh();
    return true;
}

bool Session::rotateSelection(double degrees, AxisAnswer axes) {
    recordAction("transform.rotate",
                 {{"degrees", degrees},
                  {"retarget", axes == AxisAnswer::RetargetToClusterFrame ? 1.0 : 0.0}});
    const std::optional<Point> centre = transformCentre();
    if(!centre) {
        presentation_.structure.clear();
        presentation_.structure.transformError = TransformError::NothingToMove;
        return false;
    }
    RotateOptions options;
    options.centre = *centre;
    options.angle = degrees * 3.14159265358979323846 / 180.0;
    options.axes = axes;
    return applyTransform(rotateStep(*doc_, selection_.items(), options), "rotate");
}

bool Session::scaleSelection(double factor, ValueAnswer values) {
    recordAction("transform.scale",
                 {{"factor", factor},
                  {"scale-values", values == ValueAnswer::ScaleTheValues ? 1.0 : 0.0}});
    const std::optional<Point> centre = transformCentre();
    if(!centre) {
        presentation_.structure.clear();
        presentation_.structure.transformError = TransformError::NothingToMove;
        return false;
    }
    ScaleOptions options;
    options.centre = *centre;
    options.factor = factor;
    options.values = values;
    return applyTransform(scaleStep(*doc_, selection_.items(), options), "scale");
}

bool Session::scaleSelectionNonUniform(double factorX, double factorY) {
    // No recording, deliberately, and this is the one action that must not.
    //
    // Its registry row is never applicable, so invokeAction refuses before it
    // reaches here and no surface can produce this step. A step recorded anyway
    // would replay into that refusal and re-record as nothing — record → replay
    // → record broken, silently, with the first file the only one that ever
    // mentioned it. Every other action records the request rather than the
    // outcome because a refused request replays and is refused again, which is
    // the same session; this one cannot even be requested.
    //
    // Refused in the model, and the refusal is the whole of what happens. Said
    // out loud rather than by the action being absent: a user who cannot find
    // non-uniform scale concludes the tool forgot it, and one who is told it does
    // not commute with their constraints has learned what their document is.
    return applyTransform(nonUniformScaleStep(*doc_, selection_.items(), factorX, factorY),
                          "scale");
}

bool Session::duplicateSelection(double dx, double dy) {
    recordAction("edit.duplicate", {{"dx", dx}, {"dy", dy}});
    presentation_.structure.clear();
    const CopyStep copy = copyStep(*doc_, selection_.items(), dx, dy);
    if(copy.empty()) return false;

    std::vector<Command> commands = copy.commands;
    if(journal_->applyStep(*doc_, "duplicate", std::move(commands)) != CommandError::None) {
        return false;
    }
    presentation_.structure.copied = copy.entities.size();
    presentation_.structure.droppedRelations = copy.droppedConstraints;
    presentation_.structure.droppedRegions = copy.droppedRegions;
    presentation_.structure.droppedTags = copy.droppedTags;

    // The copy becomes the selection, so a second duplicate offsets from it
    // rather than laying a third shape over the second. That is what makes this
    // the seed of arrays: repetition is repetition of the gesture.
    select(copy.copiedEntities());
    refresh();
    return true;
}

bool Session::applyCompound(const CompoundStep &step, const char *label) {
    presentation_.structure.clear();
    presentation_.structure.compoundError = step.error;
    if(!step.ok() || step.commands.empty()) return false;
    std::vector<Command> commands = step.commands;
    if(journal_->applyStep(*doc_, label, std::move(commands)) != CommandError::None) {
        return false;
    }
    presentation_.structure.copied = step.entities.size();
    refresh();
    return true;
}

bool Session::distributeSelection() {
    recordAction("relation.distribute");
    return applyCompound(distributeStep(*doc_, selection_.items()), "distribute");
}

bool Session::mirrorSelection() {
    recordAction("relation.mirror");
    const EntityId axis = mirrorAxisIn(*doc_, selection_.items());
    if(!axis.valid()) {
        presentation_.structure.clear();
        presentation_.structure.compoundError = CompoundError::NoAxis;
        return false;
    }
    return applyCompound(mirrorStep(*doc_, selection_.items(), axis), "mirror");
}

std::vector<TagId> Session::selectedTags() const {
    std::vector<TagId> out;
    for(const TagRecord &t : doc_->tags().records()) {
        // Every entity, not any: a tag is named by naming what it is over, the
        // same rule a region follows, so brushing one edge of a rectangle does
        // not put its tag under an action the user did not aim at.
        if(t.entities.empty()) continue;
        const bool whole = std::all_of(t.entities.begin(), t.entities.end(), [&](EntityId e) {
            return selection_.contains(e);
        });
        if(whole) out.push_back(t.id);
    }
    return out;
}

bool Session::dissolveTags() {
    recordAction("tag.dissolve");
    const std::vector<TagId> tags = selectedTags();
    if(tags.empty()) return false;
    std::vector<Command> step;
    step.reserve(tags.size());
    for(TagId id : tags) step.push_back(RemoveRecord<TagRecord>{id});
    // Nothing else goes. A tag owns none of what it names, which is why giving
    // one up deliberately and losing one to an edit come to the same thing.
    if(journal_->applyStep(*doc_, "dissolve tag", std::move(step)) != CommandError::None) {
        return false;
    }
    refresh();
    return true;
}

std::vector<Session::RectanglePanel> Session::rectanglePanels() const {
    std::vector<RectanglePanel> out;
    const Pose current = pose();
    for(TagId id : selectedTags()) {
        const std::optional<RectangleFrame> frame = rectangleFrame(*doc_, id);
        if(!frame) continue;
        if(const std::optional<RectangleSize> size = rectangleSize(*doc_, current, *frame)) {
            out.push_back(RectanglePanel{id, *size});
        }
    }
    return out;
}

// Sets one side of a tagged rectangle.
//
// Two paths that end in the same place. A dimensioned side is a value edit: the
// slot is rewritten and the solver moves the geometry to hold it, which is one
// of the two things PRINCIPLES allows to move a drawing. An undimensioned side
// has no slot to drive, so the number the user typed becomes the dimension —
// after which the handle drives it, which is what makes the panel and the handle
// the same affordance from two directions rather than two features.
bool Session::setRectangleSide(TagId tag, bool width, double value) {
    presentation_.structure.clear();
    if(!(value > 0.0)) return false;
    const std::optional<RectangleFrame> frame = rectangleFrame(*doc_, tag);
    if(!frame) return false;
    const EntityId edge = width ? frame->widthEdge : frame->heightEdge;
    const EntityRecord *e = doc_->entities().find(edge);
    if(e == nullptr) return false;

    const ConstraintId existing = edgeDimension(*doc_, edge);
    std::vector<Command> step;
    if(existing.valid()) {
        ConstraintRecord next = *doc_->constraints().find(existing);
        next.value = Slot(value);
        step.push_back(SetRecord<ConstraintRecord>{next});
    } else {
        ConstraintRecord dimension;
        dimension.id = ConstraintId(doc_->constraints().allocator().next());
        dimension.kind = ConstraintKind::PointPointDistance;
        dimension.operands[0] = e->points[0];
        dimension.operands[1] = e->points[1];
        dimension.value = Slot(value);

        // Checked before it is committed, exactly as every other imposition path
        // is. A rectangle whose width edge is already determined by other
        // relations would otherwise take an unchecked driving constraint, the
        // component would go inconsistent, refresh() would drop it, and the whole
        // rectangle would freeze at its committed seeds with no verdict shown and
        // no downgrade offered.
        //
        // Downgraded automatically rather than refused, matching the numeric
        // path: the value is what the user was supplying, and losing it would be
        // worse than not driving with it.
        const CandidateCheck check = checkCandidate(*doc_, topology_, dimension);
        presentation_.impositionVerdict = check.verdict;
        presentation_.conflicting = check.conflicting;
        presentation_.conflictAttributed = check.attributed;
        dimension.driving = check.committable();
        step.push_back(AddRecord<ConstraintRecord>{dimension});
    }
    if(journal_->applyStep(*doc_, width ? "rectangle width" : "rectangle height",
                           std::move(step)) != CommandError::None) {
        return false;
    }
    doc_->noteUsage(ConstraintKind::PointPointDistance);
    refresh();
    return true;
}

bool Session::setRectangleWidth(TagId tag, double width) {
    recordAction("tag.set-width", {{"tag", static_cast<double>(tag.value())},
                                   {"value", width}});
    return setRectangleSide(tag, true, width);
}

bool Session::setRectangleHeight(TagId tag, double height) {
    recordAction("tag.set-height", {{"tag", static_cast<double>(tag.value())},
                                    {"value", height}});
    return setRectangleSide(tag, false, height);
}

Bake Session::bake() const { return bakeForExport(*doc_, pose()); }

}  // namespace paroculus
