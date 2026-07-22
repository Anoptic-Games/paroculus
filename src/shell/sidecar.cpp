#include "shell/sidecar.h"

#include <charconv>
#include <fstream>
#include <sstream>

namespace paroculus {
namespace {

// The document format's number rule, applied here: shortest round-trippable form
// through to_chars, and from_chars to read, both locale-fixed to '.'. A view
// preference is not the storage of record for anything, but writing it with the
// printf family would still make it a file that fails to read back where the
// locale prints a decimal comma, and the fix costs nothing.
void writeDouble(std::string &out, double value) {
    char buffer[32];
    const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
}

// Parses a double, leaving `value` untouched and returning false when the token
// is not a number. A malformed token keeps the caller's default rather than
// coercing to zero, because the sidecar is droppable field by field.
bool readDouble(std::string_view token, double &value) {
    const std::from_chars_result result =
        std::from_chars(token.data(), token.data() + token.size(), value);
    return result.ec == std::errc() && result.ptr == token.data() + token.size();
}

// An unsigned integer, for the packed background colour. Integer to_chars is
// locale-independent by definition, so there is no comma-decimal hazard, but the
// rule is the same as the double one: parse-or-keep-the-default.
void writeUint(std::string &out, uint32_t value) {
    char buffer[16];
    const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
}

bool readUint(std::string_view token, uint32_t &value) {
    const std::from_chars_result result =
        std::from_chars(token.data(), token.data() + token.size(), value);
    return result.ec == std::errc() && result.ptr == token.data() + token.size();
}

// A boolean field, written as 0 or 1 and read leniently: any non-"0" that parses
// as an integer is true, and a malformed token keeps the default.
bool readBool(std::string_view token, bool &value) {
    uint32_t n = 0;
    if(!readUint(token, n)) return false;
    value = n != 0;
    return true;
}

}  // namespace

std::string sidecarPathFor(std::string_view documentPath) {
    static constexpr std::string_view ext = ".paro";
    if(documentPath.size() >= ext.size() &&
       documentPath.substr(documentPath.size() - ext.size()) == ext) {
        return std::string(documentPath.substr(0, documentPath.size() - ext.size())) +
               ".paro-view";
    }
    return std::string(documentPath) + ".paro-view";
}

std::string writeSidecar(const Sidecar &sidecar) {
    std::string out = "paro-view 0\n";
    out += "view ";
    writeDouble(out, sidecar.panX);
    out += ' ';
    writeDouble(out, sidecar.panY);
    out += ' ';
    writeDouble(out, sidecar.zoom);
    out += '\n';
    out += "background ";
    writeUint(out, sidecar.background);
    out += '\n';
    out += "frames ";
    writeUint(out, sidecar.showAllFrames ? 1 : 0);
    out += '\n';
    out += "extensions ";
    writeUint(out, sidecar.extensions ? 1 : 0);
    out += '\n';
    out += "grid ";
    writeUint(out, sidecar.gridVisible ? 1 : 0);
    out += '\n';
    return out;
}

Sidecar readSidecar(std::string_view text) {
    Sidecar out;
    std::istringstream stream{std::string(text)};
    std::string line;
    while(std::getline(stream, line)) {
        std::istringstream words(line);
        std::string key;
        if(!(words >> key)) continue;
        // "view" is the version-0 line; the rest were added at U2. The header
        // line and any line a build does not know are skipped, which is how a
        // reader that predates a field survives it and how a newer field with an
        // unparseable value costs that field and no more.
        if(key == "view") {
            std::string x, y, z;
            words >> x >> y >> z;
            readDouble(x, out.panX);
            readDouble(y, out.panY);
            readDouble(z, out.zoom);
        } else if(key == "background") {
            std::string v;
            words >> v;
            readUint(v, out.background);
        } else if(key == "frames") {
            std::string v;
            words >> v;
            readBool(v, out.showAllFrames);
        } else if(key == "extensions") {
            std::string v;
            words >> v;
            readBool(v, out.extensions);
        } else if(key == "grid") {
            std::string v;
            words >> v;
            readBool(v, out.gridVisible);
        }
    }
    return out;
}

Sidecar loadSidecar(const std::string &path) {
    std::ifstream in(path);
    if(!in) return Sidecar{};
    std::stringstream buffer;
    buffer << in.rdbuf();
    return readSidecar(buffer.str());
}

bool saveSidecar(const std::string &path, const Sidecar &sidecar) {
    std::ofstream out(path);
    if(!out) return false;
    out << writeSidecar(sidecar);
    out.close();
    return static_cast<bool>(out);
}

}  // namespace paroculus
