// The per-document presentation sidecar, version 0.
//
// One of the spec's three settings tiers: per-document presentation that must
// never reach document bytes. It rides beside the document as name.paro-view and
// is droppable by design — a missing or malformed sidecar costs view preferences
// and nothing else. That droppability is load-bearing: it is what makes the
// determinism property hold, that opening a document with or without its sidecar
// and editing identically produces byte-identical saves.
//
// Deliberately toolkit-free and a plain data struct rather than a QObject: the
// shell converts between it and a workspace's ViewState, and a headless test
// round-trips it without a window. The lexical rules are persist's — std::to_chars,
// never printf — for the one reason they are everywhere in this codebase: Qt calls
// setlocale on Unix, so a "%g" write produces 1,5 on half of Europe and the reader
// refuses the file it just wrote. Serialization is machine-independent or it is
// not serialization.
//
// At this stage the sidecar carries the view framing only. Background color, the
// axis and extension toggles and the grid-display preference are the same file's
// business and land in it at U2, each an additive field a version-0 reader skips.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace paroculus {

struct Sidecar {
    // The view framing the user controls: pan in screen pixels and zoom, layered
    // over a fitted base. The base is re-derived from the pose on open and never
    // stored — the document is identical on reopen, so the base fits identically
    // and pan and zoom restore the exact view. Storing the base would be storing
    // a function of the document beside the document, which is the rot the format
    // refuses in the large.
    double panX = 0.0;
    double panY = 0.0;
    double zoom = 1.0;

    // The U2 presentation preferences, each an additive line a version-0 reader
    // skips. None reaches document bytes — that is the whole point of the tier —
    // so the determinism property holds however they are set.

    // The canvas background, packed 0xAARRGGBB, or zero for the theme default.
    // Zero is the sentinel because a real background is opaque.
    uint32_t background = 0;
    // The axis all-frames toggle: draw every reference frame, not only the
    // selection's, for orientation-heavy work.
    bool showAllFrames = false;
    // The line-extension overlay: every segment's carrier extended to the
    // viewport edges, with the direction-class count in the HUD.
    bool extensions = false;
    // Whether the placement grid is drawn. Separate from grid snapping, which is
    // recorded session state — the two share the step, so the drawn grid can
    // never lie about where a click lands, but showing it and snapping to it are
    // different questions.
    bool gridVisible = true;
};

// The sidecar path for a document path. A ".paro" extension becomes ".paro-view"
// — the document's own name with -view appended to its extension — and anything
// else gets ".paro-view" appended whole, so an unsaved or non-.paro workspace
// still has a well-defined sidecar name. Not injective in the pathological case
// of an extensionless "drawing" beside "drawing.paro" in one directory, which is
// harmless: the sidecar is droppable, so at worst a stale view preference loads.
std::string sidecarPathFor(std::string_view documentPath);

// The versioned text form. Deterministic and locale-independent.
std::string writeSidecar(const Sidecar &sidecar);

// Parses the text form. Unknown or malformed lines are skipped rather than
// refused — the sidecar is droppable, so a preference that will not parse costs
// that preference and no more, unlike the document loader which refuses whole.
// Fields absent from the text keep their struct defaults.
Sidecar readSidecar(std::string_view text);

// Reads a sidecar from a file, returning defaults when the file is absent or
// unreadable. Never fails: an unreadable sidecar is an absent one.
Sidecar loadSidecar(const std::string &path);

// Writes a sidecar to a file. Returns false on a write failure, which the caller
// may ignore — a sidecar that could not be written is a lost preference, not a
// lost document.
bool saveSidecar(const std::string &path, const Sidecar &sidecar);

}  // namespace paroculus
