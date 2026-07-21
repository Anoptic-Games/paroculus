// Application settings: the tier that may never influence document bytes.
//
// The spec's first settings tier — theme, layouts, recent files, default
// directories, the glyph density preference, binding overrides when they arrive
// — stored under the platform config location through QSettings, which resolves
// to XDG on Linux, Library/Preferences on macOS and AppData on Windows. What
// separates this tier from the sidecar is not the mechanism but the rule: nothing
// here reaches a document, and nothing document-semantic is kept here.
//
// A QObject exposed to QML as a context property, alongside the manager. Named
// layouts are opaque strings — the panel host serializes its own arrangement and
// hands it here by name — so the panel-host contract stays a contract: a docking
// dependency could replace the host without this store knowing the schema.
#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

namespace paroculus {

class Settings : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Settings is created in main and exposed as a context property")

public:
    explicit Settings(QObject *parent = nullptr);

    // Recent files, most recent first, capped. Adding one moves it to the front
    // and de-duplicates, so the list is a stack of distinct paths.
    QStringList recentFiles() const;
    void addRecentFile(const QString &path);
    Q_INVOKABLE void clearRecentFiles();

    // The default directory for the open and save dialogs. The stored one once
    // the user has saved anywhere, else the platform documents location plus
    // Paroculus. Its creation on first save is the manager's, not this store's.
    QString defaultDirectory() const;
    void setDefaultDirectory(const QString &directory);

    // Named panel-arrangement snapshots. Opaque to this store; the panel host
    // owns the schema. reset is expressed by the frame reapplying the in-code
    // default, so there is no "default" entry here to corrupt.
    Q_INVOKABLE QStringList layoutNames() const;
    Q_INVOKABLE QString layout(const QString &name) const;
    Q_INVOKABLE void saveLayout(const QString &name, const QString &data);
    Q_INVOKABLE void removeLayout(const QString &name);

    // The glyph-density preference, a display-only number U2 consumes. Stored
    // here from U0 so the schema exists before the surface that reads it. Safe as
    // a plain preference because scripts never record feel policy.
    Q_INVOKABLE double glyphDensity() const;
    Q_INVOKABLE void setGlyphDensity(double density);

private:
    mutable QSettings settings_;
};

}  // namespace paroculus
