#include "core/expression.h"

#include <array>
#include <cctype>
#include <charconv>

namespace paroculus {
namespace {

// A hand-written recursive descent over a cursor, the smallest thing that gives
// precedence and parentheses. The grammar:
//
//   expr    := term (('+'|'-') term)*
//   term    := factor (('*'|'/') factor)*
//   factor  := '-' factor | primary
//   primary := number | identifier | '(' expr ')'
// The deepest nesting the parser will descend, past which it refuses rather than
// overflows the stack. Text arrives from panel paste and hand-edited scripts, so
// this is the one new untrusted-input path and it terminates on any input, the
// same reason TableParameterEnv carries a depth bound and persist checks its node
// indices. Well past any expression a person writes; a wall against a pathology.
constexpr int kMaxDepth = 256;

struct Parser {
    std::string_view s;
    const ParameterTable &table;
    size_t i = 0;
    bool failed = false;
    int depth = 0;

    // Bounds recursion at the two points it re-enters — a parenthesised
    // sub-expression and a chain of unary minus — decrementing on every return
    // path so a wide but shallow expression is not miscounted as a deep one.
    struct Guard {
        int &depth;
        bool &failed;
        Guard(int &d, bool &f) : depth(d), failed(f) {
            if(++depth > kMaxDepth) failed = true;
        }
        ~Guard() { --depth; }
    };

    void skip() {
        while(i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    }
    bool eof() {
        skip();
        return i >= s.size();
    }
    char peek() {
        skip();
        return i < s.size() ? s[i] : '\0';
    }

    std::optional<Slot> parse() {
        std::optional<Slot> out = expr();
        if(failed || !out) return std::nullopt;
        // Trailing junk means the input was not one expression, so it is refused
        // rather than read up to the first thing that stops parsing.
        if(!eof()) return std::nullopt;
        return out;
    }

    std::optional<Slot> expr() {
        const Guard guard(depth, failed);
        if(failed) return std::nullopt;
        std::optional<Slot> lhs = term();
        if(!lhs) return std::nullopt;
        for(;;) {
            const char c = peek();
            if(c != '+' && c != '-') break;
            i++;
            std::optional<Slot> rhs = term();
            if(!rhs) return std::nullopt;
            lhs = Slot::binary(c == '+' ? ExprOp::Add : ExprOp::Subtract, *lhs, *rhs);
        }
        return lhs;
    }

    std::optional<Slot> term() {
        std::optional<Slot> lhs = factor();
        if(!lhs) return std::nullopt;
        for(;;) {
            const char c = peek();
            if(c != '*' && c != '/') break;
            i++;
            std::optional<Slot> rhs = factor();
            if(!rhs) return std::nullopt;
            lhs = Slot::binary(c == '*' ? ExprOp::Multiply : ExprOp::Divide, *lhs, *rhs);
        }
        return lhs;
    }

    std::optional<Slot> factor() {
        const Guard guard(depth, failed);
        if(failed) return std::nullopt;
        if(peek() == '-') {
            i++;
            std::optional<Slot> inner = factor();
            if(!inner) return std::nullopt;
            return Slot::negate(*inner);
        }
        return primary();
    }

    std::optional<Slot> primary() {
        const char c = peek();
        if(c == '(') {
            i++;
            std::optional<Slot> inner = expr();
            if(!inner) return std::nullopt;
            if(peek() != ')') return std::nullopt;
            i++;
            return inner;
        }
        if(c == '.' || (c >= '0' && c <= '9')) return number();
        if(c == '_' || std::isalpha(static_cast<unsigned char>(c))) return identifier();
        return std::nullopt;
    }

    std::optional<Slot> number() {
        skip();
        double v = 0.0;
        const std::from_chars_result r =
            std::from_chars(s.data() + i, s.data() + s.size(), v);
        if(r.ec != std::errc{}) return std::nullopt;
        i = static_cast<size_t>(r.ptr - s.data());
        return Slot(v);
    }

    std::optional<Slot> identifier() {
        skip();
        const size_t begin = i;
        while(i < s.size() &&
              (s[i] == '_' || std::isalnum(static_cast<unsigned char>(s[i])))) {
            i++;
        }
        const std::string_view name = s.substr(begin, i - begin);
        const ParameterRecord *p = findParameterByName(table, name);
        if(p == nullptr) return std::nullopt;
        return Slot::parameter(p->id);
    }
};

std::string number(double v) {
    std::array<char, 48> buf{};
    const std::to_chars_result r = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    if(r.ec != std::errc{}) return "0";
    return std::string(buf.data(), static_cast<size_t>(r.ptr - buf.data()));
}

// Binding strength of a node's top operator, so the printer parenthesizes only
// where precedence or non-associativity would otherwise change the reading.
int precedenceOf(const std::vector<ExprNode> &nodes, uint32_t index) {
    switch(nodes[index].op) {
        case ExprOp::Add:
        case ExprOp::Subtract: return 1;
        case ExprOp::Multiply:
        case ExprOp::Divide:   return 2;
        case ExprOp::Negate:   return 3;
        case ExprOp::Constant:
        case ExprOp::Parameter: return 100;
    }
    return 100;
}

std::string formatNode(const std::vector<ExprNode> &nodes, uint32_t index,
                       const ParameterTable &table, int parentPrec, bool rightOfNonAssoc) {
    const ExprNode &n = nodes[index];
    std::string out;
    const int prec = precedenceOf(nodes, index);
    switch(n.op) {
        case ExprOp::Constant: return number(n.constant);
        case ExprOp::Parameter: {
            const ParameterRecord *p = table.find(n.parameter);
            return p != nullptr ? p->name : "?";
        }
        case ExprOp::Negate:
            out = "-" + formatNode(nodes, n.lhs, table, prec, false);
            break;
        case ExprOp::Add:
        case ExprOp::Subtract:
        case ExprOp::Multiply:
        case ExprOp::Divide: {
            const char *sym = n.op == ExprOp::Add        ? " + "
                              : n.op == ExprOp::Subtract ? " - "
                              : n.op == ExprOp::Multiply ? " * "
                                                         : " / ";
            // The right operand of a left-associative operator needs a guard when
            // it binds no tighter than its parent — a - (b - c) is not a - b - c.
            out = formatNode(nodes, n.lhs, table, prec, false) + sym +
                  formatNode(nodes, n.rhs, table, prec, true);
            break;
        }
    }
    const bool needParens = prec < parentPrec || (rightOfNonAssoc && prec == parentPrec);
    return needParens ? "(" + out + ")" : out;
}

}  // namespace

std::optional<Slot> parseExpression(std::string_view text, const ParameterTable &table) {
    Parser parser{text, table};
    if(parser.eof()) return std::nullopt;  // empty text is not an expression
    return parser.parse();
}

std::string formatExpression(const Slot &slot, const ParameterTable &table) {
    if(slot.isConstant()) return number(slot.constant());
    const std::vector<ExprNode> &nodes = slot.nodes();
    if(nodes.empty()) return number(slot.constant());
    return formatNode(nodes, static_cast<uint32_t>(nodes.size() - 1), table, 0, false);
}

}  // namespace paroculus
