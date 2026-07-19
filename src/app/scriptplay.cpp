#include "app/scriptplay.h"

#include <fstream>
#include <optional>
#include <sstream>

namespace paroculus {
namespace {

std::optional<GestureScript> pending;
std::string recordPath;

}  // namespace

bool saveScriptFile(const std::string &path, const GestureScript &script, std::string &error) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if(!file) {
        error = "cannot write " + path;
        return false;
    }
    file << serializeScript(script);
    if(!file) {
        error = "failed writing " + path;
        return false;
    }
    return true;
}

bool loadScriptFile(const std::string &path, GestureScript &out, std::string &error) {
    std::ifstream file(path, std::ios::binary);
    if(!file) {
        error = "cannot open " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();

    const ScriptLoadResult result = parseScript(buffer.str(), out);
    if(!result) {
        error = path;
        if(result.line > 0) error += ":" + std::to_string(result.line);
        error += ": " + result.error;
        return false;
    }
    return true;
}

namespace pendingScript {

void set(GestureScript script) { pending = std::move(script); }

bool take(GestureScript &out) {
    if(!pending) return false;
    out = std::move(*pending);
    pending.reset();
    return true;
}

void setRecordPath(std::string path) { recordPath = std::move(path); }

std::string takeRecordPath() {
    std::string out;
    out.swap(recordPath);
    return out;
}

}  // namespace pendingScript

}  // namespace paroculus
