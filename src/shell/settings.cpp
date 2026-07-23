#include "shell/settings.h"

#include <QDir>
#include <QStandardPaths>

namespace paroculus {
namespace {

constexpr int kRecentLimit = 12;
// Two rows deep: enough recent colours to catch a working set, and a saved
// palette large enough to hold a scheme without becoming a list to scroll.
constexpr int kRecentColorLimit = 12;
constexpr int kSavedColorLimit = 24;

}  // namespace

QVariantList Settings::withColorInserted(QVariantList colours, int argb, int cap) {
    // Drop an earlier copy so a re-use moves to the front, prepend, and cap.
    // Compared as ints, since two colours are the same swatch exactly when their
    // packed words are equal.
    for(int i = colours.size() - 1; i >= 0; i--) {
        if(colours.at(i).toInt() == argb) colours.removeAt(i);
    }
    colours.prepend(argb);
    while(colours.size() > cap) colours.removeLast();
    return colours;
}

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

QVariantList Settings::savedColors() const {
    return settings_.value(QStringLiteral("savedColors")).toList();
}

void Settings::addSavedColor(int argb) {
    settings_.setValue(QStringLiteral("savedColors"),
                       withColorInserted(savedColors(), argb, kSavedColorLimit));
}

void Settings::removeSavedColor(int argb) {
    QVariantList colours = savedColors();
    for(int i = colours.size() - 1; i >= 0; i--) {
        if(colours.at(i).toInt() == argb) colours.removeAt(i);
    }
    settings_.setValue(QStringLiteral("savedColors"), colours);
}

QVariantList Settings::recentColors() const {
    return settings_.value(QStringLiteral("recentColors")).toList();
}

void Settings::addRecentColor(int argb) {
    settings_.setValue(QStringLiteral("recentColors"),
                       withColorInserted(recentColors(), argb, kRecentColorLimit));
}

}  // namespace paroculus
