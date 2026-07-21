// The append-only session log of everything no-silent-changes generates.
//
// Deletion counts, structure reports, boundary drops, refusals: the events the
// policy requires be surfaced rather than happening quietly. The panel is the
// memory and the toast is the notice, and both read this one model — so nothing
// the policy produces is ever stringless in between, which is why the model
// exists from U0 even though the reports panel's maturity (click-to-select, an
// entry per producer) is a later stage.
//
// A QAbstractListModel so list selection and scroll survive a refresh, and
// append-only so there is no diffing: a new event is one inserted row and the
// newest row is the toast's subject. Toolkit-bound and shell-only, referenced
// from QML but constructed by its Workspace.
#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVector>
#include <QtQml/qqmlregistration.h>

namespace paroculus {

class ReportsModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("A ReportsModel belongs to a Workspace and is not constructed in QML")
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles { TextRole = Qt::UserRole + 1, KindRole };

    explicit ReportsModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Appends one entry. `kind` is a coarse tag (deletion, structure, refusal)
    // for later colouring and filtering; empty is fine at this stage.
    void append(const QString &text, const QString &kind = QString());

    Q_INVOKABLE void clear();

    // The most recent entry's text, for the toast, or empty when none.
    QString latest() const;

signals:
    void countChanged();

private:
    struct Entry {
        QString text;
        QString kind;
    };
    QVector<Entry> entries_;
};

}  // namespace paroculus
