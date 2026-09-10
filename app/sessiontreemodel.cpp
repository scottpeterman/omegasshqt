// app/sessiontreemodel.cpp

#include "app/sessiontreemodel.h"

#include <QApplication>
#include <QDataStream>
#include <QIODevice>
#include <QMimeData>
#include <QStyle>

#include <algorithm>

namespace omega::app {
namespace {

// One private type, carrying kind and id. Nothing outside this model reads it,
// and it is deliberately not text/uri-list or anything a foreign view could
// think it understood -- an internal move is the only drop this tree accepts.
const char *const kMimeType = "application/x-omega-session-item";

QString toQ(const std::string &s) { return QString::fromStdString(s); }
std::string toStd(const QString &s) { return s.toStdString(); }

}  // namespace

SessionTreeModel::SessionTreeModel(sessions::SessionStore *store,
                                   QObject *parent)
    : QAbstractItemModel(parent), store_(store) {
    root_ = std::make_unique<Node>();
    root_->kind = FolderItem;
    root_->id = -1;
    reload();
}

SessionTreeModel::~SessionTreeModel() = default;

// ---------------------------------------------------------------------------
// building
// ---------------------------------------------------------------------------

void SessionTreeModel::setLiveRegistry(LiveSessionRegistry *registry) {
    if (live_ == registry) return;
    if (live_) live_->disconnect(this);
    live_ = registry;
    if (!live_) return;

    // One row, not a reset. A reset on every state transition would collapse
    // the tree under the user four times per connect, and the pane's
    // expansion restore would run each time.
    connect(live_, &LiveSessionRegistry::changed, this, [this](qint64 id) {
        const QModelIndex idx = indexForSession(id);
        if (!idx.isValid()) return;
        emit dataChanged(idx, idx,
                         {DetailRole, LinkRole, RttRole, Qt::DisplayRole});
    });
}

// user@host:port, or host:port when the session names no user. This is the
// subtitle a session has when nothing is connected to it, which is most of
// them most of the time.
QString SessionTreeModel::storedDetail(const Node *n) const {
    const QString hostPort = QStringLiteral("%1:%2").arg(n->hostname).arg(n->port);
    return n->username.isEmpty() ? hostPort
                                 : n->username + QLatin1Char('@') + hostPort;
}

void SessionTreeModel::reload() {
    beginResetModel();

    root_->children.clear();

    if (!store_) {
        endResetModel();
        return;
    }

    sessions::Status status;
    const sessions::Tree tree = store_->tree(&status);
    if (!status) lastError_ = toQ(status.message);

    // Folders first, so a session's parent exists by the time it is placed.
    // Orphans -- a folder whose parent_id names a row that is gone -- land at
    // root rather than vanishing. The store's delete_folder reparents to root
    // and never leaves one, but a file written by something else might.
    std::vector<Node *> byId;
    std::vector<std::pair<int64_t, Node *>> pending;
    pending.reserve(tree.folders.size());

    for (const sessions::Folder &f : tree.folders) {
        auto node = std::make_unique<Node>();
        node->kind = FolderItem;
        node->id = f.id.value_or(-1);
        node->name = toQ(f.name);
        node->position = f.position;
        node->expanded = f.expanded;
        pending.emplace_back(f.parent_id.value_or(-1), node.get());
        // Parked at root; moved below once every folder node exists.
        node->parent = root_.get();
        root_->children.push_back(std::move(node));
    }

    // Reparent in a second pass, moving the unique_ptr out of root's list.
    for (const auto &[parentId, node] : pending) {
        if (parentId < 0) continue;
        Node *parentNode = findIn(root_.get(), FolderItem, parentId);
        if (!parentNode || parentNode == node) continue;

        auto it = std::find_if(root_->children.begin(), root_->children.end(),
                               [node = node](const std::unique_ptr<Node> &c) {
                                   return c.get() == node;
                               });
        if (it == root_->children.end()) continue;

        std::unique_ptr<Node> owned = std::move(*it);
        root_->children.erase(it);
        owned->parent = parentNode;
        parentNode->children.push_back(std::move(owned));
    }

    for (const sessions::Session &s : tree.sessions) {
        auto node = std::make_unique<Node>();
        node->kind = SessionItem;
        node->id = s.id.value_or(-1);
        node->name = toQ(s.name);
        node->description = toQ(s.description);
        node->hostname = toQ(s.hostname);
        node->port = s.port;
        // For the delegate's subtitle. The session's own username, not the
        // resolved one -- resolution needs the vault and the settings, and a
        // tree row is not worth opening a vault for.
        node->username = s.username ? toQ(*s.username) : QString();
        node->position = s.position;

        Node *parentNode = root_.get();
        if (s.folder_id.has_value()) {
            if (Node *f = findIn(root_.get(), FolderItem, *s.folder_id))
                parentNode = f;
        }
        node->parent = parentNode;
        parentNode->children.push_back(std::move(node));
    }

    // Sort every container: folders before sessions (FolderItem is 0), then
    // position, then name. Applying position to sessions as well as folders is
    // the ordering divergence documented in the header -- the store hands
    // sessions back ordered by name alone, as get_tree does.
    std::vector<Node *> stack{root_.get()};
    while (!stack.empty()) {
        Node *n = stack.back();
        stack.pop_back();

        std::sort(n->children.begin(), n->children.end(),
                  [](const std::unique_ptr<Node> &a,
                     const std::unique_ptr<Node> &b) {
                      if (a->kind != b->kind) return a->kind < b->kind;
                      if (a->position != b->position)
                          return a->position < b->position;
                      return a->name.localeAwareCompare(b->name) < 0;
                  });

        for (const std::unique_ptr<Node> &c : n->children) stack.push_back(c.get());
    }

    endResetModel();
}

// ---------------------------------------------------------------------------
// node lookup
// ---------------------------------------------------------------------------

SessionTreeModel::Node *SessionTreeModel::nodeFor(
    const QModelIndex &index) const {
    if (!index.isValid()) return root_.get();
    return static_cast<Node *>(index.internalPointer());
}

SessionTreeModel::Node *SessionTreeModel::findIn(Node *from, Kind kind,
                                                 qint64 id) {
    if (!from) return nullptr;
    for (const std::unique_ptr<Node> &c : from->children) {
        if (c->kind == kind && c->id == id) return c.get();
        if (Node *hit = findIn(c.get(), kind, id)) return hit;
    }
    return nullptr;
}

SessionTreeModel::Node *SessionTreeModel::findNode(Kind kind, qint64 id) const {
    return findIn(root_.get(), kind, id);
}

QModelIndex SessionTreeModel::indexForNode(Node *node) const {
    if (!node || node == root_.get()) return {};
    Node *parent = node->parent ? node->parent : root_.get();
    for (size_t i = 0; i < parent->children.size(); ++i) {
        if (parent->children[i].get() == node)
            return createIndex(static_cast<int>(i), 0, node);
    }
    return {};
}

QModelIndex SessionTreeModel::indexForFolder(qint64 id) const {
    return indexForNode(findNode(FolderItem, id));
}

QModelIndex SessionTreeModel::indexForSession(qint64 id) const {
    return indexForNode(findNode(SessionItem, id));
}

bool SessionTreeModel::isDescendantOf(Node *candidate, Node *folder) const {
    for (Node *n = candidate; n && n != root_.get(); n = n->parent)
        if (n == folder) return true;
    return false;
}

// ---------------------------------------------------------------------------
// reading
// ---------------------------------------------------------------------------

SessionTreeModel::Kind SessionTreeModel::kindOf(const QModelIndex &i) const {
    Node *n = nodeFor(i);
    return n ? n->kind : FolderItem;
}

qint64 SessionTreeModel::idOf(const QModelIndex &i) const {
    Node *n = nodeFor(i);
    return n ? n->id : -1;
}

std::optional<sessions::Session> SessionTreeModel::sessionAt(
    const QModelIndex &i) const {
    if (!store_ || !i.isValid()) return std::nullopt;
    Node *n = nodeFor(i);
    if (!n || n->kind != SessionItem) return std::nullopt;

    sessions::Status status;
    auto s = store_->getSession(n->id, &status);
    if (!status) lastError_ = toQ(status.message);
    return s;
}

std::optional<sessions::Folder> SessionTreeModel::folderAt(
    const QModelIndex &i) const {
    if (!store_ || !i.isValid()) return std::nullopt;
    Node *n = nodeFor(i);
    if (!n || n->kind != FolderItem) return std::nullopt;

    sessions::Status status;
    auto f = store_->getFolder(n->id, &status);
    if (!status) lastError_ = toQ(status.message);
    return f;
}

std::optional<qint64> SessionTreeModel::containingFolder(
    const QModelIndex &i) const {
    if (!i.isValid()) return std::nullopt;
    Node *n = nodeFor(i);
    if (!n) return std::nullopt;
    if (n->kind == FolderItem) return n->id;
    if (n->parent && n->parent != root_.get()) return n->parent->id;
    return std::nullopt;
}

bool SessionTreeModel::isExpanded(const QModelIndex &i) const {
    Node *n = nodeFor(i);
    return n && n->kind == FolderItem && n->expanded;
}

void SessionTreeModel::setExpanded(const QModelIndex &i, bool expanded) {
    if (!store_) return;
    Node *n = nodeFor(i);
    if (!n || n->kind != FolderItem || n->expanded == expanded) return;

    n->expanded = expanded;

    sessions::Status status;
    auto folder = store_->getFolder(n->id, &status);
    if (!folder) return;
    folder->expanded = expanded;
    const sessions::Status wrote = store_->updateFolder(*folder);
    if (!wrote) lastError_ = toQ(wrote.message);
}

// ---------------------------------------------------------------------------
// writing
// ---------------------------------------------------------------------------

std::optional<qint64> SessionTreeModel::addFolder(
    const QString &name, std::optional<qint64> parentId) {
    if (!store_) return std::nullopt;

    std::optional<int64_t> parent;
    if (parentId.has_value()) parent = *parentId;

    sessions::Status status;
    auto id = store_->addFolder(toStd(name), parent, &status);
    if (!status) lastError_ = toQ(status.message);
    if (id) reload();
    return id;
}

bool SessionTreeModel::renameFolder(qint64 id, const QString &name) {
    if (!store_) return false;

    sessions::Status status;
    auto folder = store_->getFolder(id, &status);
    if (!folder) return false;

    folder->name = toStd(name);
    const sessions::Status wrote = store_->updateFolder(*folder);
    if (!wrote) {
        lastError_ = toQ(wrote.message);
        return false;
    }
    reload();
    return true;
}

bool SessionTreeModel::deleteFolder(qint64 id) {
    if (!store_) return false;
    const sessions::Status status = store_->deleteFolder(id);
    if (!status) {
        lastError_ = toQ(status.message);
        return false;
    }
    reload();
    return true;
}

std::optional<qint64> SessionTreeModel::addSession(
    const sessions::Session &session) {
    if (!store_) return std::nullopt;

    sessions::Status status;
    auto id = store_->addSession(session, &status);
    if (!status) lastError_ = toQ(status.message);
    if (id) reload();
    return id;
}

bool SessionTreeModel::updateSession(const sessions::Session &session) {
    if (!store_) return false;
    const sessions::Status status = store_->updateSession(session);
    if (!status) {
        lastError_ = toQ(status.message);
        return false;
    }
    reload();
    return true;
}

bool SessionTreeModel::deleteSession(qint64 id) {
    if (!store_) return false;
    const sessions::Status status = store_->deleteSession(id);
    if (!status) {
        lastError_ = toQ(status.message);
        return false;
    }
    reload();
    return true;
}

std::optional<qint64> SessionTreeModel::duplicateSession(qint64 id) {
    if (!store_) return std::nullopt;

    sessions::Status status;
    auto original = store_->getSession(id, &status);
    if (!original) return std::nullopt;

    sessions::Session copy = *original;
    copy.id.reset();
    copy.name = toStd(toQ(original->name) + tr(" (copy)"));
    // created_at and last_connected are the new row's to earn: a duplicate has
    // never been connected to, whatever the original's history says. extras
    // rides along untouched, which is the opacity contract in sessions/store.h.
    copy.created_at.clear();
    copy.last_connected.clear();
    copy.connect_count = 0;

    auto newId = store_->addSession(copy, &status);
    if (!status) lastError_ = toQ(status.message);
    if (newId) reload();
    return newId;
}

void SessionTreeModel::recordConnect(qint64 id) {
    if (!store_) return;
    const sessions::Status status = store_->recordConnect(id);
    if (!status) lastError_ = toQ(status.message);
}

// ---------------------------------------------------------------------------
// QAbstractItemModel
// ---------------------------------------------------------------------------

QModelIndex SessionTreeModel::index(int row, int column,
                                    const QModelIndex &parent) const {
    if (column != 0 || row < 0) return {};
    Node *p = nodeFor(parent);
    if (!p || row >= static_cast<int>(p->children.size())) return {};
    return createIndex(row, column, p->children[row].get());
}

QModelIndex SessionTreeModel::parent(const QModelIndex &index) const {
    if (!index.isValid()) return {};
    Node *n = nodeFor(index);
    if (!n || !n->parent || n->parent == root_.get()) return {};
    return indexForNode(n->parent);
}

int SessionTreeModel::rowCount(const QModelIndex &parent) const {
    Node *p = nodeFor(parent);
    return p ? static_cast<int>(p->children.size()) : 0;
}

int SessionTreeModel::columnCount(const QModelIndex &) const { return 1; }

QVariant SessionTreeModel::data(const QModelIndex &index, int role) const {
    Node *n = nodeFor(index);
    if (!n || n == root_.get()) return {};

    switch (role) {
        case Qt::DisplayRole:
            if (n->kind == SessionItem && !n->description.isEmpty())
                return QStringLiteral("%1  (%2)").arg(n->name, n->description);
            return n->name;

        case Qt::ToolTipRole:
            if (n->kind == SessionItem)
                return QStringLiteral("%1:%2").arg(n->hostname).arg(n->port);
            return n->name;

        case Qt::DecorationRole: {
            QStyle *style = QApplication::style();
            if (!style) return {};
            return style->standardIcon(n->kind == FolderItem
                                           ? QStyle::SP_DirIcon
                                           : QStyle::SP_ComputerIcon);
        }

        case KindRole:
            return static_cast<int>(n->kind);
        case IdRole:
            return n->id;
        case NameRole:
            return n->name;
        case HostRole:
            return n->kind == SessionItem
                       ? QStringLiteral("%1:%2").arg(n->hostname).arg(n->port)
                       : QString();
        case ExpandedRole:
            return n->kind == FolderItem ? n->expanded : false;

        case DetailRole: {
            if (n->kind != SessionItem) return QString();
            // The live opinion wins when there is one, because "key
            // passphrase needed" is more useful than the address it replaces
            // and it is only ever there while it is true. The delegate must
            // not be the thing deciding which of the two it is looking at --
            // it draws whatever string this hands it.
            if (live_ && live_->has(n->id)) {
                const QString detail = live_->entry(n->id).detail;
                if (!detail.isEmpty()) return detail;
            }
            return storedDetail(n);
        }

        case LinkRole:
            if (n->kind != SessionItem || !live_) return 0;
            return static_cast<int>(live_->entry(n->id).link);

        case RttRole:
            if (n->kind != SessionItem || !live_) return -1;
            return live_->entry(n->id).rttMs;

        default:
            return {};
    }
}

Qt::ItemFlags SessionTreeModel::flags(const QModelIndex &index) const {
    // The invalid index is root, and root takes drops -- that is how something
    // is dragged out of a folder and back to the top level.
    if (!index.isValid()) return Qt::ItemIsDropEnabled;

    Node *n = nodeFor(index);
    if (!n) return Qt::NoItemFlags;

    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                      Qt::ItemIsDragEnabled;
    if (n->kind == FolderItem)
        f |= Qt::ItemIsDropEnabled;
    else
        f |= Qt::ItemNeverHasChildren;
    return f;
}

Qt::DropActions SessionTreeModel::supportedDropActions() const {
    return Qt::MoveAction;
}

QStringList SessionTreeModel::mimeTypes() const {
    return {QString::fromLatin1(kMimeType)};
}

QMimeData *SessionTreeModel::mimeData(const QModelIndexList &indexes) const {
    if (indexes.isEmpty()) return nullptr;

    // Single selection only, matching the view. Encoding a list here and
    // handling one on the other side is how a multi-select tree grows a
    // partial-move bug later.
    const QModelIndex first = indexes.first();
    Node *n = nodeFor(first);
    if (!n || n == root_.get()) return nullptr;

    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << static_cast<qint32>(n->kind) << static_cast<qint64>(n->id);

    auto *mime = new QMimeData;
    mime->setData(QString::fromLatin1(kMimeType), payload);
    return mime;
}

bool SessionTreeModel::canDropMimeData(const QMimeData *data,
                                       Qt::DropAction action, int, int,
                                       const QModelIndex &parent) const {
    if (action != Qt::MoveAction) return false;
    if (!data || !data->hasFormat(QString::fromLatin1(kMimeType))) return false;

    QByteArray payload = data->data(QString::fromLatin1(kMimeType));
    QDataStream stream(&payload, QIODevice::ReadOnly);
    qint32 kind = 0;
    qint64 id = -1;
    stream >> kind >> id;

    Node *target = nodeFor(parent);
    if (!target) return false;
    if (target != root_.get() && target->kind != FolderItem) return false;

    if (kind == FolderItem) {
        Node *moved = findNode(FolderItem, id);
        if (!moved) return false;
        // Into itself, or into anything beneath itself. Answered here rather
        // than in the drop so the cursor refuses instead of the drop being
        // accepted and then quietly doing nothing.
        if (isDescendantOf(target, moved)) return false;
    }

    return true;
}

bool SessionTreeModel::dropMimeData(const QMimeData *data,
                                    Qt::DropAction action, int row, int column,
                                    const QModelIndex &parent) {
    if (!store_) return false;
    if (!canDropMimeData(data, action, row, column, parent)) return false;

    QByteArray payload = data->data(QString::fromLatin1(kMimeType));
    QDataStream stream(&payload, QIODevice::ReadOnly);
    qint32 kindRaw = 0;
    qint64 id = -1;
    stream >> kindRaw >> id;
    const Kind kind = static_cast<Kind>(kindRaw);

    Node *target = nodeFor(parent);
    Node *moved = findNode(kind, id);
    if (!target || !moved) return false;

    // Where among the target's children the drop landed. row == -1 is a drop
    // ON the folder rather than between two rows, which means append.
    const int childCount = static_cast<int>(target->children.size());
    int at = (row < 0) ? childCount : std::min(row, childCount);

    // Count only same-kind siblings before the drop point, and ignore the
    // moved node if it is already here. Positions live in two tables and are
    // only ever compared within one, so a session dropped between two folders
    // means "first among the sessions", not "third child".
    int subIndex = 0;
    for (int i = 0; i < at && i < childCount; ++i) {
        Node *c = target->children[i].get();
        if (c == moved) continue;
        if (c->kind == kind) ++subIndex;
    }

    Node *sourceContainer = moved->parent ? moved->parent : root_.get();
    std::optional<int64_t> targetFolder;
    if (target != root_.get()) targetFolder = target->id;

    // Build the destination's same-kind sequence with the moved node inserted,
    // then write a contiguous run of positions over it. The source container
    // is renumbered too, so a gap left behind does not decide a later tie.
    std::vector<Node *> ordered;
    for (const std::unique_ptr<Node> &c : target->children) {
        if (c.get() == moved) continue;
        if (c->kind == kind) ordered.push_back(c.get());
    }
    subIndex = std::min(subIndex, static_cast<int>(ordered.size()));
    ordered.insert(ordered.begin() + subIndex, moved);

    bool ok = true;
    for (size_t i = 0; i < ordered.size(); ++i) {
        Node *n = ordered[i];
        const int position = static_cast<int>(i);

        if (kind == SessionItem) {
            sessions::Status status;
            auto s = store_->getSession(n->id, &status);
            if (!s) {
                ok = false;
                continue;
            }
            s->folder_id = targetFolder;
            s->position = position;
            const sessions::Status wrote = store_->updateSession(*s);
            if (!wrote) {
                lastError_ = toQ(wrote.message);
                ok = false;
            }
        } else {
            sessions::Status status;
            auto f = store_->getFolder(n->id, &status);
            if (!f) {
                ok = false;
                continue;
            }
            f->parent_id = targetFolder;
            f->position = position;
            const sessions::Status wrote = store_->updateFolder(*f);
            if (!wrote) {
                lastError_ = toQ(wrote.message);
                ok = false;
            }
        }
    }

    if (sourceContainer != target) reorderWithin(sourceContainer, moved);

    reload();
    return ok;
}

void SessionTreeModel::reorderWithin(Node *container, Node *skip) {
    if (!store_ || !container) return;

    for (const Kind kind : {FolderItem, SessionItem}) {
        int position = 0;
        for (const std::unique_ptr<Node> &c : container->children) {
            if (c->kind != kind || c.get() == skip) continue;

            if (kind == SessionItem) {
                sessions::Status status;
                auto s = store_->getSession(c->id, &status);
                if (!s) continue;
                if (s->position == position) {
                    ++position;
                    continue;
                }
                s->position = position;
                store_->updateSession(*s);
            } else {
                sessions::Status status;
                auto f = store_->getFolder(c->id, &status);
                if (!f) continue;
                if (f->position == position) {
                    ++position;
                    continue;
                }
                f->position = position;
                store_->updateFolder(*f);
            }
            ++position;
        }
    }
}

}  // namespace omega::app
