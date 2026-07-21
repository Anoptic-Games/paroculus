// Application entry.
//
//   paroculus                  open the window
//   paroculus --selftest       solve + render headless, verify, exit 0 on success
//   paroculus --script FILE    replay a recorded session in the window
//   paroculus --record FILE    write everything this session does to a script
//   paroculus --export FILE    bake the demo to SVG and exit (headless)
//   paroculus --import FILE     trace geometry out of an SVG to stdout and exit
#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "app/scriptplay.h"
#include "app/selftest.h"
#include "core/persist.h"
#include "core/pose.h"
#include "core/svg.h"
#include "solve/context.h"
#include "solve/demosketch.h"
#include "solve/solve.h"

namespace {

// Bakes the demo document to SVG. Headless — no window, no Qt — so it is the
// small end-to-end demo the exit criterion asks be recorded, and a file a user
// can open in an external viewer to check the bake against the screen.
int runExport(const char *path) {
    paroculus::Document doc = paroculus::demoDocument(1.0);
    paroculus::SolveContext context = paroculus::SolveContext::forWholeDocument(doc);
    paroculus::solve(doc, context);
    paroculus::Pose pose(doc);
    pose.overlay(context.params());

    std::ofstream out(path);
    if(!out) {
        std::fprintf(stderr, "cannot write %s\n", path);
        return 2;
    }
    out << paroculus::writeSvg(doc, pose);
    // Close and check: a disk-full short write surfaces only on the flush, so
    // testing that the stream opened is not testing that the bake landed.
    out.close();
    if(!out) {
        std::fprintf(stderr, "failed writing %s\n", path);
        return 2;
    }
    std::printf("exported demo to %s\n", path);
    return 0;
}

// Traces an SVG back to unconstrained geometry and prints the document. The other
// half of the round-trip demo: an export re-imported is a paroculus document
// again, geometry free and inference deferred.
int runImport(const char *path) {
    std::ifstream in(path);
    if(!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const paroculus::SvgImport result = paroculus::readSvg(buffer.str());
    std::fprintf(stderr, "traced %zu elements, skipped %zu\n", result.traced, result.skipped);
    std::printf("%s", paroculus::serialize(result.document).c_str());
    // The traced document goes to stdout, which a caller may have redirected to
    // a file; a short write there is as silent as an unchecked export otherwise.
    if(std::fflush(stdout) != 0) {
        std::fprintf(stderr, "failed writing traced document to stdout\n");
        return 2;
    }
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    bool wantSelftest = false;
    const char *scriptPath = nullptr;
    const char *recordPath = nullptr;
    const char *exportPath = nullptr;
    const char *importPath = nullptr;
    for(int i = 1; i < argc; i++) {
        if(std::strcmp(argv[i], "--selftest") == 0) {
            wantSelftest = true;
        } else if(std::strcmp(argv[i], "--export") == 0) {
            if(i + 1 >= argc) {
                std::fprintf(stderr, "--export needs a file\n");
                return 2;
            }
            exportPath = argv[++i];
        } else if(std::strcmp(argv[i], "--import") == 0) {
            if(i + 1 >= argc) {
                std::fprintf(stderr, "--import needs a file\n");
                return 2;
            }
            importPath = argv[++i];
        } else if(std::strcmp(argv[i], "--script") == 0) {
            if(i + 1 >= argc) {
                std::fprintf(stderr, "--script needs a file\n");
                return 2;
            }
            scriptPath = argv[++i];
        } else if(std::strcmp(argv[i], "--record") == 0) {
            if(i + 1 >= argc) {
                std::fprintf(stderr, "--record needs a file\n");
                return 2;
            }
            recordPath = argv[++i];
        }
    }
    // The five entry modes are mutually exclusive, and silently letting one beat
    // the others hid three of the pairs. --record modifies the default
    // interactive session; every other flag either exits before a window opens
    // (--export, --import), runs and exits (--selftest), or replays a fixed
    // script (--script) — none is a live session worth recording, and recording
    // a replay only reproduces the file it was given.
    {
        const int modes = static_cast<int>(wantSelftest) + (scriptPath != nullptr) +
                          (recordPath != nullptr) + (exportPath != nullptr) +
                          (importPath != nullptr);
        if(modes > 1) {
            std::fprintf(
                stderr,
                "--selftest, --script, --record, --export and --import are mutually exclusive\n");
            return 2;
        }
    }
    // Export and import are headless projections that run and exit before any
    // window: no Qt is needed to bake a file or trace one.
    if(exportPath != nullptr) return runExport(exportPath);
    if(importPath != nullptr) return runImport(importPath);

    if(recordPath != nullptr) paroculus::pendingScript::setRecordPath(recordPath);

    // Parsed before the window opens, so a bad script fails at the command line
    // rather than into an empty window the user has to interpret.
    if(scriptPath != nullptr) {
        paroculus::GestureScript script;
        std::string error;
        if(!paroculus::loadScriptFile(scriptPath, script, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 2;
        }
        std::printf("script: %zu steps\n", script.steps.size());
        paroculus::pendingScript::set(std::move(script));
    }

    // The selftest touches QImage, which needs a QGuiApplication, but must run
    // where there is no display server.
    if(wantSelftest && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("paroculus"));

    if(wantSelftest) return paroculus::selftest();

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     []() { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Paroculus", "Main");
    return app.exec();
}
