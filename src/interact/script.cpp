#include "interact/script.h"

#include <array>
#include <charconv>
#include <cstdio>

#include "core/persist.h"
#include "interact/session.h"

namespace paroculus {
namespace {

// %.17g, for the reason persist gives: the shortest form guaranteed to survive
// a double round-trip. A script is a recording, and a recording that replays
// half a pixel off has stopped reproducing what it was made to reproduce.
std::string number(double v) {
    std::array<char, 40> buf{};
    const int n = std::snprintf(buf.data(), buf.size(), "%.17g", v);
    return std::string(buf.data(), static_cast<size_t>(n < 0 ? 0 : n));
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
    }
    return "escape";
}

std::optional<Key> keyFrom(std::string_view s) {
    if(s == "escape") return Key::Escape;
    if(s == "delete") return Key::Delete;
    if(s == "undo") return Key::Undo;
    if(s == "redo") return Key::Redo;
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
                // Refused rather than silently falling back to select: a script
                // that quietly replayed a different tool would place different
                // geometry and look like a bug in the tool, not in the file.
                if(value != "select" && value != "line") {
                    return fail("unknown tool", lineNumber);
                }
                step.tool = toolFromName(value);
                haveName = true;
            }
            if(!haveName) return fail("tool step without a name", lineNumber);
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

void ScriptRecorder::key(Key key, Modifier modifiers) {
    ScriptStep step;
    step.kind = ScriptStep::Kind::Key;
    step.key = static_cast<uint8_t>(key);
    step.modifiers = modifiers;
    steps_.push_back(step);
}

}  // namespace paroculus
