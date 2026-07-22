#include "interact/registry.h"

#include <algorithm>
#include <array>

#include "core/composition.h"
#include "interact/impose.h"
#include "interact/session.h"

namespace paroculus {
namespace {

constexpr ActionParameter INDEX_PARAMETER[] = {{"index", true}};

// What an imposition takes. Both optional, and each stands for a question the
// surface may or may not have had to ask.
//
//   assignment  which reading of the selection, for the kinds that read one
//               operand against the other. Absent means the canonical one.
//   value       what to declare instead of what is measured. Absent is the
//               movement-free path — the whole point of imposition — and
//               present is a value edit, which is one of the two things that is
//               allowed to move geometry.
constexpr ActionParameter IMPOSE_PARAMETERS[] = {{"assignment", false}, {"value", false}};

bool always(const ActionContext &, const Action &) { return true; }

// Delete applies to anything selected, geometry or relation. The signature is a
// projection of the geometry alone, so reading it here dimmed Delete for exactly
// the selection the conflict walk produces — a set of relations and nothing else
// — and made the gesture the walk exists to enable a silent no-op.
bool haveDeletable(const ActionContext &c, const Action &) {
    return !c.signature.empty() || c.selectedConstraints > 0;
}
bool canUndo(const ActionContext &c, const Action &) { return c.canUndo; }
bool canRedo(const ActionContext &c, const Action &) { return c.canRedo; }
bool haveOffers(const ActionContext &c, const Action &) { return c.offers > 0; }
bool haveInferred(const ActionContext &c, const Action &) { return c.inferred > 0; }
bool haveClosedLoop(const ActionContext &c, const Action &) { return c.closedLoop; }
bool haveHealableLoop(const ActionContext &c, const Action &) { return c.healableLoop; }
bool haveSelectedConstraints(const ActionContext &c, const Action &) {
    return c.selectedConstraints > 0;
}
bool haveConflicts(const ActionContext &c, const Action &) { return !c.conflicting.empty(); }

// The applicability predicate every generated row shares, and the reason the
// action surface cannot offer what the model would refuse: it asks the same
// question validation asks, through the same operand table.
bool canImpose(const ActionContext &c, const Action &action) {
    if(c.document == nullptr) return false;
    return !assignmentsFor(*c.document, action.constraintKind, c.selection).empty();
}

template <ToolKind Kind>
bool activateTool(Session &session, const Action &, const ActionArguments &) {
    session.setTool(Kind);
    return true;
}

bool doDelete(Session &session, const Action &, const ActionArguments &) {
    session.handle(Key::Delete);
    return true;
}

bool doUndo(Session &session, const Action &, const ActionArguments &) {
    session.handle(Key::Undo);
    return true;
}

bool doRedo(Session &session, const Action &, const ActionArguments &) {
    session.handle(Key::Redo);
    return true;
}

// An index that is not a whole number is a caller error, not a rounding
// opportunity: confirming offer 1.5 is not a thing anyone meant.
std::optional<size_t> indexOf(const ActionArguments &arguments) {
    const std::optional<double> raw = arguments.value("index");
    if(!raw) return std::nullopt;
    if(*raw < 0.0 || *raw != static_cast<double>(static_cast<size_t>(*raw))) {
        return std::nullopt;
    }
    return static_cast<size_t>(*raw);
}

bool doConfirm(Session &session, const Action &, const ActionArguments &arguments) {
    const std::optional<size_t> index = indexOf(arguments);
    if(!index) return false;
    session.confirmOffer(*index);
    return true;
}

bool doDecline(Session &session, const Action &, const ActionArguments &arguments) {
    const std::optional<size_t> index = indexOf(arguments);
    if(!index) return false;
    session.declineInference(*index);
    return true;
}

// The one entrance every imposition takes, whatever surface asked and whichever
// of the three strengths it asked for. The strength rides on the action rather
// than on the arguments because it is what the action *is*: measure-once,
// impose and reference are one semantic at three settings, and a surface offers
// them as one relation with a choice.
bool doImpose(Session &session, const Action &action, const ActionArguments &arguments) {
    size_t assignment = 0;
    if(const std::optional<double> raw = arguments.value("assignment")) {
        if(*raw < 0.0 || *raw != static_cast<double>(static_cast<size_t>(*raw))) return false;
        assignment = static_cast<size_t>(*raw);
    }
    return session.impose(action.constraintKind, action.strength, assignment,
                          arguments.value("value"));
}

bool doMakeSolid(Session &session, const Action &, const ActionArguments &) {
    return session.makeSolid();
}

bool doHealAndFill(Session &session, const Action &, const ActionArguments &) {
    return session.healAndFill();
}

bool doToggleDriving(Session &session, const Action &, const ActionArguments &) {
    return session.toggleDriving();
}

bool doWalkConflicts(Session &session, const Action &, const ActionArguments &) {
    return session.selectConflicting();
}

// Composition. Each of these is one line because Session owns the semantics;
// what the registry contributes is that they are reachable identically from the
// palette, the keyboard, a script and a test.
bool doNewLayer(Session &session, const Action &, const ActionArguments &) {
    return session.newLayer();
}

// Named, else the newest layer — which is the only reading under which "move to
// layer" with nothing named does anything at all. Defaulting to the selection's
// own layer, as the visibility and lock actions do, would make this action a
// no-op every time it was invoked without an argument.
bool doAssignLayer(Session &session, const Action &, const ActionArguments &a) {
    const std::optional<double> named = a.value("layer");
    return session.assignLayer(named && *named > 0.0
                                   ? LayerId(static_cast<uint32_t>(*named))
                                   : session.topLayer());
}

template <bool Visible>
bool doLayerVisible(Session &session, const Action &, const ActionArguments &a) {
    return session.setLayerVisible(session.targetLayer(a.value("layer")), Visible);
}

template <bool Locked>
bool doLayerLocked(Session &session, const Action &, const ActionArguments &a) {
    return session.setLayerLocked(session.targetLayer(a.value("layer")), Locked);
}

template <int Delta>
bool doMoveLayer(Session &session, const Action &, const ActionArguments &a) {
    return session.moveLayer(session.targetLayer(a.value("layer")), Delta);
}

// `order` is subtract's alone: 0 cuts the upper operand out of the lower one,
// which is what the picture looks like, and 1 says the other reading. Union and
// intersect ignore it because for them there is no question — the same operands
// in the other order are the same area.
template <CompositeOp Op>
bool doCompose(Session &session, const Action &, const ActionArguments &a) {
    const std::optional<double> order = a.value("order");
    return session.composeRegions(Op, Op == CompositeOp::Subtract && order && *order != 0.0);
}

bool doLift(Session &session, const Action &, const ActionArguments &) {
    return session.liftComposite();
}

bool doPunch(Session &session, const Action &, const ActionArguments &) {
    return session.togglePunch();
}

template <int Delta>
bool doMoveRegion(Session &session, const Action &, const ActionArguments &) {
    return session.moveRegion(Delta);
}

bool doGroup(Session &session, const Action &, const ActionArguments &) {
    return session.groupSelection();
}

bool doDissolveGroup(Session &session, const Action &, const ActionArguments &) {
    return session.dissolveGroups();
}

// The bake produces a value and changes nothing, so invoking it is not an edit
// and journals nothing. Stage 8 hands the value to a writer.
bool doBake(Session &session, const Action &, const ActionArguments &) {
    session.bake();
    return true;
}

constexpr ActionParameter LAYER_PARAMETERS[] = {{"layer", false}};

// Which way round a subtract reads its operands. Absent is occlusion order,
// lowest first; 1 is the other reading.
constexpr ActionParameter REGION_ORDER_PARAMETERS[] = {{"order", false}};

// A layer action applies when there is a layer to act on and acting would
// change something. An action that says it applies and then declines is a
// surface offering what the model refuses, which is the one thing the registry
// exists to make impossible.
const LayerRecord *contextLayer(const ActionContext &c) {
    if(c.document == nullptr || c.layer == 0) return nullptr;
    return c.document->layers().find(LayerId(c.layer));
}

template <bool Visible>
bool canSetVisible(const ActionContext &c, const Action &) {
    const LayerRecord *l = contextLayer(c);
    return l != nullptr && l->visible != Visible;
}

template <bool Locked>
bool canSetLocked(const ActionContext &c, const Action &) {
    const LayerRecord *l = contextLayer(c);
    return l != nullptr && l->locked != Locked;
}

bool haveLayer(const ActionContext &c, const Action &) { return contextLayer(c) != nullptr; }

bool canAssignLayer(const ActionContext &c, const Action &) {
    return c.document != nullptr && !c.selection.empty() && !c.document->layers().empty();
}
bool haveRegions(const ActionContext &c, const Action &) { return c.selectedRegions > 0; }
bool haveTwoRegions(const ActionContext &c, const Action &) { return c.selectedRegions > 1; }
bool haveComposite(const ActionContext &c, const Action &) { return c.selectedComposites > 0; }
bool haveGroupable(const ActionContext &c, const Action &) { return c.selection.size() > 1; }
bool haveGroup(const ActionContext &c, const Action &) { return c.groupedSelection; }

// ---------------------------------------------------------------------------
// Structure operations
// ---------------------------------------------------------------------------

// Degrees for a rotation, and whether to retarget the axis constraints. Both
// optional: an invocation with neither is a zero-degree rotation keeping the
// document axes, which is a no-op rather than an error, and a surface that
// prompts for the angle is supplying it either way.
constexpr ActionParameter ROTATE_PARAMETERS[] = {{"degrees", true}, {"retarget", false}};
// The factor, and which answer the absolute dimensions get.
constexpr ActionParameter SCALE_PARAMETERS[] = {{"factor", true}, {"scale-values", false}};
constexpr ActionParameter NON_UNIFORM_PARAMETERS[] = {{"x", true}, {"y", true}};
constexpr ActionParameter OFFSET_PARAMETERS[] = {{"dx", false}, {"dy", false}};
// Which rectangle, and what to set the side to. The tag is named rather than
// inferred because a selection can reach two of them, and picking one would be
// the silent choice the surface exists to prevent.
constexpr ActionParameter SIDE_PARAMETERS[] = {{"tag", true}, {"value", true}};

// Rotate and scale refuse whole on any lock in the transform closure — seeding
// a locked parameter is a move, and a transform does not move the lock — so the
// predicate mirrors that gate: transformable geometry with nothing in it locked.
// An action that claimed to apply and then refused is exactly the
// applicable-and-refusing this table exists to make impossible.
bool haveTransformable(const ActionContext &c, const Action &) {
    return c.transformable > 0 && !c.transformLocked;
}

// Duplicate copies to new geometry and never moves the lock, so it stays
// applicable over a locked selection — the copy is different geometry the lock
// says nothing about. Mirror is the same bijection over new geometry and reads
// its own predicate; distribute imposes relations and refuses on no lock; so
// both are left as they are, and only rotate and scale gain the lock guard.
bool haveDuplicable(const ActionContext &c, const Action &) { return c.transformable > 0; }

// Never applicable, and present anyway.
//
// Non-uniform scale does not commute with almost any relation in the catalogue,
// so the model does not have it. The permanent surfaces dim what does not apply
// rather than hiding it, and this is the one row that is dimmed always: a user
// who looks for non-uniform scale finds it, greyed, saying where it does live —
// the export bake — instead of concluding the tool forgot. An action that
// claimed to apply and then refused would break the property that an applicable
// action runs, which is what makes the whole table trustworthy.
bool never(const ActionContext &, const Action &) { return false; }

bool doRotate(Session &session, const Action &, const ActionArguments &args) {
    const std::optional<double> degrees = args.value("degrees");
    if(!degrees) return false;
    // Absent is keep-the-document-axes, which is the answer that changes no
    // relation. A default that retargeted would edit constraints the caller
    // never mentioned.
    const bool retarget = args.value("retarget").value_or(0.0) != 0.0;
    return session.rotateSelection(*degrees, retarget ? AxisAnswer::RetargetToClusterFrame
                                                      : AxisAnswer::KeepDocumentAxes);
}

bool doScale(Session &session, const Action &, const ActionArguments &args) {
    const std::optional<double> factor = args.value("factor");
    if(!factor) return false;
    // Absent is let-them-resist, for the same reason: the answer that rewrites
    // nothing is the one a caller who said nothing gets.
    const bool scaleValues = args.value("scale-values").value_or(0.0) != 0.0;
    return session.scaleSelection(*factor, scaleValues ? ValueAnswer::ScaleTheValues
                                                       : ValueAnswer::LetThemResist);
}

bool doScaleNonUniform(Session &session, const Action &, const ActionArguments &args) {
    const std::optional<double> x = args.value("x");
    const std::optional<double> y = args.value("y");
    if(!x || !y) return false;
    return session.scaleSelectionNonUniform(*x, *y);
}

bool doDuplicate(Session &session, const Action &, const ActionArguments &args) {
    return session.duplicateSelection(args.value("dx").value_or(0.0),
                                      args.value("dy").value_or(0.0));
}

bool haveDistributable(const ActionContext &c, const Action &) { return c.selectedPoints > 2; }
bool haveMirrorable(const ActionContext &c, const Action &) { return c.mirrorable; }

bool doDistribute(Session &session, const Action &, const ActionArguments &) {
    return session.distributeSelection();
}

bool doMirror(Session &session, const Action &, const ActionArguments &) {
    return session.mirrorSelection();
}

bool haveTags(const ActionContext &c, const Action &) { return c.selectedTags > 0; }
bool haveRectangleTag(const ActionContext &c, const Action &) { return c.rectangleTags > 0; }

bool doDissolveTags(Session &session, const Action &, const ActionArguments &) {
    return session.dissolveTags();
}

template <bool Width>
bool doSetSide(Session &session, const Action &, const ActionArguments &args) {
    const std::optional<double> tag = args.value("tag");
    const std::optional<double> value = args.value("value");
    if(!tag || !value) return false;
    const TagId id(static_cast<uint32_t>(*tag));
    return Width ? session.setRectangleWidth(id, *value)
                 : session.setRectangleHeight(id, *value);
}

// ---------------------------------------------------------------------------
// Styles and parameters
// ---------------------------------------------------------------------------

constexpr ActionParameter COLOR_PARAMETERS[] = {{"color", true}};
constexpr ActionParameter FLAG_PARAMETERS[] = {{"flag", true}};
// A style width or opacity is a constant the toolbar scrubs; the expression
// resistance lives in applicability, not here, so the control dims rather than
// flattening a value authored elsewhere.
constexpr ActionParameter STYLE_VALUE_PARAMETERS[] = {{"value", true}};
constexpr ActionParameter STYLE_CREATE_PARAMETERS[] = {{"name", false, true}};
constexpr ActionParameter STYLE_APPLY_PARAMETERS[] = {{"style", false}};
constexpr ActionParameter STYLE_RENAME_PARAMETERS[] = {{"style", false}, {"name", true, true}};
// A parameter value is a constant or an expression, the two channels a slot
// carries. name is text; id names which parameter, and defaults to the newest.
constexpr ActionParameter PARAM_CREATE_PARAMETERS[] = {
    {"name", true, true}, {"value", false}, {"expression", false, true}};
constexpr ActionParameter PARAM_SET_PARAMETERS[] = {
    {"id", false}, {"value", false}, {"expression", false, true}};
constexpr ActionParameter PARAM_RENAME_PARAMETERS[] = {{"id", false}, {"name", true, true}};
constexpr ActionParameter PARAM_ID_PARAMETERS[] = {{"id", false}};

bool doSetStyleStroke(Session &session, const Action &, const ActionArguments &a) {
    const std::optional<double> color = a.value("color");
    if(!color) return false;
    return session.setStyleStroke(static_cast<uint32_t>(*color));
}
bool doSetStyleFill(Session &session, const Action &, const ActionArguments &a) {
    const std::optional<double> color = a.value("color");
    if(!color) return false;
    return session.setStyleFill(static_cast<uint32_t>(*color));
}
bool doSetStyleWidth(Session &session, const Action &, const ActionArguments &a) {
    const std::optional<double> v = a.value("value");
    if(!v) return false;
    return session.setStyleStrokeWidth(*v);
}
bool doSetStyleOpacity(Session &session, const Action &, const ActionArguments &a) {
    const std::optional<double> v = a.value("value");
    if(!v) return false;
    return session.setStyleOpacity(*v);
}
bool doSetStyleFilled(Session &session, const Action &, const ActionArguments &a) {
    const std::optional<double> flag = a.value("flag");
    if(!flag) return false;
    return session.setStyleFilled(*flag != 0.0);
}
bool doCreateStyle(Session &session, const Action &, const ActionArguments &a) {
    return session.createStyle(std::string(a.text("name").value_or("")));
}
bool doApplyStyle(Session &session, const Action &, const ActionArguments &a) {
    return session.applyStyle(session.targetStyle(a.value("style")));
}
bool doRenameStyle(Session &session, const Action &, const ActionArguments &a) {
    const std::optional<std::string_view> name = a.text("name");
    if(!name) return false;
    return session.renameStyle(session.targetStyle(a.value("style")), std::string(*name));
}

bool doCreateParameter(Session &session, const Action &, const ActionArguments &a) {
    const std::optional<std::string_view> name = a.text("name");
    if(!name) return false;
    if(const std::optional<std::string_view> expr = a.text("expression")) {
        return session.createParameterExpression(std::string(*name), *expr);
    }
    return session.createParameterConstant(std::string(*name), a.value("value").value_or(0.0));
}
bool doSetParameter(Session &session, const Action &, const ActionArguments &a) {
    const ParameterId id = session.targetParameter(a.value("id"));
    if(const std::optional<std::string_view> expr = a.text("expression")) {
        return session.setParameterExpression(id, *expr);
    }
    const std::optional<double> v = a.value("value");
    if(!v) return false;
    return session.setParameterConstant(id, *v);
}
bool doRenameParameter(Session &session, const Action &, const ActionArguments &a) {
    const std::optional<std::string_view> name = a.text("name");
    if(!name) return false;
    return session.renameParameter(session.targetParameter(a.value("id")), std::string(*name));
}
bool doDeleteParameter(Session &session, const Action &, const ActionArguments &a) {
    return session.deleteParameter(session.targetParameter(a.value("id")));
}

bool haveStyleableEntities(const ActionContext &c, const Action &) {
    return c.styleableEntities > 0;
}
bool haveStyleable(const ActionContext &c, const Action &) {
    return c.styleableEntities + c.styleableRegions > 0;
}
bool canStyleWidth(const ActionContext &c, const Action &) {
    return c.styleableEntities + c.styleableRegions > 0 && !c.strokeWidthExpr;
}
bool canStyleOpacity(const ActionContext &c, const Action &) {
    return c.styleableEntities + c.styleableRegions > 0 && !c.opacityExpr;
}
bool haveStyleableRegions(const ActionContext &c, const Action &) {
    return c.styleableRegions > 0;
}
bool canApplyStyle(const ActionContext &c, const Action &) {
    return c.namedStyleCount > 0 && c.styleableEntities + c.styleableRegions > 0;
}
bool haveStyle(const ActionContext &c, const Action &) { return c.namedStyleCount > 0; }
bool haveParameter(const ActionContext &c, const Action &) { return c.parameterCount > 0; }

// ---------------------------------------------------------------------------
// Layer, relation and snap vocabulary
// ---------------------------------------------------------------------------

constexpr ActionParameter LAYER_RENAME_PARAMETERS[] = {{"layer", false}, {"name", true, true}};
constexpr ActionParameter CONSTRAINT_VALUE_PARAMETERS[] = {{"constraint", false}, {"value", true}};
constexpr ActionParameter CONSTRAINT_PARAMETERS[] = {{"constraint", false}};
// A retarget names an existing frame, or asks for the document frame; absent
// both is the new-cluster-frame default, the answer that matches rotate.
constexpr ActionParameter RETARGET_PARAMETERS[] = {{"frame", false}, {"document", false}};
constexpr ActionParameter GRID_PARAMETERS[] = {{"step", true}, {"enabled", true}};
constexpr ActionParameter ATTRACT_PARAMETERS[] = {{"flag", true}};

bool doRenameLayer(Session &session, const Action &, const ActionArguments &a) {
    const std::optional<std::string_view> name = a.text("name");
    if(!name) return false;
    return session.renameLayer(session.targetLayer(a.value("layer")), std::string(*name));
}
bool doActivateLayer(Session &session, const Action &, const ActionArguments &a) {
    return session.activateLayer(session.targetLayer(a.value("layer")));
}
bool doSetRelationValue(Session &session, const Action &, const ActionArguments &a) {
    const std::optional<double> value = a.value("value");
    if(!value) return false;
    return session.setRelationValue(session.targetConstraint(a.value("constraint")), *value);
}
bool doFlipAlternative(Session &session, const Action &, const ActionArguments &a) {
    return session.flipAlternative(session.targetConstraint(a.value("constraint")));
}
bool doRetargetAxes(Session &session, const Action &, const ActionArguments &a) {
    if(a.value("document").value_or(0.0) != 0.0) {
        return session.retargetAxes(RetargetTarget::DocumentFrame, EntityId());
    }
    if(const std::optional<double> frame = a.value("frame")) {
        if(*frame > 0.0) {
            return session.retargetAxes(RetargetTarget::ExistingFrame,
                                        EntityId(static_cast<uint32_t>(*frame)));
        }
    }
    return session.retargetAxes(RetargetTarget::NewClusterFrame, EntityId());
}
bool doRetargetToDocument(Session &session, const Action &, const ActionArguments &) {
    return session.retargetAxes(RetargetTarget::DocumentFrame, EntityId());
}
bool doSetGrid(Session &session, const Action &, const ActionArguments &a) {
    const std::optional<double> step = a.value("step");
    const std::optional<double> enabled = a.value("enabled");
    if(!step || !enabled) return false;
    return session.setSnapGrid(*step, *enabled != 0.0);
}
bool doSetConstructionAttract(Session &session, const Action &, const ActionArguments &a) {
    const std::optional<double> flag = a.value("flag");
    if(!flag) return false;
    return session.setSnapConstructionAttract(*flag != 0.0);
}

// A real layer must exist and be unlocked to activate; the base layer, which has
// no record, is always there and always activatable. The gate mirrors the
// session's — an applicable activate is one the session performs.
bool canActivateLayer(const ActionContext &c, const Action &) {
    if(c.layer == 0) return true;  // the base layer
    const LayerRecord *l = contextLayer(c);
    return l != nullptr && !l->locked;
}
bool canSetValue(const ActionContext &c, const Action &) { return c.selectedConstraintValued; }
bool canFlipAlternative(const ActionContext &c, const Action &) {
    return c.selectedConstraintHasAlternative;
}
// The new-cluster-frame default is what a menu or the palette invokes — and the
// only target U1 surfaces — so the row dims exactly as that rewrite would refuse:
// document-framed axis relations to retarget (axisReferencedIn skips ones already
// on a frame), and nothing in the closure locked or frame-referenced. The
// document-frame and existing-frame targets the action also carries act on
// already-clustered relations too, so when U2's inspector axis section surfaces
// them it must gate on the broader "any axis relation in the moved set" — from
// axisReferences() — rather than this predicate, which is right for the default
// alone. One predicate cannot serve all three because it cannot see the target.
bool canRetargetAxes(const ActionContext &c, const Action &) {
    return c.axisConstraints > 0 && !c.transformLocked;
}
// The second retarget target U2 surfaces: back to the document frame, which
// sheds a cluster reference. It applies exactly when there is a clustered axis
// relation to shed and nothing in the closure locked or frame-referenced — the
// same lock/frame gate the rewrite refuses on — so applicable still equals
// runnable. A predicate of its own rather than a broadened canRetargetAxes,
// because broadening the latter would make the no-arg menu default (a new
// cluster frame) apply to an already-clustered selection and orphan a frame.
bool canRetargetToDocument(const ActionContext &c, const Action &) {
    return c.clusteredAxisConstraints > 0 && !c.transformLocked;
}

// A title a surface can show, derived rather than stored: "point-on-line"
// becomes "Point on line". The taxonomy's name is a serialization token and
// therefore format; a title is presentation and free to change, so deriving one
// keeps a presentation string out of a table that is format.
std::string titleOf(ConstraintKind kind, Strength strength) {
    std::string out(constraintInfo(kind).name);
    for(char &c : out) {
        if(c == '-') c = ' ';
    }
    if(!out.empty() && out[0] >= 'a' && out[0] <= 'z') out[0] = static_cast<char>(out[0] - 32);
    switch(strength) {
        case Strength::Impose:    break;
        case Strength::Reference: out += " (reference)"; break;
        case Strength::Measure:   out += " (once)"; break;
    }
    return out;
}

std::string nameOf(ConstraintKind kind, Strength strength) {
    std::string out = "constrain." + std::string(constraintInfo(kind).name);
    // The imposing form takes the bare name. It is the one a script is most
    // likely to spell by hand and the one the strip invokes, and a suffix on
    // every row would make the common case the noisy one.
    if(strength != Strength::Impose) {
        out += '.';
        out += strengthName(strength);
    }
    return out;
}

// The one-line tooltip for a hand-written action, keyed by its stable name.
//
// A table rather than a derivation because these describe intent a title cannot
// — "Move the selection to a layer" reads better than any transform of
// "layer.assign". Generated rows are composed instead (below), since there the
// title already carries the relation and the strength. Missing is empty, and the
// conformance sweep turns empty into a red test, so a new hand-written action
// cannot land undescribed.
std::string_view descriptionFor(std::string_view name) {
    if(name == "tool.select") return "Return to the selection tool";
    if(name == "tool.line") return "Draw a line";
    if(name == "tool.circle") return "Draw a circle";
    if(name == "tool.arc") return "Draw an arc";
    if(name == "tool.rectangle") return "Draw a rectangle";
    if(name == "edit.delete") return "Delete the selection";
    if(name == "edit.undo") return "Undo the last change";
    if(name == "edit.redo") return "Redo the undone change";
    if(name == "edit.duplicate") return "Duplicate the selection";
    if(name == "inference.confirm") return "Confirm an offered relation";
    if(name == "inference.decline") return "Decline the last declared relation";
    if(name == "region.make-solid") return "Fill the closed outline";
    if(name == "region.heal-and-fill") return "Close the gaps in the outline and fill it";
    if(name == "region.union") return "Combine the selected regions by union";
    if(name == "region.intersect") return "Combine the selected regions by intersection";
    if(name == "region.subtract") return "Cut one selected region out of another";
    if(name == "region.lift") return "Lift a composite back into its operands";
    if(name == "region.punch") return "Punch the region through what is below it";
    if(name == "region.raise") return "Raise the region in the stacking order";
    if(name == "region.lower") return "Lower the region in the stacking order";
    if(name == "relation.toggle-driving") return "Flip the relation between driving and reference";
    if(name == "relation.walk-conflicts") return "Select the conflicting relations";
    if(name == "relation.set-value") return "Set the selected relation's value";
    if(name == "relation.flip-alternative") return "Flip the relation's alternative form";
    if(name == "relation.retarget-axes") return "Retarget the selection's axis relations";
    if(name == "relation.retarget-to-document")
        return "Point the selection's axis relations back at the document frame";
    if(name == "snap.set-grid") return "Set grid snapping and its step";
    if(name == "snap.set-construction-attract") return "Toggle snapping to construction geometry";
    if(name == "relation.distribute") return "Distribute the selected points evenly";
    if(name == "relation.mirror") return "Mirror the selection about a segment";
    if(name == "layer.new") return "Create a new layer";
    if(name == "layer.assign") return "Move the selection to a layer";
    if(name == "layer.hide") return "Hide the layer";
    if(name == "layer.show") return "Show the layer";
    if(name == "layer.lock") return "Lock the layer against edits";
    if(name == "layer.unlock") return "Unlock the layer";
    if(name == "layer.raise") return "Raise the layer in the stacking order";
    if(name == "layer.lower") return "Lower the layer in the stacking order";
    if(name == "layer.rename") return "Rename the layer";
    if(name == "layer.activate") return "Draw new geometry on this layer";
    if(name == "group.create") return "Group the selection to drag together";
    if(name == "group.dissolve") return "Dissolve the selection's groupings";
    if(name == "transform.rotate") return "Rotate the selection about its centre";
    if(name == "transform.scale") return "Scale the selection uniformly about its centre";
    if(name == "transform.scale-non-uniform") return "Non-uniform scale, available at the export bake only";
    if(name == "tag.dissolve") return "Dissolve the selection's tags";
    if(name == "tag.set-width") return "Set the tagged rectangle's width";
    if(name == "tag.set-height") return "Set the tagged rectangle's height";
    if(name == "style.set-stroke") return "Set the selection's stroke colour";
    if(name == "style.set-fill") return "Set the selection's fill colour";
    if(name == "style.set-stroke-width") return "Set the selection's stroke width";
    if(name == "style.set-opacity") return "Set the selection's opacity";
    if(name == "style.set-filled") return "Fill or unfill the selected regions";
    if(name == "style.create") return "Create a named style from the selection";
    if(name == "style.apply") return "Apply a named style to the selection";
    if(name == "style.rename") return "Rename a style";
    if(name == "parameter.create") return "Create a named parameter";
    if(name == "parameter.set") return "Set a parameter's value";
    if(name == "parameter.rename") return "Rename a parameter";
    if(name == "parameter.delete") return "Delete a parameter, freezing what it drove";
    if(name == "export.bake") return "Flatten the visible drawing for export";
    return {};
}

// The tooltip for a generated imposition row, composed from the relation and its
// strength. The title already reads "Length ratio (reference)"; this says what
// invoking it does.
std::string describeImposition(ConstraintKind kind, Strength strength) {
    std::string base(constraintInfo(kind).name);
    for(char &c : base) {
        if(c == '-') c = ' ';
    }
    switch(strength) {
        case Strength::Impose:    return "Impose " + base + " on the selection";
        case Strength::Reference: return "Add " + base + " as a reference measurement";
        case Strength::Measure:   return "Measure " + base + " once, adding no relation";
    }
    return base;
}

// The whole table: the hand-written rows, then one triple per constraint kind.
//
// Generated rather than listed because the taxonomy is the single source. A
// relation added to CONSTRAINT_KINDS reaches the strip, the palette, the
// keyboard, the script format and the conformance sweep without a second list
// being edited — which is the difference between one list with five projections
// and five lists that drift.
struct Catalogue {
    // Reserved to its final size before anything is pushed, so every
    // string_view handed out points at storage that never moves. The table is
    // built once and never touched again.
    std::vector<std::string> storage;
    std::vector<Action> rows;
};

const Catalogue &catalogue() {
    static const Catalogue table = [] {
        Catalogue c;
        c.rows = {
            {"tool.select", "Select", "v", {}, always, activateTool<ToolKind::Select>},
            {"tool.line", "Line", "l", {}, always, activateTool<ToolKind::Line>},
            {"tool.circle", "Circle", "c", {}, always, activateTool<ToolKind::Circle>},
            {"tool.arc", "Arc", "a", {}, always, activateTool<ToolKind::Arc>},
            {"tool.rectangle", "Rectangle", "r", {}, always, activateTool<ToolKind::Rectangle>},
            {"edit.delete", "Delete", "del", {}, haveDeletable, doDelete},
            {"edit.undo", "Undo", "z", {}, canUndo, doUndo},
            {"edit.redo", "Redo", "shift+z", {}, canRedo, doRedo},
            // alt, not a bare digit: a bare digit is a digit. See resolveKey.
            {"inference.confirm", "Confirm relation", "alt+1..9", INDEX_PARAMETER, haveOffers,
             doConfirm},
            {"inference.decline", "Decline relation", "shift+1..9", INDEX_PARAMETER,
             haveInferred, doDecline},
            // The flagship equivalence. Making a closed outline a solid is a
            // region record over the cycle — no geometry copied, no path
            // synthesized, no constraint touched — so its inverse is deleting
            // that record and the round trip is exact by construction.
            {"region.make-solid", "Make solid", "f", {}, haveClosedLoop, doMakeSolid},
            // And the offer for an outline that only looks closed. The epsilon
            // motion is the explicit point of the action rather than something
            // it does quietly, which is why it is a separate action and not a
            // fallback inside the one above.
            {"region.heal-and-fill", "Heal and fill", "shift+f", {}, haveHealableLoop,
             doHealAndFill},
            {"relation.toggle-driving", "Driving / reference", "d", {},
             haveSelectedConstraints, doToggleDriving},
            {"relation.walk-conflicts", "Select conflicting", "shift+w", {}, haveConflicts,
             doWalkConflicts},
            // The inspector's per-row actions on a selected constraint: set the
            // value checked-before-drive, flip an alternative form, retarget the
            // axis relations. set-value and flip act on the first selected
            // constraint when no id is given; the inspector passes one per row.
            {"relation.set-value", "Set value", {}, CONSTRAINT_VALUE_PARAMETERS, canSetValue,
             doSetRelationValue},
            {"relation.flip-alternative", "Flip alternative", {}, CONSTRAINT_PARAMETERS,
             canFlipAlternative, doFlipAlternative},
            {"relation.retarget-axes", "Retarget axes", {}, RETARGET_PARAMETERS, canRetargetAxes,
             doRetargetAxes},
            {"relation.retarget-to-document", "Retarget to document frame", {}, {},
             canRetargetToDocument, doRetargetToDocument},

            // Layers and groups: organization, so none of these touches a
            // constraint and none of them can refuse an edit. Only visibility
            // and lock take keys — the rest live in the palette, where a
            // permanent surface dims what does not apply rather than hiding it.
            {"layer.new", "New layer", {}, {}, always, doNewLayer},
            {"layer.assign", "Move to layer", {}, LAYER_PARAMETERS, canAssignLayer,
             doAssignLayer},
            {"layer.hide", "Hide layer", "h", LAYER_PARAMETERS, canSetVisible<false>,
             doLayerVisible<false>},
            {"layer.show", "Show layer", "shift+h", LAYER_PARAMETERS, canSetVisible<true>,
             doLayerVisible<true>},
            {"layer.lock", "Lock layer", "k", LAYER_PARAMETERS, canSetLocked<true>,
             doLayerLocked<true>},
            {"layer.unlock", "Unlock layer", "shift+k", LAYER_PARAMETERS, canSetLocked<false>,
             doLayerLocked<false>},
            {"layer.raise", "Raise layer", {}, LAYER_PARAMETERS, haveLayer, doMoveLayer<1>},
            {"layer.lower", "Lower layer", {}, LAYER_PARAMETERS, haveLayer, doMoveLayer<-1>},
            {"layer.rename", "Rename layer", {}, LAYER_RENAME_PARAMETERS, haveLayer,
             doRenameLayer},
            // The active layer is recorded session state, not a document edit, so
            // this journals nothing while tools stamp it on what they emit.
            {"layer.activate", "Activate layer", {}, LAYER_PARAMETERS, canActivateLayer,
             doActivateLayer},
            {"group.create", "Group", "g", {}, haveGroupable, doGroup},
            {"group.dissolve", "Ungroup", "shift+g", {}, haveGroup, doDissolveGroup},

            // Booleans as composition. The operands survive every one of these,
            // which is what makes lift a real inverse rather than an undo.
            {"region.union", "Union", {}, {}, haveTwoRegions, doCompose<CompositeOp::Union>},
            {"region.intersect", "Intersect", {}, {}, haveTwoRegions,
             doCompose<CompositeOp::Intersect>},
            // The one boolean with a question in it: which region is being cut
            // and which is doing the cutting. Optional because it has a visible
            // default — the upper out of the lower — and present at all because
            // a default is not an answer, it is a guess the surface has to be
            // able to overrule.
            {"region.subtract", "Subtract", {}, REGION_ORDER_PARAMETERS, haveTwoRegions,
             doCompose<CompositeOp::Subtract>},
            {"region.lift", "Lift out", {}, {}, haveComposite, doLift},
            {"region.punch", "Punch through", {}, {}, haveRegions, doPunch},
            {"region.raise", "Raise", {}, {}, haveRegions, doMoveRegion<1>},
            {"region.lower", "Lower", {}, {}, haveRegions, doMoveRegion<-1>},

            // Structure operations. Every one of them is a typed operation
            // rather than a gesture — the numbers come from the digits, and the
            // centre from the selection — which is why they take parameters and
            // carry no bindings: a key that rotated by some remembered amount
            // would be a key whose effect the user cannot read off the keyboard.
            {"transform.rotate", "Rotate", {}, ROTATE_PARAMETERS, haveTransformable, doRotate},
            {"transform.scale", "Scale", {}, SCALE_PARAMETERS, haveTransformable, doScale},
            // Listed, permanently dimmed. See `never`.
            {"transform.scale-non-uniform", "Scale non-uniformly (export only)", {},
             NON_UNIFORM_PARAMETERS, never, doScaleNonUniform},
            {"edit.duplicate", "Duplicate", "ctrl+d", OFFSET_PARAMETERS, haveDuplicable,
             doDuplicate},

            // Compound relations: primitives plus a tag, never a new kind.
            {"relation.distribute", "Distribute evenly", {}, {}, haveDistributable,
             doDistribute},
            {"relation.mirror", "Mirror", {}, {}, haveMirrorable, doMirror},

            // The tag surface. Dissolving is the deliberate half of graceful
            // dissolution, and the width and height fields are the rectangle
            // panel — the same slots its corner handles drive.
            {"tag.dissolve", "Dissolve tag", {}, {}, haveTags, doDissolveTags},
            {"tag.set-width", "Set width", {}, SIDE_PARAMETERS, haveRectangleTag,
             doSetSide<true>},
            {"tag.set-height", "Set height", {}, SIDE_PARAMETERS, haveRectangleTag,
             doSetSide<false>},

            // Styles. The first family whose Session methods write records no
            // earlier one wrote; the forking rule lives in Session, and the
            // registry contributes that a recolour is reachable identically from
            // the toolbar, the inspector, the palette, the keyboard and a script.
            // Width and opacity dim over an expression-driven slot, which is the
            // resistance the rectangle handle already carries.
            {"style.set-stroke", "Stroke colour", {}, COLOR_PARAMETERS, haveStyleableEntities,
             doSetStyleStroke},
            {"style.set-fill", "Fill colour", {}, COLOR_PARAMETERS, haveStyleable, doSetStyleFill},
            {"style.set-stroke-width", "Stroke width", {}, STYLE_VALUE_PARAMETERS, canStyleWidth,
             doSetStyleWidth},
            {"style.set-opacity", "Opacity", {}, STYLE_VALUE_PARAMETERS, canStyleOpacity,
             doSetStyleOpacity},
            {"style.set-filled", "Filled", {}, FLAG_PARAMETERS, haveStyleableRegions,
             doSetStyleFilled},
            {"style.create", "Create style", {}, STYLE_CREATE_PARAMETERS, haveStyleable,
             doCreateStyle},
            {"style.apply", "Apply style", {}, STYLE_APPLY_PARAMETERS, canApplyStyle,
             doApplyStyle},
            {"style.rename", "Rename style", {}, STYLE_RENAME_PARAMETERS, haveStyle,
             doRenameStyle},

            // Parameters. Named document values the slot graph references by id,
            // in create/set/rename/delete order so the shared conformance sweep
            // makes one before it sets or deletes it.
            {"parameter.create", "New parameter", {}, PARAM_CREATE_PARAMETERS, always,
             doCreateParameter},
            {"parameter.set", "Set parameter", {}, PARAM_SET_PARAMETERS, haveParameter,
             doSetParameter},
            {"parameter.rename", "Rename parameter", {}, PARAM_RENAME_PARAMETERS, haveParameter,
             doRenameParameter},
            {"parameter.delete", "Delete parameter", {}, PARAM_ID_PARAMETERS, haveParameter,
             doDeleteParameter},

            // Snap policy fields that affect an edit, so a toggle is a recorded
            // action rather than a raw policy mutation no script would capture.
            // Not journalled — session state — but recorded, so a script that
            // toggles grid snapping mid-drawing replays to the identical document.
            {"snap.set-grid", "Grid snapping", {}, GRID_PARAMETERS, always, doSetGrid},
            {"snap.set-construction-attract", "Construction attract", {}, ATTRACT_PARAMETERS,
             always, doSetConstructionAttract},

            // The only destructive path in the tool, and it leads out of it.
            {"export.bake", "Bake for export", {}, {}, always, doBake},
        };

        constexpr std::array<Strength, 3> STRENGTHS = {Strength::Impose, Strength::Reference,
                                                       Strength::Measure};
        // Three strings per generated row — name, title, description — reserved up
        // front: a reallocation here would dangle every view already handed out.
        c.storage.reserve(CONSTRAINT_KINDS.size() * STRENGTHS.size() * 3);
        c.rows.reserve(c.rows.size() + CONSTRAINT_KINDS.size() * STRENGTHS.size());
        for(const ConstraintKindInfo &info : CONSTRAINT_KINDS) {
            for(Strength strength : STRENGTHS) {
                c.storage.push_back(nameOf(info.kind, strength));
                const std::string &name = c.storage.back();
                c.storage.push_back(titleOf(info.kind, strength));
                const std::string &title = c.storage.back();
                c.storage.push_back(describeImposition(info.kind, strength));
                const std::string &description = c.storage.back();

                Action a;
                a.name = name;
                a.title = title;
                // No keyboard binding: twenty-two relations at three strengths
                // is sixty-six rows and the alphabet has twenty-six letters.
                // They are reached through the strip and the palette, which is
                // what those surfaces are for.
                a.binding = {};
                a.parameters = IMPOSE_PARAMETERS;
                a.applicable = canImpose;
                a.invoke = doImpose;
                a.generated = true;
                a.constraintKind = info.kind;
                a.strength = strength;
                a.description = description;
                c.rows.push_back(a);
            }
        }

        // The metadata finalize pass. Category is the prefix before the first
        // dot, a view into the name's own stable storage — "tool.line" groups
        // under "tool", every generated row under "constrain" — so the one
        // grouping rule reads off the stable token every surface already agrees
        // on rather than a second list. Hand-written descriptions come from the
        // table above; generated ones were composed in the loop.
        for(Action &a : c.rows) {
            const size_t dot = a.name.find('.');
            a.category = dot == std::string_view::npos ? a.name : a.name.substr(0, dot);
            if(a.description.empty()) a.description = descriptionFor(a.name);
        }
        return c;
    }();
    return table;
}

}  // namespace

std::optional<double> ActionArguments::value(std::string_view name) const {
    for(const auto &[key, v] : values) {
        if(key == name) return v;
    }
    return std::nullopt;
}

void ActionArguments::set(std::string_view name, double v) {
    for(auto &[key, existing] : values) {
        if(key == name) {
            existing = v;
            return;
        }
    }
    values.emplace_back(std::string(name), v);
}

std::optional<std::string_view> ActionArguments::text(std::string_view name) const {
    for(const auto &[key, v] : texts) {
        if(key == name) return std::string_view(v);
    }
    return std::nullopt;
}

void ActionArguments::setText(std::string_view name, std::string v) {
    for(auto &[key, existing] : texts) {
        if(key == name) {
            existing = std::move(v);
            return;
        }
    }
    texts.emplace_back(std::string(name), std::move(v));
}

std::span<const Action> actions() { return catalogue().rows; }

const Action *findAction(std::string_view name) {
    for(const Action &a : catalogue().rows) {
        if(a.name == name) return &a;
    }
    return nullptr;
}

KeyBinding resolveKey(const ActionContext &context, const KeyStroke &stroke) {
    KeyBinding out;
    auto action = [&](std::string_view name, std::optional<size_t> index) {
        const Action *a = findAction(name);
        if(a == nullptr) return;
        out.kind = KeyBinding::Kind::Action;
        out.action = a;
        if(index) out.arguments.set("index", static_cast<double>(*index));
    };

    // A stroke carrying Control is resolved here, before any other reading —
    // offers, fields and commands alike — and resolves the production ctrl+
    // chords or nothing. ctrl+d is an ordinary binding on edit.duplicate;
    // ctrl+z and ctrl+shift+z are aliases beside undo's and redo's bare
    // single-letter bindings, which stay exactly what they were. Every other
    // Control chord still resolves to nothing, so a platform whose text()
    // delivers the plain letter under Ctrl cannot alias ctrl+r into the
    // rectangle tool — the swallow the earlier stages relied on, now with three
    // holes cut in it by name rather than removed.
    if(has(stroke.modifiers, Modifier::Control)) {
        if(stroke.character != 0) {
            const char lowered = stroke.character >= 'A' && stroke.character <= 'Z'
                                     ? static_cast<char>(stroke.character - 'A' + 'a')
                                     : stroke.character;
            if(lowered >= 'a' && lowered <= 'z') {
                std::string chord = "ctrl+";
                if(has(stroke.modifiers, Modifier::Shift)) chord += "shift+";
                chord += lowered;
                if(const Action *a = actionForBinding(chord)) {
                    action(a->name, std::nullopt);
                } else if(chord == "ctrl+z") {
                    action("edit.undo", std::nullopt);
                } else if(chord == "ctrl+shift+z") {
                    action("edit.redo", std::nullopt);
                }
            }
        }
        return out;
    }

    // Offer numbers first, and only when a modifier claims them. Read off the
    // key's digit rather than the character it printed, because a shifted digit
    // prints whatever the layout says and the rank is not a property of the
    // layout.
    if(stroke.digit >= 1 && stroke.digit <= 9) {
        const auto index = static_cast<size_t>(stroke.digit - 1);
        if(has(stroke.modifiers, Modifier::Alt)) {
            action("inference.confirm", index);
            return out;
        }
        if(has(stroke.modifiers, Modifier::Shift)) {
            action("inference.decline", index);
            return out;
        }
    }

    if(stroke.character == 0) return out;

    // A field that is open swallows everything printable. Units are spelled
    // with the same letters the tools are bound to, and "45mm" must not
    // activate the rectangle tool halfway through.
    if(context.numericActive) {
        out.kind = KeyBinding::Kind::Text;
        out.character = stroke.character;
        return out;
    }

    // Opening one. Something with a numeric twin is in flight — a creation
    // tool's placement, or a drag of geometry that already exists — and this
    // reads as the start of a number rather than as a command. That is the
    // whole of "digits type a value into the strip", and what a bare digit
    // could not do while it was spent on confirming an offer.
    const bool startsValue = (stroke.character >= '0' && stroke.character <= '9') ||
                             stroke.character == '.' || stroke.character == '-';
    if((context.tool != ToolKind::Select || context.dragging) && startsValue) {
        out.kind = KeyBinding::Kind::Text;
        out.character = stroke.character;
        return out;
    }

    // Otherwise it is a command, spelled the way the table spells it.
    std::string binding;
    if(has(stroke.modifiers, Modifier::Shift)) binding += "shift+";
    const char lowered = stroke.character >= 'A' && stroke.character <= 'Z'
                             ? static_cast<char>(stroke.character - 'A' + 'a')
                             : stroke.character;
    if(lowered < 'a' || lowered > 'z') return out;
    binding += lowered;
    if(const Action *a = actionForBinding(binding)) action(a->name, std::nullopt);
    return out;
}

const Action *actionForBinding(std::string_view binding) {
    if(binding.empty()) return nullptr;
    for(const Action &a : catalogue().rows) {
        if(a.binding == binding) return &a;
    }
    return nullptr;
}

ActionContext contextOf(const Session &session) {
    ActionContext c;
    c.document = &session.document();
    c.signature = session.signature();
    c.selection = session.selection().items();
    c.canUndo = session.canUndo();
    c.canRedo = session.canRedo();
    c.tool = session.tool();
    c.offers = session.presentation().offers().size();
    c.inferred = session.presentation().inferred.size();
    c.numericActive = session.presentation().numericActive;
    c.dragging = session.presentation().dragging;
    c.closedLoop = !session.presentation().closedLoop.empty();
    c.healableLoop = !session.presentation().healableLoop.empty();
    c.selectedConstraints = session.selection().constraints().size();
    c.conflicting = session.presentation().conflicting;

    c.layer = session.targetLayer().value();
    for(RegionId id : session.selectedRegions()) {
        c.selectedRegions++;
        const RegionRecord *r = session.document().regions().find(id);
        if(r != nullptr && r->op != CompositeOp::Outline) c.selectedComposites++;
    }
    for(const GroupRecord &g : session.document().groups().records()) {
        for(EntityId m : g.members) {
            if(session.selection().contains(m)) {
                c.groupedSelection = true;
                break;
            }
        }
    }

    // The transform questions, asked of the same functions the transforms
    // themselves ask. An applicability predicate that computed its own answer is
    // a surface that can offer what the model refuses, which is the one thing
    // this table exists to make impossible.
    const std::vector<EntityId> moved = transformClosure(session.document(), c.selection);
    for(EntityId id : moved) {
        const EntityRecord *e = session.document().entities().find(id);
        if(e != nullptr && entityInfo(e->kind).ownParamCount > 0) {
            c.transformable++;
            // Mirrors the transform's own anyLocked gate: a rotation or scale
            // refuses whole if any parameter it would write sits on a locked
            // layer, so the predicate has to see the same lock the step would.
            if(isLocked(session.document(), id)) c.transformLocked = true;
        }
        if(e != nullptr && e->kind == EntityKind::Point && session.selection().contains(id)) {
            c.selectedPoints++;
        }
    }
    // A frame-referenced relation binding the closure dims rotate and scale for
    // the same reason a lock does: it means symmetry about the document frame
    // through no operand, so a transform can neither retarget nor rewrite it and
    // refuses whole. Set on the same flag — it dims the same two rows — so
    // applicable still equals runnable; duplicate ignores the flag and copies to
    // new geometry the drop rules handle. In record (ID) order, so the flag is
    // set the same way every frame.
    if(!c.transformLocked) {
        for(const ConstraintRecord &rel : session.document().constraints().records()) {
            if(!constraintInfo(rel.kind).frameReferenced) continue;
            const size_t bound = boundOperandCount(rel);
            for(size_t i = 0; i < bound; i++) {
                if(std::binary_search(moved.begin(), moved.end(), rel.operands[i])) {
                    c.transformLocked = true;
                    break;
                }
            }
            if(c.transformLocked) break;
        }
    }
    c.axisConstraints = axisReferencedIn(session.document(), moved).size();
    c.absoluteDimensions = absoluteValuedIn(session.document(), moved).size();
    // Axis relations already on a cluster frame in the moved set — the ones
    // retarget-to-document sheds — which axisReferencedIn skips. Walked in record
    // order for a stable count.
    for(const ConstraintRecord &rel : session.document().constraints().records()) {
        if(rel.kind != ConstraintKind::Horizontal && rel.kind != ConstraintKind::Vertical) continue;
        if(boundOperandCount(rel) <= constraintInfo(rel.kind).operandCount) continue;  // doc-framed
        if(std::binary_search(moved.begin(), moved.end(), rel.operands[0])) {
            c.clusteredAxisConstraints++;
        }
    }

    const EntityId axis = mirrorAxisIn(session.document(), c.selection);
    if(axis.valid()) {
        const EntityRecord *a = session.document().entities().find(axis);
        for(EntityId id : c.selection) {
            if(id == axis) continue;
            if(a != nullptr && (id == a->points[0] || id == a->points[1])) continue;
            c.mirrorable = true;
            break;
        }
    }

    for(TagId id : session.selectedTags()) {
        c.selectedTags++;
        if(rectangleFrame(session.document(), id)) c.rectangleTags++;
    }

    // Style applicability, asked of the same query the toolbar reads, so an
    // applicable style edit is one the toolbar can perform and the sweep can run.
    const Session::StyleAppearance appearance = session.resolvedAppearance();
    c.styleableEntities = appearance.entities;
    c.styleableRegions = appearance.regions;
    c.strokeWidthExpr = appearance.strokeWidthExpr;
    c.opacityExpr = appearance.opacityExpr;
    c.namedStyleCount = session.document().styles().size();
    c.parameterCount = session.document().parameters().size();

    // The first selected constraint's flags, for the relation actions that act on
    // it with no id. Read the same taxonomy the session's own checks read, so an
    // applicable row is one the session performs.
    if(!session.selection().constraints().empty()) {
        const ConstraintId first = session.selection().constraints().front();
        if(const ConstraintRecord *r = session.document().constraints().find(first)) {
            c.selectedConstraintValued = constraintInfo(r->kind).valueArity == 1;
            c.selectedConstraintHasAlternative = constraintInfo(r->kind).alternatives > 0;
        }
    }
    return c;
}

const Action *impositionAction(ConstraintKind kind, Strength strength) {
    for(const Action &a : catalogue().rows) {
        if(a.generated && a.constraintKind == kind && a.strength == strength) return &a;
    }
    return nullptr;
}

bool invokeAction(Session &session, std::string_view name, const ActionArguments &arguments) {
    const Action *action = findAction(name);
    if(action == nullptr) return false;
    if(action->applicable != nullptr && !action->applicable(contextOf(session), *action)) {
        return false;
    }

    // The schema is checked here rather than inside each action, so a missing
    // required parameter fails the same way whoever asked — a menu, a script,
    // a key — and no action has to remember to check. A text parameter reads the
    // string channel and a numeric one the value channel, so the check follows
    // the schema rather than assuming every argument is a number.
    for(const ActionParameter &p : action->parameters) {
        if(!p.required) continue;
        if(p.text ? !arguments.text(p.name) : !arguments.value(p.name)) return false;
    }
    if(action->invoke == nullptr) return false;
    return action->invoke(session, *action, arguments);
}

}  // namespace paroculus
