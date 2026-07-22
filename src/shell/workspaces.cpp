#include "shell/workspaces.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#include <QDir>
#include <QFileInfo>

#include "app/scriptplay.h"
#include "core/persist.h"
#include "shell/settings.h"
#include "shell/workspace.h"

namespace paroculus {

WorkspaceManager::WorkspaceManager(Settings *settings, QObject *parent)
    : QObject(parent), settings_(settings) {}

WorkspaceManager::~WorkspaceManager() = default;

void WorkspaceManager::setGlyphDensity(double multiplier) {
    if(settings_ != nullptr) settings_->setGlyphDensity(multiplier);
    // Every open workspace, not only the active one: the preference is
    // application-wide, so a hidden tab must read at the same density the moment
    // it is chosen rather than keeping its own until it is next activated.
    for(Workspace *workspace : workspaces_) workspace->setGlyphDensity(multiplier);
}

Workspace *WorkspaceManager::addWorkspace() {
    Workspace *ws = new Workspace(this);
    ws->untitledOrdinal_ = nextUntitled_++;
    // The glyph-density preference is application-wide, so every workspace is
    // born honouring it — a new or opened tab reads at the same density as the
    // rest without the user re-setting it.
    if(settings_ != nullptr) ws->setGlyphDensity(settings_->glyphDensity());
    workspaces_.append(ws);
    return ws;
}

bool WorkspaceManager::pristine(const Workspace *workspace) const {
    // An untitled tab the user has not touched: no path and nothing to save.
    // Open reuses one of these rather than leaving an empty scratch tab beside
    // the opened document.
    return workspace != nullptr && workspace->filePath().isEmpty() && !workspace->dirty();
}

void WorkspaceManager::activate(int index) {
    Workspace *previous = active();
    activeIndex_ = index;
    Workspace *now = active();
    // Async on the active workspace only: disable it on the workspace being left
    // and enable it on the one arrived at, so exactly one scheduler is alive — the
    // posture the plan fixes for U0 before the cross-session gate audit.
    if(previous != nullptr && previous != now) previous->disableAsync();
    if(now != nullptr) now->enableAsync();
    emit activeChanged();
}

void WorkspaceManager::startup() {
    Workspace *ws = addWorkspace();

    GestureScript pending;
    if(pendingScript::take(pending)) {
        ws->playScript(std::move(pending));
    } else {
        const std::string openPath = pendingScript::takeOpenPath();
        const std::string recordPath = pendingScript::takeRecordPath();
        if(!openPath.empty()) {
            std::ifstream in(openPath);
            if(in) {
                std::stringstream buffer;
                buffer << in.rdbuf();
                const LoadResult result =
                    ws->loadFrom(buffer.str(), QString::fromStdString(openPath));
                if(result) {
                    ws->loadSidecarFrom(ws->sidecarPath());
                    if(settings_) settings_->addRecentFile(QString::fromStdString(openPath));
                } else {
                    // The frame is not connected yet at launch, so stderr is the
                    // reliable channel; the signal is emitted too for a later hook.
                    std::fprintf(stderr, "%s (line %zu)\n", result.error.c_str(), result.line);
                    emit openFailed(QString::fromStdString(openPath),
                                    QString::fromStdString(result.error), int(result.line));
                }
            } else {
                std::fprintf(stderr, "cannot open %s\n", openPath.c_str());
            }
        }
        if(!recordPath.empty()) ws->startRecording(QString::fromStdString(recordPath));
    }

    // activeIndex_ is still -1 here, so activate has no previous to disable and
    // enables async on the first workspace alone.
    activate(0);
    emit tabsChanged();
}

QList<QObject *> WorkspaceManager::tabs() const {
    QList<QObject *> out;
    out.reserve(workspaces_.size());
    for(Workspace *ws : workspaces_) out.append(ws);
    return out;
}

int WorkspaceManager::count() const { return static_cast<int>(workspaces_.size()); }

Workspace *WorkspaceManager::active() const {
    if(activeIndex_ < 0 || activeIndex_ >= workspaces_.size()) return nullptr;
    return workspaces_[activeIndex_];
}

void WorkspaceManager::setActiveIndex(int index) {
    if(index < 0 || index >= workspaces_.size() || index == activeIndex_) return;
    activate(index);
}

void WorkspaceManager::newWorkspace() {
    addWorkspace();
    activate(static_cast<int>(workspaces_.size()) - 1);
    emit tabsChanged();
}

void WorkspaceManager::cycleActive(int delta) {
    if(workspaces_.isEmpty()) return;
    const int n = static_cast<int>(workspaces_.size());
    setActiveIndex(((activeIndex_ + delta) % n + n) % n);
}

void WorkspaceManager::closeWorkspace(int index) {
    if(index < 0 || index >= workspaces_.size()) return;
    Workspace *ws = workspaces_.takeAt(index);
    // Join its workers now rather than waiting for the deferred destruction, so no
    // two schedulers overlap while the neighbour we activate enables its own.
    ws->disableAsync();
    ws->deleteLater();

    // Never a window with no document: closing the last tab opens a fresh empty
    // one. Otherwise keep the active tab pointing at the same document it was, or
    // clamp when the closed one was to its right.
    if(workspaces_.isEmpty()) {
        addWorkspace();
        activeIndex_ = -1;
        activate(0);
    } else {
        if(index < activeIndex_) activeIndex_--;
        if(activeIndex_ >= workspaces_.size()) activeIndex_ = static_cast<int>(workspaces_.size()) - 1;
        const int landed = activeIndex_;
        activeIndex_ = -1;  // force activate() to re-enable async and signal
        activate(landed);
    }
    emit tabsChanged();
}

bool WorkspaceManager::openFile(const QString &path) {
    std::ifstream in(path.toStdString());
    if(!in) {
        emit openFailed(path, QStringLiteral("cannot open file"), 0);
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();

    const bool reuse = pristine(active());
    Workspace *target = reuse ? active() : addWorkspace();

    const LoadResult result = target->loadFrom(buffer.str(), path);
    if(!result) {
        emit openFailed(path, QString::fromStdString(result.error), int(result.line));
        // A refused load leaves the document untouched, so a freshly added tab is
        // now an empty one with no reason to exist: drop it. A reused pristine tab
        // stays exactly as it was.
        if(!reuse) {
            workspaces_.removeAll(target);
            target->deleteLater();
        }
        return false;
    }
    target->loadSidecarFrom(target->sidecarPath());
    if(settings_) settings_->addRecentFile(path);

    // activate reads the current activeIndex_ as the previous, so opening into a
    // new tab disables async on the tab left behind; opening into the reused tab
    // (target == active) re-enables it, which loadFrom cleared.
    activate(static_cast<int>(workspaces_.indexOf(target)));
    emit tabsChanged();
    return true;
}

bool WorkspaceManager::activeNeedsPath() const {
    const Workspace *ws = active();
    return ws != nullptr && ws->filePath().isEmpty();
}

bool WorkspaceManager::saveActiveTo(const QString &path) {
    Workspace *ws = active();
    if(ws == nullptr) return false;

    // The default directory is created on first save, per the spec: a chosen path
    // in a not-yet-existing Paroculus folder writes rather than failing.
    QDir().mkpath(QFileInfo(path).absolutePath());

    std::ofstream out(path.toStdString());
    if(!out) {
        emit saveFailed(path);
        return false;
    }
    out << ws->serializeDocument();
    // Close and check: a disk-full short write surfaces only on the flush, so a
    // save that only tested the open is not a save that landed.
    out.close();
    if(!out) {
        emit saveFailed(path);
        return false;
    }

    ws->markSaved(path);
    ws->writeSidecarTo(ws->sidecarPath());
    if(settings_ != nullptr) {
        settings_->addRecentFile(path);
        settings_->setDefaultDirectory(QFileInfo(path).absolutePath());
    }
    emit tabsChanged();
    return true;
}

QString WorkspaceManager::activePath() const {
    const Workspace *ws = active();
    return ws != nullptr ? ws->filePath() : QString();
}

QStringList WorkspaceManager::recentFiles() const {
    return settings_ != nullptr ? settings_->recentFiles() : QStringList();
}

QString WorkspaceManager::defaultDirectory() const {
    return settings_ != nullptr ? settings_->defaultDirectory() : QString();
}

}  // namespace paroculus
