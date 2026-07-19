// Application entry.
//
//   paroculus                  open the window
//   paroculus --selftest       solve + render headless, verify, exit 0 on success
//   paroculus --script FILE    replay a recorded session in the window
//   paroculus --record FILE    write everything this session does to a script
#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include <cstdio>
#include <cstring>

#include "app/scriptplay.h"
#include "app/selftest.h"

int main(int argc, char *argv[]) {
    bool wantSelftest = false;
    const char *scriptPath = nullptr;
    const char *recordPath = nullptr;
    for(int i = 1; i < argc; i++) {
        if(std::strcmp(argv[i], "--selftest") == 0) {
            wantSelftest = true;
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
    if(scriptPath != nullptr && recordPath != nullptr) {
        // Recording a replay would only reproduce the file it was given, which
        // the corpus already asserts and which nobody wants by hand.
        std::fprintf(stderr, "--script and --record are mutually exclusive\n");
        return 2;
    }
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
