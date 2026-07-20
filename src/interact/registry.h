// Actions as data.
//
// One registry holds every action: what it is called, what it takes, whether it
// applies right now, and how to run it. Menus, the context strip, the command
// palette, keyboard dispatch and the scripting harness are all projections of
// this table rather than parallel implementations of it — so an action
// reachable one way is reachable every way, and an action inapplicable in the
// model is offerable by no surface.
//
// That property is the point, and it only holds if nothing takes a shortcut. A
// surface that calls Session directly is a surface that can offer something the
// model would refuse, and the bug it produces looks like a model bug rather than
// a UI one. The registry is where that stops being possible.
//
// Every action is invocable headlessly with parameters, because the registry is
// the automation surface as well as the UI's. Keyboard reachability falls out of
// the same property rather than being maintained beside it.
//
// Stage 4 builds the table and drives it from scripts. Projecting it onto menus
// and a context strip is stage 5, and the projections take no new information —
// which is the test of whether the table carries enough.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/document.h"
#include "interact/events.h"
#include "interact/selection.h"
#include "interact/tools.h"

namespace paroculus {

class Session;

// One parameter an action accepts. Numbers only in v0 — the language grows to
// slots and expressions with the property editor, and the schema is here so it
// can grow rather than be retrofitted.
struct ActionParameter {
    std::string_view name;
    bool required = false;
};

// The arguments one invocation carries.
struct ActionArguments {
    std::vector<std::pair<std::string, double>> values;

    std::optional<double> value(std::string_view name) const;
    void set(std::string_view name, double v);
};

// What applicability is decided against.
//
// A snapshot rather than the session itself, so deciding whether an action
// applies cannot change what it is deciding about — and so a surface can ask
// the question without holding anything it could mutate.
struct ActionContext {
    const Document *document = nullptr;
    Signature signature;
    // What is selected, not merely what kinds are. Applicability over a
    // constraint kind is a question about the signature, but which operand
    // fills which slot is a question about the entities — and an action that
    // could not name its operands would be an action no surface could invoke.
    std::vector<EntityId> selection;
    // Relations the user is currently walking, from the last refused
    // imposition. What makes a conflict set selectable rather than merely
    // rendered.
    std::vector<ConstraintId> conflicting;
    bool canUndo = false;
    bool canRedo = false;
    ToolKind tool = ToolKind::Select;
    size_t offers = 0;
    size_t inferred = 0;
    // A closed outline is under the selection or was just drawn, so make-solid
    // has something to fill.
    bool closedLoop = false;
    // An almost-closed outline: heal-and-fill has gaps to shut.
    bool healableLoop = false;
    // A driving/reference toggle has something to toggle.
    size_t selectedConstraints = 0;
    // The layer the composition actions would act on. Zero is the implicit base
    // layer, which has no record and therefore nothing to hide, lock or
    // reorder — every layer action dims until the user has made one.
    uint32_t layer = 0;
    // Regions the selection reaches, and how many of those are composites. A
    // region is named by selecting what bounds it, so both are counts over the
    // geometry selection rather than a selection kind of their own.
    size_t selectedRegions = 0;
    size_t selectedComposites = 0;
    // Some of the selection is in a group, so there is a grouping to dissolve.
    bool groupedSelection = false;
    // A numeric field is open. Keyboard resolution needs it: while one is, the
    // letters that would pick a tool are the letters a unit is spelled with.
    bool numericActive = false;
    // A drag of existing geometry is in flight. Keyboard resolution needs this
    // too: a drag has a numeric twin exactly as a placement does, and digits
    // have to open its field even though the tool in force is Select.
    bool dragging = false;
};

struct Action {
    // Stable token. Scripts and bindings name actions by it, so it is format.
    std::string_view name;
    // What a surface shows. Presentation, and free to change.
    std::string_view title;
    // The keyboard projection, as data rather than as a switch somewhere else.
    std::string_view binding;
    std::span<const ActionParameter> parameters;

