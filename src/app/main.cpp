// Application entry.
//
//   paroculus              open the window
//   paroculus --selftest   solve + render headless, verify, exit 0 on success
#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include <cstring>

#include "app/selftest.h"

int main(int argc, char *argv[]) {
    bool wantSelftest = false;
    for(int i = 1; i < argc; i++) {
        if(std::strcmp(argv[i], "--selftest") == 0) wantSelftest = true;
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
