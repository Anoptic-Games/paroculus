#include "interact/numeric.h"

namespace paroculus {

void NumericEntry::begin(size_t target) {
    active_ = true;
    target_ = target;
    text_.clear();
}

void NumericEntry::retarget(size_t target) {
    target_ = target;
    text_.clear();
}

void NumericEntry::type(char c) {
    if(!active_) return;
    // Anything the length grammar can contain. Filtering here rather than at
    // parse time keeps the field from accumulating characters that could never
    // become a number, which is what makes backspace predictable.
    const bool digit = c >= '0' && c <= '9';
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if(!digit && !letter && c != '.' && c != '-' && c != ' ') return;
    text_ += c;
}

void NumericEntry::backspace() {
    if(!active_ || text_.empty()) return;
    text_.pop_back();
}

void NumericEntry::cancel() {
    active_ = false;
    text_.clear();
}

std::optional<double> NumericEntry::value() const {
    if(!active_ || text_.empty()) return std::nullopt;
    // Millimetres is the storage unit, so a bare number is already stored form.
    // The suffix path is the only place display units exist at all.
    const std::optional<ParsedLength> parsed = parseLength(text_, Unit::Millimetre);
    if(!parsed) return std::nullopt;
    return parsed->millimetres;
}

}  // namespace paroculus
