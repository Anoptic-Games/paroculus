// Value cells.
//
// Every constraint value and every style property is a Slot, and a constant is
// the trivial expression. That indirection is not deferrable: typed input,
// scrubbing, dimensions, scale-the-values, expressions and keyframes all hang
// off it, and retrofitting slots under thousands of raw doubles is the
// expensive path. One indirection, six features.
//
// The v1 language is deliberately minimal — numbers, arithmetic, and references
// to named document parameters. What it deliberately cannot express is a
// measurement: a slot that needs to depend on geometry is not an expression
// reading a measurement, it is a constraint, where the feedback loop belongs to
// the solver. That exclusion is what keeps the value-dependency graph acyclic
// by construction and closes off measure-drive-measure oscillation.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "core/ids.h"

namespace paroculus {

enum class ExprOp : uint8_t {
    Constant,
    Parameter,
    Add,
    Subtract,
    Multiply,
    Divide,
    Negate,  // uses lhs only
};

// A node in a slot's expression. Operand fields index the owning slot's node
// vector; the root is the last node, so evaluation is a single forward pass.
struct ExprNode {
    ExprOp op = ExprOp::Constant;
    double constant = 0.0;
    ParameterId parameter;
    uint32_t lhs = 0;
    uint32_t rhs = 0;
};

bool operator==(const ExprNode &a, const ExprNode &b);
inline bool operator!=(const ExprNode &a, const ExprNode &b) { return !(a == b); }

// Resolves a named parameter to its value during evaluation.
// Returns nullopt when the parameter is unknown or itself unevaluable, which
// propagates as an unevaluable slot rather than a silent zero.
class ParameterEnv {
public:
    virtual ~ParameterEnv() = default;
    virtual std::optional<double> lookup(ParameterId id) const = 0;
};

// Invariant: `nodes` is empty exactly when the slot is a bare constant, which
// is the overwhelmingly common case and costs no allocation.
// Invariant: every node's lhs/rhs index strictly below its own position, so the
// expression is a DAG in evaluation order and cannot self-reference.
class Slot {
public:
    Slot() = default;
    explicit Slot(double constant) : constant_(constant) {}

    static Slot parameter(ParameterId id);

    // Builds a binary node over two sub-expressions. The operands are copied in
    // and reindexed, so a Slot is always self-contained and copyable.
    static Slot binary(ExprOp op, const Slot &lhs, const Slot &rhs);
    static Slot negate(const Slot &operand);

    // Rebuilds a slot from a node list, for the loader. The caller owns the
    // invariant that every operand index points strictly below its own node;
    // persist checks that while parsing, because a file is untrusted input.
    static Slot fromNodes(std::vector<ExprNode> nodes);

    bool isConstant() const { return nodes_.empty(); }

    // Valid only when isConstant(). The stored, full-precision value.
    double constant() const { return constant_; }

    const std::vector<ExprNode> &nodes() const { return nodes_; }

    // env: resolves parameter references. May be null when the slot is known
    // constant. Returns nullopt if a reference is unresolvable or the
    // expression divides by zero — an unevaluable slot holds its last good
    // value at the call site, it does not become zero.
    std::optional<double> evaluate(const ParameterEnv *env) const;

    // Every parameter this slot reads, in first-appearance order. The
    // acyclicity check and the dependency-invalidation walk both consume this.
    std::vector<ParameterId> references() const;

    friend bool operator==(const Slot &a, const Slot &b);
    friend bool operator!=(const Slot &a, const Slot &b) { return !(a == b); }

private:
    double constant_ = 0.0;
    std::vector<ExprNode> nodes_;
};

}  // namespace paroculus
