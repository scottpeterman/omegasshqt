// app/sessiontreemodel.h
//
// A QAbstractItemModel over omega::sessions::SessionStore.
//
// This is the one place Omega diverges from nterm-qt rather than porting it.
// manager/tree.py uses a QTreeWidget and a _persist_tree_state that walks the
// whole visible tree after every drop, reading parents and positions back out
// of the view. That works, and it has two consequences worth naming because
// they are why the divergence exists:
//
//   - The walk sees only what is shown. With a filter active, hidden rows are
//     absent from the walk, so a drop while filtering rewrites positions from
//     a partial tree. The pane's answer is to refuse drags while the filter is
//     non-empty; the model's answer is that the drop is computed from the
//     model, not from the view, so the view's hidden state cannot reach it.
//
//   - The view is the source of truth between the drop and the write. Here the
//     store is, always.
//
// The ordering divergence is smaller but real. get_tree returns sessions
// ORDER BY name with no folder grouping, so nterm-qt writes sessions.position
// on every drop and never reads it -- drag a session within a folder, reload,
// and it snaps back to alphabetical. Folders do not have this problem; they
// are listed ORDER BY position, name. This model sorts BOTH by position then
// name, which is what the column is for. Nothing about that is incompatible:
// nterm-qt keeps ignoring sessions.position and sorts by name as it always
// did, and the file means the same thing to both.
//
// Children within a container are folders first, then sessions, matching the
// order refresh() builds them in. positions are per-table and therefore
// renumbered per-kind; see reorderWithin().

#ifndef OMEGA_APP_SESSIONTREEMODEL_H
#define OMEGA_APP_SESSIONTREEMODEL_H

#include <QAbstractItemModel>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "app/livesessions.h"
#include "sessions/store.h"

namespace omega::app {

class SessionTreeModel : public QAbstractItemModel {
    Q_OBJECT

public:
    // Named *Item to stay out of the way of omega::sessions::Session and
    // omega::sessions::Folder, which are the payload types.
    enum Kind { FolderItem, SessionItem };

    enum Roles {
        KindRole = Qt::UserRole + 1,  // Kind
        IdRole,                       // qint64, the store's primary key
        NameRole,                     // QString, unadorned
        HostRole,                     // QString, "host:port", sessions only
        ExpandedRole,                 // bool, folders only

        // The three the session delegate needs. They are NOT stored: the
        // first is composed here and the other two come from the live
        // registry, because a per-run connection state has no business in a
        // SQLite file two applications share.
        DetailRole,  // QString, the subtitle line
        LinkRole,    // int(Link)
        RttRole,     // int milliseconds, -1 when unknown
    };

    explicit SessionTreeModel(sessions::SessionStore *store,
                              QObject *parent = nullptr);

    // Optional. Without one, LinkRole is always Idle and RttRole always -1,
    // which is what the delegate draws for a session nobody has opened -- so
    // a model with no registry is correct rather than broken.
    void setLiveRegistry(LiveSessionRegistry *registry);
    ~SessionTreeModel() override;

    // Rebuilds the whole node tree from the store. Every mutation below ends
    // here. A reset is heavier than the equivalent begin/endMoveRows dance,
    // and it is chosen anyway: a session tree is hundreds of rows, the reload
    // is one SELECT of each table, and the alternative is a class of
    // persistent-index bugs that only show up after a specific drag. The pane
    // restores expansion from folders.expanded and selection by id after a
    // reset, so nothing visible is lost.
    void reload();

    // --- reading ----------------------------------------------------------

    Kind kindOf(const QModelIndex &index) const;
    qint64 idOf(const QModelIndex &index) const;

    std::optional<sessions::Session> sessionAt(const QModelIndex &index) const;
    std::optional<sessions::Folder> folderAt(const QModelIndex &index) const;

    // The folder an index sits IN: the folder itself when index is a folder,
    // its parent folder when index is a session, empty at root. This is what
    // "New Session Here" means, and it is why the two cases are one call.
    std::optional<qint64> containingFolder(const QModelIndex &index) const;

    QModelIndex indexForFolder(qint64 id) const;
    QModelIndex indexForSession(qint64 id) const;

    bool isExpanded(const QModelIndex &index) const;

    // Writes folders.expanded through to the store without a reset -- the
    // view is already in the state being recorded, and resetting under an
    // expand handler collapses what the user just opened.
    void setExpanded(const QModelIndex &index, bool expanded);

    // --- writing ----------------------------------------------------------
    // Each of these writes through to the store and reloads. They return the
    // new row's id where there is one, so a caller can select what it made.

    std::optional<qint64> addFolder(const QString &name,
                                    std::optional<qint64> parentId);
    bool renameFolder(qint64 id, const QString &name);
    bool deleteFolder(qint64 id);

    std::optional<qint64> addSession(const sessions::Session &session);
    bool updateSession(const sessions::Session &session);
    bool deleteSession(qint64 id);
    std::optional<qint64> duplicateSession(qint64 id);

    // last_connected and connect_count, written by SQLite's clock. No reload:
    // neither column is displayed, and connecting is not a reason to reset the
    // tree under the user.
    void recordConnect(qint64 id);

    // The message from the last failed store call, for a caller that wants to
    // show it. Empty when the last call succeeded.
    QString lastError() const { return lastError_; }

    // --- QAbstractItemModel ----------------------------------------------

    QModelIndex index(int row, int column,
                      const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &index) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    Qt::DropActions supportedDropActions() const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    bool canDropMimeData(const QMimeData *data, Qt::DropAction action, int row,
                         int column, const QModelIndex &parent) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row,
                      int column, const QModelIndex &parent) override;

private:
    struct Node {
        Kind kind = FolderItem;
        qint64 id = -1;

        QString name;
        QString description;
        QString hostname;
        QString username;
        int port = 22;
        int position = 0;
        bool expanded = true;

        Node *parent = nullptr;
        std::vector<std::unique_ptr<Node>> children;
    };

    QString storedDetail(const Node *node) const;

    Node *nodeFor(const QModelIndex &index) const;
    Node *findNode(Kind kind, qint64 id) const;
    static Node *findIn(Node *from, Kind kind, qint64 id);
    QModelIndex indexForNode(Node *node) const;

    // True when `folder` is `candidate` or an ancestor of it -- the check that
    // stops a folder being dropped into its own subtree. move_folder does the
    // same walk; here it also has to answer before the drop is accepted, so
    // the cursor shows a refusal rather than the drop silently doing nothing.
    bool isDescendantOf(Node *candidate, Node *folder) const;

    // Rewrites positions for one container after a move. Folders and sessions
    // are renumbered independently because their positions live in different
    // tables and are compared only against siblings of the same kind.
    //
    // `skip` is the node that just left. The in-memory tree is not rebuilt
    // until reload(), so a node moved to another container is still sitting in
    // this one's child list; renumbering it here would undo the write that
    // just placed it.
    void reorderWithin(Node *container, Node *skip = nullptr);

    sessions::SessionStore *store_ = nullptr;
    LiveSessionRegistry *live_ = nullptr;
    std::unique_ptr<Node> root_;
    mutable QString lastError_;
};

}  // namespace omega::app

#endif  // OMEGA_APP_SESSIONTREEMODEL_H
