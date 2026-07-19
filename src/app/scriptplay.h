// `--script` playback.
//
// The corpus can only assert what someone thought to assert. Watching a
// recorded session is the other instrument: a state can satisfy every invariant
// on the list and still be visibly wrong — the stage 3 branch flip satisfied
// parallelism, the length ratio and every tolerance, and was still a segment
// jumping to the wrong side of the sketch. Nothing in CI caught it; a person
// looking caught it immediately.
//
// Playback is deliberately faithful rather than convenient: the script's own
// viewport steps are applied, so what plays back is the session as recorded and
// not the session as this window would have framed it.
#pragma once

#include <string>

#include "interact/script.h"

namespace paroculus {

// Reads and parses a script file. On failure returns false and fills `error`.
bool loadScriptFile(const std::string &path, GestureScript &out, std::string &error);

// Writes a script to a file. Returns false and fills `error` on failure.
bool saveScriptFile(const std::string &path, const GestureScript &script, std::string &error);

// The script the shell should play once it has a view, if --script was given,
// and the path it should record to, if --record was.
//
// A process-level hand-off because the view is instantiated by QML, which does
// not carry argv. Deliberately not a general channel: one script, taken once.
namespace pendingScript {

void set(GestureScript script);
// Moves the pending script into `out`. Returns false when none was set.
bool take(GestureScript &out);

void setRecordPath(std::string path);
// Returns the path and forgets it, or an empty string when none was set.
std::string takeRecordPath();

}  // namespace pendingScript

}  // namespace paroculus
