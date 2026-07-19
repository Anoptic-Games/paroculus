// The one conversion boundary between stored values and displayed values.
//
// Document storage is millimetres, always, and every number above this file is
// document-absolute and zoom-independent. Units exist at presentation only:
// parse converts inbound text to millimetres, format converts millimetres to
// outbound text, and nothing in between ever holds a value in display units.
//
// The hygiene rule this file enforces: display rounding never round-trips into
// stored values. format() is lossy by design, so an edit session must open on
// the stored value, not on the string that was rendered from it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace paroculus {

enum class Unit : uint8_t {
    Millimetre,
    Centimetre,
    Metre,
    Inch,
    Point,  // typographic, 1/72 inch
};

// u: any unit. Returns how many millimetres one of u is. Always > 0.
double millimetresPer(Unit u);

// u: any unit. Returns the token parse() accepts and format() emits.
std::string_view unitSuffix(Unit u);

// text: a suffix token, case-sensitive, no surrounding space.
// Returns the unit, or nullopt if the token names none.
std::optional<Unit> unitFromSuffix(std::string_view text);

struct ParsedLength {
    double millimetres = 0.0;
    // False when the text carried no unit token and `fallback` supplied it.
    // The caller needs this to decide whether a typed value pins a unit.
    bool hadSuffix = false;
};

// Parses a length in the v1 input language: optional sign, decimal number,
// optional unit suffix. Surrounding whitespace is ignored; internal whitespace
// between number and suffix is allowed.
// text: the raw field contents. fallback: the unit a bare number is read in.
// Returns nullopt on anything it cannot parse in full — trailing garbage is a
// parse failure, not a truncation, because silently accepting "12mmm" as 12mm
// is how a typo becomes a wrong document.
std::optional<ParsedLength> parseLength(std::string_view text, Unit fallback);

// millimetres: a stored value. u: the display unit. decimals: >= 0.
// Returns the value for display. Lossy: never feed the result back into
// storage, and never re-parse it to obtain the value you started with.
std::string formatLength(double millimetres, Unit u, int decimals);

}  // namespace paroculus
