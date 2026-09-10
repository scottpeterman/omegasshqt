// app/sessionfilterproxy.cpp

#include "app/sessionfilterproxy.h"

#include "app/sessiontreemodel.h"

namespace omega::app {

SessionFilterProxy::SessionFilterProxy(QObject *parent)
    : QSortFilterProxyModel(parent) {
    setRecursiveFilteringEnabled(false);  // the recursion below is our own
    setDynamicSortFilter(true);
}

void SessionFilterProxy::setFilterText(const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed == text_) return;
    text_ = trimmed;
    invalidateFilter();
}

bool SessionFilterProxy::matches(const QModelIndex &index) const {
    if (text_.isEmpty()) return true;
    if (!index.isValid()) return false;

    const auto kind = static_cast<SessionTreeModel::Kind>(
        index.data(SessionTreeModel::KindRole).toInt());

    const QString name = index.data(SessionTreeModel::NameRole).toString();
    if (name.contains(text_, Qt::CaseInsensitive)) return true;

    if (kind == SessionTreeModel::SessionItem) {
        // DisplayRole carries "name  (description)" and HostRole carries
        // "host:port", which between them cover the same three fields
        // _apply_filter reads. Going through the roles rather than back to the
        // store keeps the filter off the database on every keystroke.
        if (index.data(Qt::DisplayRole).toString().contains(
                text_, Qt::CaseInsensitive))
            return true;
        if (index.data(SessionTreeModel::HostRole).toString().contains(
                text_, Qt::CaseInsensitive))
            return true;
    }

    return false;
}

bool SessionFilterProxy::anyDescendantMatches(const QModelIndex &index) const {
    const QAbstractItemModel *m = sourceModel();
    if (!m) return false;

    const int rows = m->rowCount(index);
    for (int i = 0; i < rows; ++i) {
        const QModelIndex child = m->index(i, 0, index);
        if (matches(child)) return true;
        if (anyDescendantMatches(child)) return true;
    }
    return false;
}

bool SessionFilterProxy::anyAncestorMatches(const QModelIndex &index) const {
    for (QModelIndex p = index.parent(); p.isValid(); p = p.parent())
        if (matches(p)) return true;
    return false;
}

bool SessionFilterProxy::filterAcceptsRow(int row,
                                          const QModelIndex &parent) const {
    if (text_.isEmpty()) return true;

    const QAbstractItemModel *m = sourceModel();
    if (!m) return true;

    const QModelIndex index = m->index(row, 0, parent);
    if (!index.isValid()) return false;

    return matches(index) || anyDescendantMatches(index) ||
           anyAncestorMatches(index);
}

}  // namespace omega::app
