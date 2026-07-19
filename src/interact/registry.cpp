#include "interact/registry.h"

#include <algorithm>
#include <array>

#include "interact/session.h"

namespace paroculus {
namespace {

constexpr ActionParameter INDEX_PARAMETER[] = {{"index", true}};

bool always(const ActionContext &) { return true; }

bool haveSelection(const ActionContext &c) { return !c.signature.empty(); }
bool canUndo(const ActionContext &c) { return c.canUndo; }
bool canRedo(const ActionContext &c) { return c.canRedo; }
bool haveOffers(const ActionContext &c) { return c.offers > 0; }
bool haveInferred(const ActionContext &c) { return c.inferred > 0; }

template <ToolKind Kind>
bool activateTool(Session &session, const ActionArguments &) {
    session.setTool(Kind);
    return true;
}

bool doDelete(Session &session, const ActionArguments &) {
    session.handle(Key::Delete);
    return true;
}

bool doUndo(Session &session, const ActionArguments &) {
    session.handle(Key::Undo);
    return true;
}

bool doRedo(Session &session, const ActionArguments &) {
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

bool doConfirm(Session &session, const ActionArguments &arguments) {
    const std::optional<size_t> index = indexOf(arguments);
    if(!index) return false;
    session.confirmOffer(*index);
    return true;
}

bool doDecline(Session &session, const ActionArguments &arguments) {
    const std::optional<size_t> index = indexOf(arguments);
    if(!index) return false;
    session.declineInference(*index);
    return true;
}

// The catalogue. Adding an action is adding a row here, and every surface picks
// it up — which is the whole reason the table exists.
constexpr std::array<Action, 10> CATALOGUE = {{
    {"tool.select", "Select", "v", {}, always, activateTool<ToolKind::Select>},
    {"tool.line", "Line", "l", {}, always, activateTool<ToolKind::Line>},
    {"tool.circle", "Circle", "c", {}, always, activateTool<ToolKind::Circle>},
    {"tool.arc", "Arc", "a", {}, always, activateTool<ToolKind::Arc>},
    {"tool.rectangle", "Rectangle", "r", {}, always, activateTool<ToolKind::Rectangle>},
    {"edit.delete", "Delete", "del", {}, haveSelection, doDelete},
    {"edit.undo", "Undo", "z", {}, canUndo, doUndo},
    {"edit.redo", "Redo", "shift+z", {}, canRedo, doRedo},
    // alt, not a bare digit: a bare digit is a digit. See resolveKey.
    {"inference.confirm", "Confirm relation", "alt+1..9", INDEX_PARAMETER, haveOffers, doConfirm},
    {"inference.decline", "Decline relation", "shift+1..9", INDEX_PARAMETER, haveInferred,
     doDecline},
}};

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

std::span<const Action> actions() { return CATALOGUE; }

const Action *findAction(std::string_view name) {
    for(const Action &a : CATALOGUE) {
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

    // Opening one. A creation tool is running and this reads as the start of a
    // number rather than as a command — which is the whole of "digits type a
    // value into the strip", and what a bare digit could not do while it was
    // spent on confirming an offer.
    const bool startsValue = (stroke.character >= '0' && stroke.character <= '9') ||
                             stroke.character == '.' || stroke.character == '-';
    if(context.tool != ToolKind::Select && startsValue) {
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
    for(const Action &a : CATALOGUE) {
        if(a.binding == binding) return &a;
    }
    return nullptr;
}

ActionContext contextOf(const Session &session) {
    ActionContext c;
    c.document = &session.document();
    c.signature = session.signature();
    c.canUndo = session.canUndo();
    c.canRedo = session.canRedo();
    c.tool = session.tool();
    c.offers = session.presentation().offers().size();
    c.inferred = session.presentation().inferred.size();
    c.numericActive = session.presentation().numericActive;
    return c;
}

bool invokeAction(Session &session, std::string_view name, const ActionArguments &arguments) {
    const Action *action = findAction(name);
    if(action == nullptr) return false;
    if(action->applicable != nullptr && !action->applicable(contextOf(session))) return false;

    // The schema is checked here rather than inside each action, so a missing
    // required parameter fails the same way whoever asked — a menu, a script,
    // a key — and no action has to remember to check.
    for(const ActionParameter &p : action->parameters) {
        if(p.required && !arguments.value(p.name)) return false;
    }
    if(action->invoke == nullptr) return false;
    return action->invoke(session, arguments);
}

}  // namespace paroculus
