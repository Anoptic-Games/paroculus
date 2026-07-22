// Infix text ⟷ Slot.
//
// A Slot is stored as a node DAG (core/slots.h) and serialized to persist's node
// form, neither of which a person types. The value editors — the parameters panel
// and the style toolbar — want the ordinary infix a user writes: `3`, `gutter`,
// `gutter * 2`, `(a + b) / 2`. This is the one place that grammar lives, so a
// panel that reads a value and a session method that commits one share it rather
// than growing two parsers that drift.
//
// The language is exactly slots.h's: numbers, the four arithmetic operators with
// the conventional precedence, parentheses, unary minus, and references to named
// document parameters. It deliberately cannot express a measurement, which is
// what keeps the value graph acyclic — the same exclusion slots.h states.
//
// A reference is a parameter name resolved against the table at parse time, so a
// name the document does not hold makes the whole expression unparseable rather
// than a silent zero — the same discipline evaluate() follows for an unresolvable
// slot. Identifiers are [A-Za-z_][A-Za-z0-9_]*; a parameter whose name falls
// outside that set is not referenceable by expression, which is a naming
// restriction the panel surfaces rather than a parse that guesses.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/parameters.h"
#include "core/slots.h"

namespace paroculus {

// Parses infix text into a Slot, resolving identifiers against `table`. Returns
// nullopt for malformed input, a divide-token with no operand, or a name the
// table does not hold — never a partial or coerced result.
std::optional<Slot> parseExpression(std::string_view text, const ParameterTable &table);

// Renders a Slot back to infix, naming parameters from `table`. The inverse of
// parseExpression up to whitespace and redundant parentheses: re-parsing the
// output yields a slot that evaluates identically. Display only — record and
// replay carry the user's verbatim text, never this rendering — so a parameter
// whose id is gone from the table renders as "?" rather than failing.
std::string formatExpression(const Slot &slot, const ParameterTable &table);

}  // namespace paroculus
