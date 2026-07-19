// The document format, version 0.
//
// The file is the declaration layer plus seeds plus roles and tags: authoring
// intent and branch choice, nothing rebuildable. No tessellations, no spatial
// indexes, no component partition — all of those are derived and would only rot
// on disk.
//
// Four properties the format is built to hold, all of them tested:
//
//   versioned      from the first write, so a migration policy has something to
//                  migrate from. The freeze is stage 8; churn before then is
//                  expected and regenerates the corpus rather than growing
//                  shims.
//   stably ordered records come out in ID order every time, so diffs are
//                  readable and merges are sane, and so byte-stability is a
//                  property rather than an accident of hash iteration.
//   lossless       doubles round-trip exactly. Display rounding lives at the
//                  presentation boundary and never reaches storage.
//   forward-safe   record kinds this build does not understand survive a
//                  round-trip verbatim. Otherwise every save from an older
//                  install silently truncates a newer file.
#pragma once

#include <string>
#include <string_view>

#include "core/document.h"

namespace paroculus {

inline constexpr int FORMAT_VERSION = 0;

// Returns the document as text. Deterministic: the same document serializes
// byte-identically on every machine and every run.
std::string serialize(const Document &doc);

struct LoadResult {
    bool ok = false;
    std::string error;
    size_t line = 0;  // 1-based, 0 when the failure is not line-specific

    explicit operator bool() const { return ok; }
};

// Replaces `out` wholesale. On failure `out` is left empty rather than
// half-populated, because a partially loaded document is a corrupt one wearing
// a valid document's interface.
LoadResult deserialize(std::string_view text, Document &out);

}  // namespace paroculus
