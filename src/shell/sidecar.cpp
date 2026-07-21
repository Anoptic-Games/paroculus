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
        // "view" is the only content line at version 0. The header line and any
        // line this build does not know are skipped, which is how a version-0
        // reader survives the additive fields U2 writes above it: they parse as
        // unknown keys and are left at their defaults.
        if(key == "view") {
            std::string x, y, z;
            words >> x >> y >> z;
            readDouble(x, out.panX);
            readDouble(y, out.panY);
            readDouble(z, out.zoom);
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
