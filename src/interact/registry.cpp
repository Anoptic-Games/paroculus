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
    {"inference.confirm", "Confirm relation", "1..9", INDEX_PARAMETER, haveOffers, doConfirm},
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
