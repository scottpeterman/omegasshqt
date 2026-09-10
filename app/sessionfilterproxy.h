// app/sessionfilterproxy.h
//
// The filter over SessionTreeModel.
//
// nterm-qt filters by walking the tree and calling setHidden on each item,
// which is a property of the view. That is what makes its post-drop walk
// dangerous while a filter is up: the walk reads the view, and the view is
// missing rows. Here the filter is a proxy and the model underneath is
// untouched, so a filtered view cannot describe a tree the model does not
// have.
//
// Three ways a row survives the filter, and all three are needed for a tree:
//
//   - it matches. Sessions match on name, description and hostname, which is
//     the same three fields _apply_filter uses. Folders match on name.
//   - a descendant matches, so the folder containing a hit stays visible and
//     the hit is not orphaned. This is the "without flattening" half.
//   - an ancestor matches, so a folder matched by name shows its contents
//     rather than appearing empty. nterm-qt does not do this: a folder whose
//     name matches but whose sessions do not is hidden along with them,
//     because a folder's visibility there is decided only by its children.
//
// QSortFilterProxyModel::filterAcceptsRow is asked about a row bottom-up and
// caches nothing across calls, so the descendant check is a recursive walk. A
// session tree is small enough that this does not matter, and the alternative
// -- maintaining a match set on every keystroke -- is a second source of truth
// about what is visible.

#ifndef OMEGA_APP_SESSIONFILTERPROXY_H
#define OMEGA_APP_SESSIONFILTERPROXY_H

#include <QSortFilterProxyModel>
#include <QString>

namespace omega::app {

class SessionFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit SessionFilterProxy(QObject *parent = nullptr);

    // Case-insensitive substring, trimmed. Empty means everything shows, which
    // is a distinct state from "matches nothing" and is why the pane asks this
    // rather than inspecting the line edit.
    void setFilterText(const QString &text);
    QString filterText() const { return text_; }
    bool isFiltering() const { return !text_.isEmpty(); }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;

private:
    bool matches(const QModelIndex &index) const;
    bool anyDescendantMatches(const QModelIndex &index) const;
    bool anyAncestorMatches(const QModelIndex &index) const;

    QString text_;
};

}  // namespace omega::app

#endif  // OMEGA_APP_SESSIONFILTERPROXY_H
