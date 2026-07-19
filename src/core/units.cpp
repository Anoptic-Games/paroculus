#include "core/units.h"

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace paroculus {
namespace {

struct UnitInfo {
    Unit unit;
    std::string_view suffix;
    double millimetres;
};

// One table, three projections: the scale factor, the emitted token, and the
// accepted token. Order matches the enum so lookup is an index.
constexpr std::array<UnitInfo, 5> UNITS = {{
    {Unit::Millimetre, "mm", 1.0},
    {Unit::Centimetre, "cm", 10.0},
    {Unit::Metre, "m", 1000.0},
    {Unit::Inch, "in", 25.4},
    {Unit::Point, "pt", 25.4 / 72.0},
}};

constexpr const UnitInfo &info(Unit u) { return UNITS[static_cast<size_t>(u)]; }

std::string_view trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while(b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while(e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

}  // namespace

double millimetresPer(Unit u) { return info(u).millimetres; }

std::string_view unitSuffix(Unit u) { return info(u).suffix; }

std::optional<Unit> unitFromSuffix(std::string_view text) {
    for(const UnitInfo &i : UNITS) {
        if(i.suffix == text) return i.unit;
    }
    return std::nullopt;
}

std::optional<ParsedLength> parseLength(std::string_view text, Unit fallback) {
    const std::string_view s = trim(text);
    if(s.empty()) return std::nullopt;

    double value = 0.0;
    const char *begin = s.data();
    const char *end = s.data() + s.size();
    const std::from_chars_result r = std::from_chars(begin, end, value);
    if(r.ec != std::errc{}) return std::nullopt;
    if(!std::isfinite(value)) return std::nullopt;

    const std::string_view rest = trim(s.substr(static_cast<size_t>(r.ptr - begin)));
    if(rest.empty()) return ParsedLength{value * millimetresPer(fallback), false};

    const std::optional<Unit> u = unitFromSuffix(rest);
    if(!u) return std::nullopt;
    return ParsedLength{value * millimetresPer(*u), true};
}

std::string formatLength(double millimetres, Unit u, int decimals) {
    if(decimals < 0) decimals = 0;
    const double value = millimetres / millimetresPer(u);

    std::array<char, 64> buf{};
    const int n = std::snprintf(buf.data(), buf.size(), "%.*f", decimals, value);
    if(n <= 0) return std::string(unitSuffix(u));

    std::string out(buf.data(), static_cast<size_t>(n));
    // "-0.00" is a rounding artefact, never a value the user meant.
    if(out.find_first_not_of("-0.") == std::string::npos && out.front() == '-') {
        out.erase(out.begin());
    }
    out += unitSuffix(u);
    return out;
}

}  // namespace paroculus
