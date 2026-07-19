#include "core/persist.h"

#include <array>
#include <charconv>
#include <vector>

namespace paroculus {

// Grants persist the wholesale table access a load needs. A load is not an
// edit: it must not be journalled, and it must accept records whose operands
// appear later in the file, which the command layer would rightly refuse one at
// a time. Validation happens once, over the finished document.
struct DocumentLoader {
    static RecordTable<EntityRecord> &entities(Document &d) { return d.entities_; }
    static RecordTable<ConstraintRecord> &constraints(Document &d) { return d.constraints_; }
    static RecordTable<RegionRecord> &regions(Document &d) { return d.regions_; }
    static RecordTable<TagRecord> &tags(Document &d) { return d.tags_; }
    static RecordTable<StyleRecord> &styles(Document &d) { return d.styles_; }
    static RecordTable<LayerRecord> &layers(Document &d) { return d.layers_; }
    static RecordTable<GroupRecord> &groups(Document &d) { return d.groups_; }
    static ParameterTable &parameters(Document &d) { return d.parameters_; }
    static std::vector<std::string> &unknown(Document &d) { return d.unknown_; }
};

namespace {

// to_chars, not snprintf: the printf family honours LC_NUMERIC, and
// QGuiApplication calls setlocale(LC_ALL, "") on Unix, so under a
// comma-decimal locale a "%g" save writes 1,5 and from_chars — which is
// locale-fixed to '.' — refuses to read it back. Serialization is
// machine-independent or it is not the storage of record. to_chars is also
// shortest-round-trip rather than 17 significant digits, so 0.1 writes as 0.1.
std::string number(double v) {
    std::array<char, 48> buf{};
    const std::to_chars_result r = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    if(r.ec != std::errc{}) return "0";
    return std::string(buf.data(), static_cast<size_t>(r.ptr - buf.data()));
}

// Names are quoted and escaped so a name containing a space or a quote cannot
// forge extra fields on the line.
//
// Control characters take a hex escape rather than riding through literally. A
// name holding a newline would otherwise split its own record: the loader reads
// the file a line at a time, so the tail would arrive as a record kind nothing
// recognises and be kept verbatim as an unknown one. The round trip would
// silently truncate the name and grow a junk line, which is worse than
// refusing. Names are arbitrary strings through the command layer, so this is
// reachable rather than theoretical.
std::string quote(std::string_view s) {
    const char digits[] = "0123456789abcdef";
    std::string out = "\"";
    for(char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if(u < 0x20 || u == 0x7f) {
            out += "\\x";
            out += digits[(u >> 4) & 0xf];
            out += digits[u & 0xf];
            continue;
        }
        if(c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

std::string_view roleName(Role r) { return r == Role::Construction ? "construction" : "normal"; }

std::string_view tagKindName(TagKind k) {
    switch(k) {
        case TagKind::Rectangle:    return "rectangle";
        case TagKind::Distribution: return "distribution";
        case TagKind::Mirror:       return "mirror";
    }
    return "rectangle";
}

// Expression nodes are written with explicit operand indices rather than as
// postfix, because a loaded expression may share subtrees and postfix would
// silently unshare them.
std::string writeSlot(const Slot &s) {
    if(s.isConstant()) return number(s.constant());

    // Separated by ';' rather than spaces: fields on a record line are
    // space-delimited, so a space here would split one value into several.
    std::string out = "[";
    bool first = true;
    for(const ExprNode &n : s.nodes()) {
        if(!first) out += ';';
        first = false;
        switch(n.op) {
            case ExprOp::Constant: out += "c" + number(n.constant); break;
            case ExprOp::Parameter: out += "p" + std::to_string(n.parameter.value()); break;
            case ExprOp::Negate:
                out += "~" + std::to_string(n.lhs);
                break;
            default: {
                const char op = n.op == ExprOp::Add        ? '+'
                                : n.op == ExprOp::Subtract ? '-'
                                : n.op == ExprOp::Multiply ? '*'
                                                           : '/';
                out += op + std::to_string(n.lhs) + "," + std::to_string(n.rhs);
                break;
            }
        }
    }
    out += "]";
    return out;
}

template <typename Id>
std::string idList(const std::vector<Id> &ids) {
    if(ids.empty()) return "-";
    std::string out;
    for(size_t i = 0; i < ids.size(); i++) {
        if(i) out += ',';
        out += std::to_string(ids[i].value());
    }
    return out;
}

// ---------------------------------------------------------------------------
// Parsing helpers
// ---------------------------------------------------------------------------

// Splits a record line into its leading kind token and the remaining
// key=value fields, respecting quotes so a quoted name may contain spaces.
struct Fields {
    std::string kind;
    std::vector<std::string> tokens;
};

Fields split(std::string_view line) {
    Fields f;
    std::string current;
    bool inQuotes = false;
    bool escaped = false;

    for(char c : line) {
        if(escaped) {
            current += c;
            escaped = false;
            continue;
        }
        if(c == '\\' && inQuotes) {
            escaped = true;
            current += c;
            continue;
        }
        if(c == '"') inQuotes = !inQuotes;
        if(c == ' ' && !inQuotes) {
            if(!current.empty()) f.tokens.push_back(std::move(current));
            current.clear();
            continue;
        }
        current += c;
    }
    if(!current.empty()) f.tokens.push_back(std::move(current));
    if(!f.tokens.empty()) {
        f.kind = f.tokens.front();
        f.tokens.erase(f.tokens.begin());
    }
    return f;
}

std::optional<std::string_view> field(const Fields &f, std::string_view key) {
    for(const std::string &t : f.tokens) {
        if(t.size() > key.size() && t.compare(0, key.size(), key) == 0 && t[key.size()] == '=') {
            return std::string_view(t).substr(key.size() + 1);
        }
    }
    return std::nullopt;
}

std::optional<double> toDouble(std::string_view s) {
    double v = 0.0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if(r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

// Layer order is signed: a layer can sit below the default. Reading it through
// toUint would silently turn every negative order into zero.
std::optional<int32_t> toInt(std::string_view s) {
    int32_t v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if(r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

std::optional<uint32_t> toUint(std::string_view s) {
    uint32_t v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if(r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

// The inverse of quote(). A backslash escapes the character after it, except
// for \xHH which names one by code — so a name holding the four literal
// characters of an escape survives, because quote() doubled its backslash.
std::string unquote(std::string_view s) {
    auto hex = [](char c) {
        if(c >= '0' && c <= '9') return c - '0';
        if(c >= 'a' && c <= 'f') return c - 'a' + 10;
        if(c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    if(s.size() < 2 || s.front() != '"' || s.back() != '"') return std::string(s);
    std::string out;
    for(size_t i = 1; i + 1 < s.size(); i++) {
        if(s[i] != '\\' || i + 2 >= s.size()) {
            out += s[i];
            continue;
        }
        if(s[i + 1] == 'x' && i + 4 < s.size()) {
            const int high = hex(s[i + 2]);
            const int low = hex(s[i + 3]);
            if(high >= 0 && low >= 0) {
                out += static_cast<char>(high * 16 + low);
                i += 3;
                continue;
            }
        }
        i++;
        out += s[i];
    }
    return out;
}

template <typename Id>
std::optional<std::vector<Id>> parseIdList(std::string_view s) {
    std::vector<Id> out;
    if(s == "-") return out;
    size_t start = 0;
    while(start <= s.size()) {
        const size_t comma = s.find(',', start);
        const std::string_view token =
            s.substr(start, comma == std::string_view::npos ? std::string_view::npos
                                                            : comma - start);
        const std::optional<uint32_t> v = toUint(token);
        if(!v) return std::nullopt;
        out.push_back(Id(*v));
        if(comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return out;
}

std::optional<Slot> parseSlot(std::string_view s) {
    if(s.empty()) return std::nullopt;
    if(s.front() != '[') {
        const std::optional<double> v = toDouble(s);
        if(!v) return std::nullopt;
        return Slot(*v);
    }
    if(s.back() != ']') return std::nullopt;

    std::vector<ExprNode> nodes;
    const std::string_view body = s.substr(1, s.size() - 2);
    size_t pos = 0;
    while(pos < body.size()) {
        size_t end = body.find(';', pos);
        if(end == std::string_view::npos) end = body.size();
        const std::string_view token = body.substr(pos, end - pos);
        pos = end + 1;
        if(token.empty()) continue;

        ExprNode n;
        const char head = token.front();
        const std::string_view rest = token.substr(1);
        if(head == 'c') {
            const std::optional<double> v = toDouble(rest);
            if(!v) return std::nullopt;
            n.op = ExprOp::Constant;
            n.constant = *v;
        } else if(head == 'p') {
            const std::optional<uint32_t> v = toUint(rest);
            if(!v) return std::nullopt;
            n.op = ExprOp::Parameter;
            n.parameter = ParameterId(*v);
        } else if(head == '~') {
            const std::optional<uint32_t> v = toUint(rest);
            if(!v) return std::nullopt;
            n.op = ExprOp::Negate;
            n.lhs = *v;
        } else {
            const size_t comma = rest.find(',');
            if(comma == std::string_view::npos) return std::nullopt;
            const std::optional<uint32_t> a = toUint(rest.substr(0, comma));
            const std::optional<uint32_t> b = toUint(rest.substr(comma + 1));
            if(!a || !b) return std::nullopt;
            n.op = head == '+'   ? ExprOp::Add
                   : head == '-' ? ExprOp::Subtract
                   : head == '*' ? ExprOp::Multiply
                   : head == '/' ? ExprOp::Divide
                                 : ExprOp::Constant;
            if(n.op == ExprOp::Constant) return std::nullopt;  // unknown operator
            n.lhs = *a;
            n.rhs = *b;
        }
        // Operands must index strictly below, which is the invariant that makes
        // evaluation a single forward pass and self-reference impossible.
        const auto here = static_cast<uint32_t>(nodes.size());
        if(n.op != ExprOp::Constant && n.op != ExprOp::Parameter) {
            if(n.lhs >= here) return std::nullopt;
            if(n.op != ExprOp::Negate && n.rhs >= here) return std::nullopt;
        }
        nodes.push_back(n);
    }
    if(nodes.empty()) return std::nullopt;
    return Slot::fromNodes(std::move(nodes));
}

}  // namespace

// ---------------------------------------------------------------------------
// Serialize
// ---------------------------------------------------------------------------

std::string serialize(const Document &doc) {
    std::string out;
    out += "paroculus " + std::to_string(FORMAT_VERSION) + "\n";

    // ID watermarks. Without these a reopened document would reissue an ID that
    // a deleted record once held, and any surviving reference to it would
    // silently rebind.
    out += "watermark";
    out += " entity=" + std::to_string(doc.entities().allocator().next());
    out += " constraint=" + std::to_string(doc.constraints().allocator().next());
    out += " region=" + std::to_string(doc.regions().allocator().next());
    out += " tag=" + std::to_string(doc.tags().allocator().next());
    out += " style=" + std::to_string(doc.styles().allocator().next());
    out += " layer=" + std::to_string(doc.layers().allocator().next());
    out += " group=" + std::to_string(doc.groups().allocator().next());
    out += " parameter=" + std::to_string(doc.parameters().allocator().next());
    out += "\n";

    // Sections in dependency order, records in ID order within each. Nothing
    // here consults a hash map, so the byte output is a function of the
    // document alone.
    for(const LayerRecord &r : doc.layers().records()) {
        out += "layer " + std::to_string(r.id.value()) + " name=" + quote(r.name) +
               " order=" + std::to_string(r.order) + " visible=" + (r.visible ? "1" : "0") +
               " locked=" + (r.locked ? "1" : "0") + "\n";
    }
    for(const StyleRecord &r : doc.styles().records()) {
        out += "style " + std::to_string(r.id.value()) + " name=" + quote(r.name) +
               " stroke-width=" + writeSlot(r.strokeWidth) +
               " stroke=" + std::to_string(r.strokeColor) +
               " fill=" + std::to_string(r.fillColor) + " filled=" + (r.filled ? "1" : "0") +
               "\n";
    }
    for(const ParameterRecord &r : doc.parameters().records()) {
        out += "parameter " + std::to_string(r.id.value()) + " name=" + quote(r.name) +
               " value=" + writeSlot(r.value) + "\n";
    }
    for(const EntityRecord &r : doc.entities().records()) {
        const EntityKindInfo &info = entityInfo(r.kind);
        out += "entity " + std::to_string(r.id.value()) + " kind=" + std::string(info.name) +
               " role=" + std::string(roleName(r.role)) +
               " layer=" + std::to_string(r.layer.value());
        out += " points=";
        if(info.pointCount == 0) {
            out += "-";
        } else {
            for(size_t i = 0; i < info.pointCount; i++) {
                if(i) out += ',';
                out += std::to_string(r.points[i].value());
            }
        }
        // Seeds are the record of which branch the user was shown, so they are
        // written even when they are still zero.
        out += " seeds=";
        if(info.ownParamCount == 0) {
            out += "-";
        } else {
            for(size_t i = 0; i < info.ownParamCount; i++) {
                if(i) out += ',';
                out += number(r.seeds[i]);
            }
        }
        out += "\n";
    }
    for(const ConstraintRecord &r : doc.constraints().records()) {
        const ConstraintKindInfo &info = constraintInfo(r.kind);
        out += "constraint " + std::to_string(r.id.value()) + " kind=" + std::string(info.name) +
               " driving=" + (r.driving ? "1" : "0") + " operands=";
        for(size_t i = 0; i < info.operandCount; i++) {
            if(i) out += ',';
            out += std::to_string(r.operands[i].value());
        }
        if(info.valueArity == 1) out += " value=" + writeSlot(r.value);
        out += "\n";
    }
    for(const RegionRecord &r : doc.regions().records()) {
        out += "region " + std::to_string(r.id.value()) +
               " style=" + std::to_string(r.style.value()) +
               " layer=" + std::to_string(r.layer.value()) +
               " boundary=" + idList(r.boundary) + "\n";
    }
    for(const TagRecord &r : doc.tags().records()) {
        out += "tag " + std::to_string(r.id.value()) +
               " kind=" + std::string(tagKindName(r.kind)) +
               " entities=" + idList(r.entities) +
               " constraints=" + idList(r.constraints) + "\n";
    }
    for(const GroupRecord &r : doc.groups().records()) {
        out += "group " + std::to_string(r.id.value()) + " name=" + quote(r.name) +
               " members=" + idList(r.members) + "\n";
    }

    // Anything a newer version wrote that this build could not read, verbatim
    // and in the order it arrived. Emitted last so the known sections stay
    // stably ordered; a file therefore reaches a byte fixed point after one
    // save rather than shuffling on every one.
    for(const std::string &line : doc.unknownRecords()) out += line + "\n";

    return out;
}

// ---------------------------------------------------------------------------
// Deserialize
// ---------------------------------------------------------------------------

LoadResult deserialize(std::string_view text, Document &out) {
    Document doc;
    auto fail = [](std::string message, size_t line) {
        LoadResult r;
        r.error = std::move(message);
        r.line = line;
        return r;
    };

    std::vector<std::string_view> lines;
    size_t start = 0;
    while(start <= text.size()) {
        const size_t nl = text.find('\n', start);
        if(nl == std::string_view::npos) {
            if(start < text.size()) lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    if(lines.empty()) return fail("empty document", 0);

    const Fields header = split(lines[0]);
    if(header.kind != "paroculus") return fail("not a paroculus document", 1);
    if(header.tokens.empty()) return fail("missing format version", 1);
    const std::optional<uint32_t> version = toUint(header.tokens.front());
    if(!version) return fail("malformed format version", 1);
    // A newer major version is refused rather than half-read. Unknown *records*
    // survive; an unknown *format* is not something this build can claim to
    // understand well enough to write back.
    if(static_cast<int>(*version) > FORMAT_VERSION) {
        return fail("document written by a newer version", 1);
    }

    for(size_t i = 1; i < lines.size(); i++) {
        const std::string_view line = lines[i];
        if(line.empty()) continue;
        const Fields f = split(line);
        const size_t lineNumber = i + 1;

        auto requireId = [&](const Fields &fields) -> std::optional<uint32_t> {
            if(fields.tokens.empty()) return std::nullopt;
            return toUint(fields.tokens.front());
        };

        if(f.kind == "watermark") {
            auto set = [&](const char *key, auto &table) {
                const auto v = field(f, key);
                if(v) {
                    const auto n = toUint(*v);
                    if(n) table.allocator().setNext(*n);
                }
            };
            set("entity", DocumentLoader::entities(doc));
            set("constraint", DocumentLoader::constraints(doc));
            set("region", DocumentLoader::regions(doc));
            set("tag", DocumentLoader::tags(doc));
            set("style", DocumentLoader::styles(doc));
            set("layer", DocumentLoader::layers(doc));
            set("group", DocumentLoader::groups(doc));
            set("parameter", DocumentLoader::parameters(doc));

        } else if(f.kind == "layer") {
            const auto id = requireId(f);
            if(!id) return fail("layer without an id", lineNumber);
            LayerRecord r;
            r.id = LayerId(*id);
            if(const auto v = field(f, "name")) r.name = unquote(*v);
            if(const auto v = field(f, "order")) {
                const auto n = toInt(*v);
                if(!n) return fail("malformed layer order", lineNumber);
                r.order = *n;
            }
            if(const auto v = field(f, "visible")) r.visible = (*v == "1");
            if(const auto v = field(f, "locked")) r.locked = (*v == "1");
            if(!DocumentLoader::layers(doc).addAt(std::move(r))) {
                return fail("duplicate layer id", lineNumber);
            }

        } else if(f.kind == "style") {
            const auto id = requireId(f);
            if(!id) return fail("style without an id", lineNumber);
            StyleRecord r;
            r.id = StyleId(*id);
            if(const auto v = field(f, "name")) r.name = unquote(*v);
            if(const auto v = field(f, "stroke-width")) {
                const auto s = parseSlot(*v);
                if(!s) return fail("malformed stroke width", lineNumber);
                r.strokeWidth = *s;
            }
            if(const auto v = field(f, "stroke")) r.strokeColor = toUint(*v).value_or(0);
            if(const auto v = field(f, "fill")) r.fillColor = toUint(*v).value_or(0);
            if(const auto v = field(f, "filled")) r.filled = (*v == "1");
            if(!DocumentLoader::styles(doc).addAt(std::move(r))) {
                return fail("duplicate style id", lineNumber);
            }

        } else if(f.kind == "parameter") {
            const auto id = requireId(f);
            if(!id) return fail("parameter without an id", lineNumber);
            ParameterRecord r;
            r.id = ParameterId(*id);
            if(const auto v = field(f, "name")) r.name = unquote(*v);
            if(const auto v = field(f, "value")) {
                const auto s = parseSlot(*v);
                if(!s) return fail("malformed parameter value", lineNumber);
                r.value = *s;
            }
            if(!DocumentLoader::parameters(doc).addAt(std::move(r))) {
                return fail("duplicate parameter id", lineNumber);
            }

        } else if(f.kind == "entity") {
            const auto id = requireId(f);
            if(!id) return fail("entity without an id", lineNumber);
            EntityRecord r;
            r.id = EntityId(*id);

            const auto kindText = field(f, "kind");
            if(!kindText) return fail("entity without a kind", lineNumber);
            const auto kind = entityKindFromName(*kindText);
            if(!kind) return fail("unknown entity kind", lineNumber);
            r.kind = *kind;

            if(const auto v = field(f, "role")) {
                r.role = (*v == "construction") ? Role::Construction : Role::Normal;
            }
            if(const auto v = field(f, "layer")) r.layer = LayerId(toUint(*v).value_or(0));

            const EntityKindInfo &info = entityInfo(r.kind);
            if(const auto v = field(f, "points")) {
                const auto list = parseIdList<EntityId>(*v);
                if(!list || list->size() != info.pointCount) {
                    return fail("entity point count does not match its kind", lineNumber);
                }
                for(size_t k = 0; k < list->size(); k++) r.points[k] = (*list)[k];
            }
            if(const auto v = field(f, "seeds")) {
                if(*v != "-") {
                    size_t pos = 0, index = 0;
                    while(pos <= v->size() && index < info.ownParamCount) {
                        const size_t comma = v->find(',', pos);
                        const auto value = toDouble(v->substr(
                            pos, comma == std::string_view::npos ? std::string_view::npos
                                                                 : comma - pos));
                        if(!value) return fail("malformed seed", lineNumber);
                        r.seeds[index++] = *value;
                        if(comma == std::string_view::npos) break;
                        pos = comma + 1;
                    }
                }
            }
            if(!DocumentLoader::entities(doc).addAt(std::move(r))) {
                return fail("duplicate entity id", lineNumber);
            }

        } else if(f.kind == "constraint") {
            const auto id = requireId(f);
            if(!id) return fail("constraint without an id", lineNumber);
            ConstraintRecord r;
            r.id = ConstraintId(*id);

            const auto kindText = field(f, "kind");
            if(!kindText) return fail("constraint without a kind", lineNumber);
            const auto kind = constraintKindFromName(*kindText);
            if(!kind) return fail("unknown constraint kind", lineNumber);
            r.kind = *kind;

            if(const auto v = field(f, "driving")) r.driving = (*v == "1");

            const ConstraintKindInfo &info = constraintInfo(r.kind);
            const auto operands = field(f, "operands");
            if(!operands) return fail("constraint without operands", lineNumber);
            const auto list = parseIdList<EntityId>(*operands);
            if(!list || list->size() != info.operandCount) {
                return fail("constraint operand count does not match its kind", lineNumber);
            }
            for(size_t k = 0; k < list->size(); k++) r.operands[k] = (*list)[k];

            if(const auto v = field(f, "value")) {
                const auto s = parseSlot(*v);
                if(!s) return fail("malformed constraint value", lineNumber);
                r.value = *s;
            }
            if(!DocumentLoader::constraints(doc).addAt(std::move(r))) {
                return fail("duplicate constraint id", lineNumber);
            }

        } else if(f.kind == "region") {
            const auto id = requireId(f);
            if(!id) return fail("region without an id", lineNumber);
            RegionRecord r;
            r.id = RegionId(*id);
            if(const auto v = field(f, "style")) r.style = StyleId(toUint(*v).value_or(0));
            if(const auto v = field(f, "layer")) r.layer = LayerId(toUint(*v).value_or(0));
            if(const auto v = field(f, "boundary")) {
                const auto list = parseIdList<EntityId>(*v);
                if(!list) return fail("malformed region boundary", lineNumber);
                r.boundary = *list;
            }
            if(!DocumentLoader::regions(doc).addAt(std::move(r))) {
                return fail("duplicate region id", lineNumber);
            }

        } else if(f.kind == "tag") {
            const auto id = requireId(f);
            if(!id) return fail("tag without an id", lineNumber);
            TagRecord r;
            r.id = TagId(*id);
            if(const auto v = field(f, "kind")) {
                r.kind = *v == "distribution" ? TagKind::Distribution
                         : *v == "mirror"     ? TagKind::Mirror
                                              : TagKind::Rectangle;
            }
            if(const auto v = field(f, "entities")) {
                const auto list = parseIdList<EntityId>(*v);
                if(!list) return fail("malformed tag entities", lineNumber);
                r.entities = *list;
            }
            if(const auto v = field(f, "constraints")) {
                const auto list = parseIdList<ConstraintId>(*v);
                if(!list) return fail("malformed tag constraints", lineNumber);
                r.constraints = *list;
            }
            if(!DocumentLoader::tags(doc).addAt(std::move(r))) {
                return fail("duplicate tag id", lineNumber);
            }

        } else if(f.kind == "group") {
            const auto id = requireId(f);
            if(!id) return fail("group without an id", lineNumber);
            GroupRecord r;
            r.id = GroupId(*id);
            if(const auto v = field(f, "name")) r.name = unquote(*v);
            if(const auto v = field(f, "members")) {
                const auto list = parseIdList<EntityId>(*v);
                if(!list) return fail("malformed group members", lineNumber);
                r.members = *list;
            }
            if(!DocumentLoader::groups(doc).addAt(std::move(r))) {
                return fail("duplicate group id", lineNumber);
            }

        } else {
            // A record kind this build does not know. Kept verbatim rather than
            // dropped, so an older install saving a newer file does not
            // silently truncate it.
            DocumentLoader::unknown(doc).emplace_back(line);
        }
    }

    // Validate once, over the finished document, because a file may legally
    // mention an entity before defining it and per-record validation would
    // reject that ordering rather than the actual corruption it is looking for.
    for(const EntityRecord &r : doc.entities().records()) {
        if(doc.validate(r) != CommandError::None) {
            return fail("entity " + std::to_string(r.id.value()) + " is not valid", 0);
        }
    }
    for(const ConstraintRecord &r : doc.constraints().records()) {
        if(doc.validate(r) != CommandError::None) {
            return fail("constraint " + std::to_string(r.id.value()) + " is not valid", 0);
        }
    }
    for(const RegionRecord &r : doc.regions().records()) {
        if(doc.validate(r) != CommandError::None) {
            return fail("region " + std::to_string(r.id.value()) + " is not valid", 0);
        }
    }
    for(const TagRecord &r : doc.tags().records()) {
        if(doc.validate(r) != CommandError::None) {
            return fail("tag " + std::to_string(r.id.value()) + " is not valid", 0);
        }
    }
    for(const GroupRecord &r : doc.groups().records()) {
        if(doc.validate(r) != CommandError::None) {
            return fail("group " + std::to_string(r.id.value()) + " is not valid", 0);
        }
    }
    for(const StyleRecord &r : doc.styles().records()) {
        if(doc.validate(r) != CommandError::None) {
            return fail("style " + std::to_string(r.id.value()) + " is not valid", 0);
        }
    }
    // Covers the cycle check and the reference check together, so a parameter
    // naming a parameter that is not in the file is refused here rather than
    // evaluating to nullopt for the rest of the session.
    for(const ParameterRecord &r : doc.parameters().records()) {
        if(doc.validate(r) != CommandError::None) {
            return fail("parameter " + std::to_string(r.id.value()) + " is not valid", 0);
        }
    }

    out = std::move(doc);
    LoadResult ok;
    ok.ok = true;
    return ok;
}

}  // namespace paroculus
