#include "shell/settings.h"

#include <QDir>
#include <QStandardPaths>

namespace paroculus {
namespace {

constexpr int kRecentLimit = 12;

}  // namespace

Settings::Settings(QObject *parent)
    : QObject(parent),
      // Organization and application both "paroculus", so the config path is one
      // level rather than nested under a vendor. QCoreApplication carries the same
      // names; QSettings default construction reads them.
      settings_(QSettings::IniFormat, QSettings::UserScope,
                QStringLiteral("paroculus"), QStringLiteral("paroculus")) {}

QStringList Settings::recentFiles() const {
    return settings_.value(QStringLiteral("recentFiles")).toStringList();
}

void Settings::addRecentFile(const QString &path) {
    if(path.isEmpty()) return;
    QStringList recents = recentFiles();
    recents.removeAll(path);
    recents.prepend(path);
    while(recents.size() > kRecentLimit) recents.removeLast();
    settings_.setValue(QStringLiteral("recentFiles"), recents);
}

void Settings::clearRecentFiles() { settings_.remove(QStringLiteral("recentFiles")); }

QString Settings::defaultDirectory() const {
    const QString stored = settings_.value(QStringLiteral("defaultDirectory")).toString();
    if(!stored.isEmpty()) return stored;
    const QString documents =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return QDir(documents).filePath(QStringLiteral("Paroculus"));
}

void Settings::setDefaultDirectory(const QString &directory) {
    settings_.setValue(QStringLiteral("defaultDirectory"), directory);
}

QStringList Settings::layoutNames() const {
    settings_.beginGroup(QStringLiteral("layouts"));
    const QStringList names = settings_.childKeys();
    settings_.endGroup();
    return names;
}

QString Settings::layout(const QString &name) const {
    return settings_.value(QStringLiteral("layouts/") + name).toString();
}

void Settings::saveLayout(const QString &name, const QString &data) {
    settings_.setValue(QStringLiteral("layouts/") + name, data);
}

void Settings::removeLayout(const QString &name) {
    settings_.remove(QStringLiteral("layouts/") + name);
}

double Settings::glyphDensity() const {
    return settings_.value(QStringLiteral("glyphDensity"), 1.0).toDouble();
}

void Settings::setGlyphDensity(double density) {
    settings_.setValue(QStringLiteral("glyphDensity"), density);
}

}  // namespace paroculus
