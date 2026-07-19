#include "interact/script.h"

#include <array>
#include <charconv>

#include "core/persist.h"
#include "interact/registry.h"
#include "interact/session.h"

namespace paroculus {
namespace {

// to_chars, for the reason persist gives: shortest round-trip, and
// locale-independent where the printf family is not. A script is a recording,
// and a recording that replays half a pixel off — or not at all, because the
// machine that wrote it spells the point as a comma — has stopped reproducing
// what it was made to reproduce.
std::string number(double v) {
    std::array<char, 48> buf{};
    const std::to_chars_result r = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    if(r.ec != std::errc{}) return "0";
    return std::string(buf.data(), static_cast<size_t>(r.ptr - buf.data()));
}

// A typed character as one field value.
//
// Fields are space-delimited, so a literal space would split the line and the
// parser would refuse a file the recorder had just written. The length grammar
// accepts a space — "45 mm" is a value a user types — so this is a key anyone
// can press, not a theoretical one. Printable characters stay literal because
// the format is meant to be hand-edited; the rest take a hex escape.
std::string typedChar(char c) {
    const auto u = static_cast<unsigned char>(c);
    if(u > 0x20 && u < 0x7f && c != '\\') return std::string(1, c);
    const char digits[] = "0123456789abcdef";
    return std::string("\\x") + digits[(u >> 4) & 0xf] + digits[u & 0xf];
}

std::optional<char> typedCharFrom(std::string_view s) {
    if(s.size() == 1 && s[0] != '\\') return s[0];
    if(s.size() != 4 || s[0] != '\\' || s[1] != 'x') return std::nullopt;
    int value = 0;
    for(size_t i = 2; i < 4; i++) {
        const char d = s[i];
        const int n = d >= '0' && d <= '9'   ? d - '0'
                      : d >= 'a' && d <= 'f' ? d - 'a' + 10
                                             : -1;
        if(n < 0) return std::nullopt;
        value = value * 16 + n;
    }
    return static_cast<char>(value);
}

std::optional<double> toDouble(std::string_view s) {
    double v = 0.0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if(r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

std::optional<int> toInt(std::string_view s) {
    int v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if(r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

std::string_view buttonName(Button b) {
    switch(b) {
        case Button::None:   return "none";
        case Button::Left:   return "left";
        case Button::Middle: return "middle";
        case Button::Right:  return "right";
    }
    return "none";
}

std::optional<Button> buttonFrom(std::string_view s) {
    if(s == "none") return Button::None;
    if(s == "left") return Button::Left;
    if(s == "middle") return Button::Middle;
    if(s == "right") return Button::Right;
    return std::nullopt;
}

std::string_view actionName(PointerAction a) {
    switch(a) {
        case PointerAction::Move:    return "move";
        case PointerAction::Press:   return "press";
        case PointerAction::Release: return "release";
    }
    return "move";
}

std::string_view keyName(Key k) {
    switch(k) {
        case Key::Escape: return "escape";
        case Key::Delete: return "delete";
        case Key::Undo:   return "undo";
        case Key::Redo:   return "redo";
        case Key::Enter:  return "enter";
        case Key::Tab:    return "tab";
    }
    return "escape";
}

std::optional<Key> keyFrom(std::string_view s) {
    if(s == "escape") return Key::Escape;
    if(s == "delete") return Key::Delete;
    if(s == "undo") return Key::Undo;
    if(s == "redo") return Key::Redo;
    if(s == "enter") return Key::Enter;
    if(s == "tab") return Key::Tab;
    return std::nullopt;
}

// Modifiers are written in a fixed order rather than in the order they arrived,
// so the same set always produces the same bytes.
std::string modifierNames(Modifier m) {
    std::string out;
    auto add = [&](Modifier bit, const char *name) {
        if(!has(m, bit)) return;
        if(!out.empty()) out += '+';
        out += name;
    };
    add(Modifier::Shift, "shift");
    add(Modifier::Control, "control");
    add(Modifier::Alt, "alt");
    return out;
}

std::optional<Modifier> modifiersFrom(std::string_view s) {
    Modifier out = Modifier::None;
    size_t start = 0;
    while(start <= s.size()) {
        const size_t plus = s.find('+', start);
        const std::string_view part =
            s.substr(start, plus == std::string_view::npos ? std::string_view::npos : plus - start);
        if(part == "shift") out |= Modifier::Shift;
        else if(part == "control") out |= Modifier::Control;
        else if(part == "alt") out |= Modifier::Alt;
        else if(!part.empty() && part != "none") return std::nullopt;
        if(plus == std::string_view::npos) break;
        start = plus + 1;
    }
    return out;
}

std::vector<std::string_view> splitTokens(std::string_view line) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while(i < line.size()) {
        while(i < line.size() && line[i] == ' ') i++;
        const size_t begin = i;
        while(i < line.size() && line[i] != ' ') i++;
        if(i > begin) out.push_back(line.substr(begin, i - begin));
    }
    return out;
}

// Splits "key=value". Returns nullopt for a token with no '='.
std::optional<std::pair<std::string_view, std::string_view>> field(std::string_view token) {
    const size_t eq = token.find('=');
    if(eq == std::string_view::npos) return std::nullopt;
    return std::pair{token.substr(0, eq), token.substr(eq + 1)};
}

std::optional<Eigen::Vector2d> vectorFrom(std::string_view s) {
    const size_t comma = s.find(',');
    if(comma == std::string_view::npos) return std::nullopt;
    const std::optional<double> x = toDouble(s.substr(0, comma));
    const std::optional<double> y = toDouble(s.substr(comma + 1));
    if(!x || !y) return std::nullopt;
    return Eigen::Vector2d(*x, *y);
}

std::string writeViewport(const Viewport &v) {
    const Eigen::Matrix2d l = v.view.matrix().linear();
    const Eigen::Vector2d t = v.view.matrix().translation();
    std::string out = "viewport";
    out += " width=" + number(v.width);
    out += " height=" + number(v.height);
    // Row-major linear part then translation: (x,y) -> (a*x + b*y + e,
    // c*x + d*y + f). Written out rather than reconstructed from a fitView call
    // because the view a session ran under is part of what was recorded.
    out += " m=" + number(l(0, 0)) + "," + number(l(0, 1)) + "," + number(l(1, 0)) + "," +
           number(l(1, 1)) + "," + number(t.x()) + "," + number(t.y());
    return out;
}

std::optional<Viewport> readViewport(const std::vector<std::string_view> &tokens) {
    Viewport v;
    bool haveMatrix = false;
    for(size_t i = 1; i < tokens.size(); i++) {
        const auto f = field(tokens[i]);
        if(!f) return std::nullopt;
        const auto [key, value] = *f;
        if(key == "width") {
            const std::optional<double> w = toDouble(value);
            if(!w) return std::nullopt;
            v.width = *w;
        } else if(key == "height") {
            const std::optional<double> h = toDouble(value);
            if(!h) return std::nullopt;
            v.height = *h;
        } else if(key == "m") {
            std::array<double, 6> m{};
            size_t start = 0;
            for(size_t k = 0; k < 6; k++) {
                const size_t comma = value.find(',', start);
                const bool last = k == 5;
                if(last != (comma == std::string_view::npos)) return std::nullopt;
                const std::optional<double> d = toDouble(
                    value.substr(start, last ? std::string_view::npos : comma - start));
                if(!d) return std::nullopt;
                m[k] = *d;
                start = comma + 1;
            }
            Eigen::Affine2d affine = Eigen::Affine2d::Identity();
            affine.linear() << m[0], m[1], m[2], m[3];
            affine.translation() << m[4], m[5];
            // A singular view has no inverse, so hit testing and every document
            // coordinate derived below it would be garbage. Refuse the file.
            if(affine.linear().determinant() == 0.0) return std::nullopt;
            v.view = ViewTransform(affine);
            haveMatrix = true;
        }
        // Unknown fields are ignored, matching persist's forward-safety: a
        // script written by a newer build stays replayable to the extent this
        // build understands it.
    }
    if(!haveMatrix) return std::nullopt;
    return v;
}

}  // namespace

std::string serializeScript(const GestureScript &script) {
    std::string out;
    out += "paroculus-script " + std::to_string(SCRIPT_VERSION) + "\n";

    // The starting document verbatim, in its own format, delimited rather than
    // escaped — so the document format stays the single authority on its own
    // bytes and this one never has to track its changes.
    out += "document\n";
    out += serialize(script.document);
    out += "end-document\n";

    for(const ScriptStep &s : script.steps) {
        switch(s.kind) {
            case ScriptStep::Kind::Viewport:
                out += writeViewport(s.viewport);
                break;
            case ScriptStep::Kind::Pointer: {
                out += actionName(s.action);
                out += " screen=" + number(s.screen.x()) + "," + number(s.screen.y());
                if(s.button != Button::None) {
                    out += " button=";
                    out += buttonName(s.button);
                }
                break;
            }
            case ScriptStep::Kind::Key:
                out += "key name=";
                out += keyName(static_cast<Key>(s.key));
                break;
            case ScriptStep::Kind::Tool:
                out += "tool name=";
                out += toolName(s.tool);
                break;
            case ScriptStep::Kind::Confirm:
                out += "confirm index=" + std::to_string(s.index);
                break;
            case ScriptStep::Kind::Decline:
                out += "decline index=" + std::to_string(s.index);
                break;
            case ScriptStep::Kind::Type:
                out += "type char=" + typedChar(s.character);
                break;
            case ScriptStep::Kind::NumericResolve:  out += "numeric do=resolve"; break;
            case ScriptStep::Kind::NumericImpose:   out += "numeric do=impose"; break;
            case ScriptStep::Kind::NumericBackspace: out += "numeric do=backspace"; break;
            case ScriptStep::Kind::NumericAdvance:  out += "numeric do=advance"; break;
            case ScriptStep::Kind::NumericCancel:   out += "numeric do=cancel"; break;
            case ScriptStep::Kind::Action:
                out += "action name=" + s.actionName;
                for(const auto &[key, v] : s.arguments) out += " " + key + "=" + number(v);
                break;
        }
        const std::string mods = modifierNames(s.modifiers);
        if(!mods.empty()) out += " mods=" + mods;
        out += "\n";
    }
    return out;
}

ScriptLoadResult parseScript(std::string_view text, GestureScript &out) {
    out = GestureScript();
    ScriptLoadResult result;
    size_t lineNumber = 0;

    auto fail = [&](std::string message, size_t line) {
        out = GestureScript();
        result.ok = false;
        result.error = std::move(message);
        result.line = line;
        return result;
    };

    std::vector<std::string_view> lines;
    size_t start = 0;
    while(start <= text.size()) {
        const size_t nl = text.find('\n', start);
        lines.push_back(text.substr(start, nl == std::string_view::npos ? std::string_view::npos
                                                                        : nl - start));
        if(nl == std::string_view::npos) break;
        start = nl + 1;
    }

    size_t index = 0;
    auto nextLine = [&]() -> std::optional<std::string_view> {
        while(index < lines.size()) {
            const std::string_view line = lines[index++];
            lineNumber = index;
            if(line.empty() || line.front() == '#') continue;
            return line;
        }
        return std::nullopt;
    };

    const std::optional<std::string_view> header = nextLine();
    if(!header) return fail("empty script", 1);
    const std::vector<std::string_view> headerTokens = splitTokens(*header);
    if(headerTokens.size() < 2 || headerTokens[0] != "paroculus-script") {
        return fail("not a paroculus script", lineNumber);
    }
    const std::optional<int> version = toInt(headerTokens[1]);
    if(!version) return fail("malformed script version", lineNumber);
    // Refused rather than half-read, for the same reason persist refuses:
    // guessing at a format you do not know produces a plausible wrong answer.
    if(*version > SCRIPT_VERSION) return fail("script written by a newer version", lineNumber);

    bool haveDocument = false;
    while(const std::optional<std::string_view> line = nextLine()) {
        const std::vector<std::string_view> tokens = splitTokens(*line);
        if(tokens.empty()) continue;
        const std::string_view kind = tokens[0];

        if(kind == "document") {
            if(haveDocument) return fail("more than one document block", lineNumber);
            std::string body;
            bool closed = false;
            while(index < lines.size()) {
                const std::string_view inner = lines[index++];
                lineNumber = index;
                if(inner == "end-document") {
                    closed = true;
                    break;
                }
                body += inner;
                body += '\n';
            }
            if(!closed) return fail("unterminated document block", lineNumber);
            const LoadResult loaded = deserialize(body, out.document);
            if(!loaded) return fail("document: " + loaded.error, lineNumber);
            haveDocument = true;
            continue;
        }

        ScriptStep step;
        if(kind == "viewport") {
            const std::optional<Viewport> viewport = readViewport(tokens);
            if(!viewport) return fail("malformed viewport", lineNumber);
            step.kind = ScriptStep::Kind::Viewport;
            step.viewport = *viewport;
        } else if(kind == "move" || kind == "press" || kind == "release") {
            step.kind = ScriptStep::Kind::Pointer;
            step.action = kind == "move"    ? PointerAction::Move
                          : kind == "press" ? PointerAction::Press
                                            : PointerAction::Release;
            bool haveScreen = false;
            for(size_t i = 1; i < tokens.size(); i++) {
                const auto f = field(tokens[i]);
                if(!f) return fail("malformed field", lineNumber);
                const auto [key, value] = *f;
                if(key == "screen") {
                    const std::optional<Eigen::Vector2d> v = vectorFrom(value);
                    if(!v) return fail("malformed screen position", lineNumber);
                    step.screen = *v;
                    haveScreen = true;
                } else if(key == "button") {
                    const std::optional<Button> b = buttonFrom(value);
                    if(!b) return fail("unknown button", lineNumber);
                    step.button = *b;
                } else if(key == "mods") {
                    const std::optional<Modifier> m = modifiersFrom(value);
                    if(!m) return fail("unknown modifier", lineNumber);
                    step.modifiers = *m;
                }
            }
            if(!haveScreen) return fail("pointer step without a position", lineNumber);
        } else if(kind == "tool") {
            step.kind = ScriptStep::Kind::Tool;
            bool haveName = false;
            for(size_t i = 1; i < tokens.size(); i++) {
                const auto f = field(tokens[i]);
                if(!f) return fail("malformed field", lineNumber);
                const auto [key, value] = *f;
                if(key != "name") continue;
                // Round-tripped through the name table rather than checked
                // against a list here, so a tool added there cannot leave this
                // behind. Refused rather than silently falling back to select:
                // a script that quietly replayed a different tool would place
                // different geometry and look like a bug in the tool rather
                // than in the file.
                step.tool = toolFromName(value);
                if(value != toolName(step.tool)) return fail("unknown tool", lineNumber);
                haveName = true;
            }
            if(!haveName) return fail("tool step without a name", lineNumber);
        } else if(kind == "confirm" || kind == "decline") {
            step.kind =
                kind == "confirm" ? ScriptStep::Kind::Confirm : ScriptStep::Kind::Decline;
            bool haveIndex = false;
            for(size_t i = 1; i < tokens.size(); i++) {
                const auto f = field(tokens[i]);
                if(!f) return fail("malformed field", lineNumber);
                const auto [key, value] = *f;
                if(key != "index") continue;
                const std::optional<int> n = toInt(value);
                if(!n || *n < 0) return fail("malformed index", lineNumber);
                step.index = static_cast<size_t>(*n);
                haveIndex = true;
            }
            if(!haveIndex) return fail("step without an index", lineNumber);
        } else if(kind == "action") {
            step.kind = ScriptStep::Kind::Action;
            for(size_t i = 1; i < tokens.size(); i++) {
                const auto f = field(tokens[i]);
                if(!f) return fail("malformed field", lineNumber);
                const auto [key, value] = *f;
                if(key == "name") {
                    step.actionName = std::string(value);
                    continue;
                }
                const std::optional<double> v = toDouble(value);
                if(!v) return fail("malformed action argument", lineNumber);
                step.arguments.emplace_back(std::string(key), *v);
            }
            if(step.actionName.empty()) return fail("action step without a name", lineNumber);
            // Refused at parse rather than ignored at replay: a script naming an
            // action this build does not have has asked for something that will
            // not happen, and silence would hide that.
            if(findAction(step.actionName) == nullptr) return fail("unknown action", lineNumber);
        } else if(kind == "type") {
            step.kind = ScriptStep::Kind::Type;
            bool haveChar = false;
            for(size_t i = 1; i < tokens.size(); i++) {
                const auto f = field(tokens[i]);
                if(!f) return fail("malformed field", lineNumber);
                const auto [key, value] = *f;
                if(key != "char") continue;
                const std::optional<char> c = typedCharFrom(value);
                if(!c) return fail("type takes one character", lineNumber);
                step.character = *c;
                haveChar = true;
            }
            if(!haveChar) return fail("type step without a character", lineNumber);
        } else if(kind == "numeric") {
            bool haveWhat = false;
            for(size_t i = 1; i < tokens.size(); i++) {
                const auto f = field(tokens[i]);
                if(!f) return fail("malformed field", lineNumber);
                const auto [key, value] = *f;
                if(key != "do") continue;
                if(value == "resolve") step.kind = ScriptStep::Kind::NumericResolve;
                else if(value == "impose") step.kind = ScriptStep::Kind::NumericImpose;
                else if(value == "backspace") step.kind = ScriptStep::Kind::NumericBackspace;
                else if(value == "advance") step.kind = ScriptStep::Kind::NumericAdvance;
                else if(value == "cancel") step.kind = ScriptStep::Kind::NumericCancel;
                else return fail("unknown numeric action", lineNumber);
                haveWhat = true;
            }
            if(!haveWhat) return fail("numeric step without an action", lineNumber);
        } else if(kind == "key") {
            step.kind = ScriptStep::Kind::Key;
            bool haveName = false;
            for(size_t i = 1; i < tokens.size(); i++) {
                const auto f = field(tokens[i]);
                if(!f) return fail("malformed field", lineNumber);
                const auto [key, value] = *f;
                if(key == "name") {
                    const std::optional<Key> k = keyFrom(value);
                    if(!k) return fail("unknown key", lineNumber);
                    step.key = static_cast<uint8_t>(*k);
                    haveName = true;
                } else if(key == "mods") {
                    const std::optional<Modifier> m = modifiersFrom(value);
                    if(!m) return fail("unknown modifier", lineNumber);
                    step.modifiers = *m;
                }
            }
            if(!haveName) return fail("key step without a name", lineNumber);
        } else {
            // An unknown step kind is refused rather than skipped. Persist
            // preserves unknown *records* because dropping one silently
            // truncates a document; here, skipping a step silently replays a
            // different session, which is worse than refusing to replay.
            return fail("unknown step kind", lineNumber);
        }

        out.steps.push_back(step);
    }

    if(!haveDocument) return fail("script has no document block", 0);
    result.ok = true;
    return result;
}

void applyStep(Session &session, const ScriptStep &step) {
    switch(step.kind) {
        case ScriptStep::Kind::Viewport:
            session.setViewport(step.viewport);
            return;
        case ScriptStep::Kind::Pointer:
            // Both spaces filled from the screen position through the viewport
            // in force, exactly as the shell fills them from a QEvent. The
            // document coordinate is derived, never stored, so it cannot
            // disagree with the transform the rest of the replay uses.
            session.handle(PointerEvent::at(step.action, step.screen, session.viewport().view,
                                            step.button, step.modifiers));
            return;
        case ScriptStep::Kind::Key:
            session.handle(static_cast<Key>(step.key), step.modifiers);
            return;
        case ScriptStep::Kind::Tool:
            session.setTool(step.tool);
            return;
        case ScriptStep::Kind::Confirm:
            session.confirmOffer(step.index);
            return;
        case ScriptStep::Kind::Decline:
            session.declineInference(step.index);
            return;
        case ScriptStep::Kind::Type:
            session.type(step.character);
            return;
        case ScriptStep::Kind::NumericResolve:   session.numericResolve(false); return;
        case ScriptStep::Kind::NumericImpose:    session.numericResolve(true); return;
        case ScriptStep::Kind::NumericBackspace: session.numericBackspace(); return;
        case ScriptStep::Kind::NumericAdvance:   session.numericAdvance(); return;
        case ScriptStep::Kind::NumericCancel:    session.numericCancel(); return;
        case ScriptStep::Kind::Action: {
            ActionArguments arguments;
            for(const auto &[key, v] : step.arguments) arguments.set(key, v);
            invokeAction(session, step.actionName, arguments);
            return;
        }
    }
}

void replay(Session &session, const GestureScript &script) {
    for(const ScriptStep &step : script.steps) applyStep(session, step);
}

void ScriptRecorder::viewport(const Viewport &viewport) {
    ScriptStep step;
    step.kind = ScriptStep::Kind::Viewport;
    step.viewport = viewport;
    steps_.push_back(step);
}

void ScriptRecorder::pointer(const PointerEvent &event) {
    ScriptStep step;
    step.kind = ScriptStep::Kind::Pointer;
    step.action = event.action;
    step.button = event.button;
    step.modifiers = event.modifiers;
    step.screen = event.screen;
    steps_.push_back(step);
}

void ScriptRecorder::tool(ToolKind kind) {
    ScriptStep step;
    step.kind = ScriptStep::Kind::Tool;
    step.tool = kind;
    steps_.push_back(step);
}

void ScriptRecorder::confirm(size_t index) {
    ScriptStep step;
    step.kind = ScriptStep::Kind::Confirm;
    step.index = index;
    steps_.push_back(step);
}

void ScriptRecorder::decline(size_t index) {
    ScriptStep step;
    step.kind = ScriptStep::Kind::Decline;
    step.index = index;
    steps_.push_back(step);
}

void ScriptRecorder::type(char c) {
    ScriptStep step;
    step.kind = ScriptStep::Kind::Type;
    step.character = c;
    steps_.push_back(step);
}

void ScriptRecorder::numeric(ScriptStep::Kind kind) {
    ScriptStep step;
    step.kind = kind;
    steps_.push_back(step);
}

void ScriptRecorder::action(std::string_view name,
                            const std::vector<std::pair<std::string, double>> &args) {
    ScriptStep step;
    step.kind = ScriptStep::Kind::Action;
    step.actionName = std::string(name);
    step.arguments = args;
    steps_.push_back(step);
}

void ScriptRecorder::key(Key key, Modifier modifiers) {
    ScriptStep step;
    step.kind = ScriptStep::Kind::Key;
    step.key = static_cast<uint8_t>(key);
    step.modifiers = modifiers;
    steps_.push_back(step);
}

}  // namespace paroculus
