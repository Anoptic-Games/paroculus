#include "shell/models/reports.h"

namespace paroculus {

ReportsModel::ReportsModel(QObject *parent) : QAbstractListModel(parent) {}

int ReportsModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

QVariant ReportsModel::data(const QModelIndex &index, int role) const {
    if(!index.isValid() || index.row() < 0 || index.row() >= entries_.size()) return {};
    const Entry &entry = entries_[index.row()];
    switch(role) {
        case TextRole:
        case Qt::DisplayRole: return entry.text;
        case KindRole:        return entry.kind;
        case EntitiesRole:    return entry.entities;
        case ConstraintsRole: return entry.constraints;
        case SelectableRole:  return !entry.entities.isEmpty() || !entry.constraints.isEmpty();
        default:              return {};
    }
}

QHash<int, QByteArray> ReportsModel::roleNames() const {
    return {{TextRole, "text"},
            {KindRole, "kind"},
            {EntitiesRole, "entities"},
            {ConstraintsRole, "constraints"},
            {SelectableRole, "selectable"}};
}

void ReportsModel::append(const QString &text, const QString &kind, const QVariantList &entities,
                          const QVariantList &constraints) {
    beginInsertRows(QModelIndex(), static_cast<int>(entries_.size()),
                    static_cast<int>(entries_.size()));
    entries_.push_back(Entry{text, kind, entities, constraints});
    endInsertRows();
    emit countChanged();
}

void ReportsModel::clear() {
    if(entries_.isEmpty()) return;
    beginResetModel();
    entries_.clear();
    endResetModel();
    emit countChanged();
}

QString ReportsModel::latest() const {
    return entries_.isEmpty() ? QString() : entries_.back().text;
}

}  // namespace paroculus