    // Both take the action itself, so one function can serve every row the
    // taxonomy generates. That is what keeps the constraint catalogue a
    // projection of one list rather than twenty-two hand-written triples: a new
    // relation is a row in CONSTRAINT_KINDS and it reaches the strip, the
    // palette, the keyboard and the script format without anything else being
    // edited.
    bool (*applicable)(const ActionContext &, const Action &) = nullptr;
    // Runs it. Returns false when the arguments do not satisfy the schema.
    bool (*invoke)(Session &, const Action &, const ActionArguments &) = nullptr;

    // What a generated row carries, and meaningless on a hand-written one.
    bool generated = false;
    ConstraintKind constraintKind = ConstraintKind::Coincident;
    Strength strength = Strength::Impose;
};

// The whole catalogue, in a stable order: the hand-written rows first, then one
// triple of imposition rows per constraint kind in taxonomy order.
//
// Built once at first use rather than declared constexpr, because the
// constraint half is generated from CONSTRAINT_KINDS and generated names have
// to be owned somewhere. Stable order and stable storage are the two properties
// that matter, and both hold: the table is built in one pass and never touched
// again, so a name's string_view outlives every caller.
std::span<const Action> actions();

// The action that imposes `kind` at `strength`, or nullptr. What a surface
// projecting the offers reaches for, so no surface has to spell an action name.
const Action *impositionAction(ConstraintKind kind, Strength strength);

// Returns nullptr for a name the table does not hold. Unknown is refused rather
// than ignored: a script naming an action this build lacks has asked for
// something that will not happen, and silence would hide that.
const Action *findAction(std::string_view name);

// The action a binding names, or nullptr. What makes keyboard dispatch a
// projection of the table rather than a switch maintained beside it: a binding
// added here reaches the keyboard without anything else being edited.
const Action *actionForBinding(std::string_view binding);

// The context a session presents right now.
ActionContext contextOf(const Session &session);

// ---------------------------------------------------------------------------
// Keyboard resolution
// ---------------------------------------------------------------------------

// One keystroke, as a surface sees it before deciding what it means.
struct KeyStroke {
    // The character the key produced, or 0. What a numeric field consumes, and
    // what an action binding is spelled with.
    char character = 0;
    // The digit engraved on the key, 1..9, or 0 for anything else. Carried
    // separately from `character` on purpose: shift+4 prints a dollar sign on
    // one layout and something else on the next, and the offer at rank four is
    // neither of them.
    int digit = 0;
    Modifier modifiers = Modifier::None;
};

// What a keystroke means right now.
struct KeyBinding {
    enum class Kind : uint8_t {
        None,    // nothing is bound; the surface may pass the key on
        Action,  // run `action` with `arguments`
        Text,    // feed `character` to the numeric field, opening one if needed
    };
    Kind kind = Kind::None;
    const Action *action = nullptr;
    ActionArguments arguments;
    char character = 0;
};

// Resolves a keystroke against the catalogue and the session's state.
//
// This lives here rather than in the shell because it is a policy, and a policy
// in a Qt event handler is a policy no headless test can reach — which is
// exactly how the digits came to be unable to open a numeric field while the
// documentation said they could.
//
// The one collision worth naming: numeric entry and offer confirmation are
// live in precisely the same state, a creation tool with a placement in
// flight, so no test of context can tell them apart and the binding has to.
// Digits type. Every gesture has a numeric twin and typing a value is the
// frequent, primary path; confirming a ranked offer is the occasional one, and
// it takes alt. Decline keeps shift because it acts on the last commit rather
// than on the placement in flight — confirm and decline are not a pair to be
// kept symmetric, they act on different objects.
KeyBinding resolveKey(const ActionContext &context, const KeyStroke &stroke);

// Finds, checks applicability, checks the schema, and runs. Returns false when
// any of those fail, so one call is the whole contract a surface needs.
bool invokeAction(Session &session, std::string_view name,
                  const ActionArguments &arguments = {});

}  // namespace paroculus
