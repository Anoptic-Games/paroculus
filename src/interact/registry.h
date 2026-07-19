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
    bool canUndo = false;
    bool canRedo = false;
    ToolKind tool = ToolKind::Select;
    size_t offers = 0;
    size_t inferred = 0;
};

struct Action {
    // Stable token. Scripts and bindings name actions by it, so it is format.
    std::string_view name;
    // What a surface shows. Presentation, and free to change.
    std::string_view title;
    // The keyboard projection, as data rather than as a switch somewhere else.
    std::string_view binding;
    std::span<const ActionParameter> parameters;

    bool (*applicable)(const ActionContext &) = nullptr;
    // Runs it. Returns false when the arguments do not satisfy the schema.
    bool (*invoke)(Session &, const ActionArguments &) = nullptr;
};

// The whole catalogue, in a stable order.
std::span<const Action> actions();

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

// Finds, checks applicability, checks the schema, and runs. Returns false when
// any of those fail, so one call is the whole contract a surface needs.
bool invokeAction(Session &session, std::string_view name,
                  const ActionArguments &arguments = {});

}  // namespace paroculus
