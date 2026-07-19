#include "core/slots.h"

#include <algorithm>

namespace paroculus {
namespace {

// Copies one slot's expression onto the end of `out`, reindexing its operand
// references, and returns the index its root landed at. A bare constant
// materialises as a single Constant node, which is how the empty-nodes
// canonical form for constants survives composition.
uint32_t appendInto(std::vector<ExprNode> &out, const Slot &s) {
    if(s.isConstant()) {
        ExprNode n;
        n.op = ExprOp::Constant;
        n.constant = s.constant();
        out.push_back(n);
        return static_cast<uint32_t>(out.size() - 1);
    }
    const auto base = static_cast<uint32_t>(out.size());
    for(ExprNode n : s.nodes()) {
        switch(n.op) {
            case ExprOp::Constant:
            case ExprOp::Parameter:
                break;
            case ExprOp::Negate:
                n.lhs += base;
                break;
            default:
                n.lhs += base;
                n.rhs += base;
                break;
        }
        out.push_back(n);
    }
    return static_cast<uint32_t>(out.size() - 1);
}

}  // namespace

bool operator==(const ExprNode &a, const ExprNode &b) {
    return a.op == b.op && a.constant == b.constant && a.parameter == b.parameter &&
           a.lhs == b.lhs && a.rhs == b.rhs;
}

Slot Slot::parameter(ParameterId id) {
    Slot s;
    ExprNode n;
    n.op = ExprOp::Parameter;
    n.parameter = id;
    s.nodes_.push_back(n);
    return s;
}

Slot Slot::fromNodes(std::vector<ExprNode> nodes) {
    Slot s;
    s.nodes_ = std::move(nodes);
    return s;
}

Slot Slot::binary(ExprOp op, const Slot &lhs, const Slot &rhs) {
    Slot s;
    ExprNode n;
    n.op = op;
    n.lhs = appendInto(s.nodes_, lhs);
    n.rhs = appendInto(s.nodes_, rhs);
    s.nodes_.push_back(n);
    return s;
}

Slot Slot::negate(const Slot &operand) {
    Slot s;
    ExprNode n;
    n.op = ExprOp::Negate;
    n.lhs = appendInto(s.nodes_, operand);
    s.nodes_.push_back(n);
    return s;
}

// Nodes are stored in evaluation order with operands strictly below, so one
// forward pass suffices and no recursion or visited set is needed.
std::optional<double> Slot::evaluate(const ParameterEnv *env) const {
    if(isConstant()) return constant_;

    std::vector<double> value(nodes_.size(), 0.0);
    for(size_t i = 0; i < nodes_.size(); i++) {
        const ExprNode &n = nodes_[i];
        switch(n.op) {
            case ExprOp::Constant:
                value[i] = n.constant;
                break;
            case ExprOp::Parameter: {
                if(env == nullptr) return std::nullopt;
                const std::optional<double> v = env->lookup(n.parameter);
                if(!v) return std::nullopt;
                value[i] = *v;
                break;
            }
            case ExprOp::Add:
                value[i] = value[n.lhs] + value[n.rhs];
                break;
            case ExprOp::Subtract:
                value[i] = value[n.lhs] - value[n.rhs];
                break;
            case ExprOp::Multiply:
                value[i] = value[n.lhs] * value[n.rhs];
                break;
            case ExprOp::Divide:
                if(value[n.rhs] == 0.0) return std::nullopt;
                value[i] = value[n.lhs] / value[n.rhs];
                break;
            case ExprOp::Negate:
                value[i] = -value[n.lhs];
                break;
        }
    }
    return value.back();
}

std::vector<ParameterId> Slot::references() const {
    std::vector<ParameterId> out;
    for(const ExprNode &n : nodes_) {
        if(n.op != ExprOp::Parameter) continue;
        if(std::find(out.begin(), out.end(), n.parameter) == out.end()) {
            out.push_back(n.parameter);
        }
    }
    return out;
}

// Structural, not semantic: 2+3 does not equal 5. Undo byte-identity and
// persist round-tripping both want the authored form preserved exactly, and
// v1 does no constant folding, so structural equality is the honest one.
bool operator==(const Slot &a, const Slot &b) {
    if(a.nodes_.empty() != b.nodes_.empty()) return false;
    if(a.nodes_.empty()) return a.constant_ == b.constant_;
    return a.nodes_ == b.nodes_;
}

}  // namespace paroculus
