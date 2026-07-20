#include "interact/registry.h"

#include <algorithm>
#include <array>

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

bool haveSelection(const ActionContext &c, const Action &) { return !c.signature.empty(); }
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
            {"edit.delete", "Delete", "del", {}, haveSelection, doDelete},
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
        };

        constexpr std::array<Strength, 3> STRENGTHS = {Strength::Impose, Strength::Reference,
                                                       Strength::Measure};
        // Two strings per generated row, reserved up front: a reallocation here
        // would dangle every name already handed out.
        c.storage.reserve(CONSTRAINT_KINDS.size() * STRENGTHS.size() * 2);
        c.rows.reserve(c.rows.size() + CONSTRAINT_KINDS.size() * STRENGTHS.size());
        for(const ConstraintKindInfo &info : CONSTRAINT_KINDS) {
            for(Strength strength : STRENGTHS) {
                c.storage.push_back(nameOf(info.kind, strength));
                const std::string &name = c.storage.back();
                c.storage.push_back(titleOf(info.kind, strength));
                const std::string &title = c.storage.back();

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
                c.rows.push_back(a);
            }
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
    // a key — and no action has to remember to check.
    for(const ActionParameter &p : action->parameters) {
        if(p.required && !arguments.value(p.name)) return false;
    }
    if(action->invoke == nullptr) return false;
    return action->invoke(session, *action, arguments);
}

}  // namespace paroculus
