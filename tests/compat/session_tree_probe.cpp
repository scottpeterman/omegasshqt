// tests/compat/session_tree_probe.cpp
//
// Drives the session tree model and its filter with no display.
//
//   session_tree_probe [db-path]
//
// The claims 4d makes that cannot be checked by reading are the ones about
// what a drop WRITES: which folder a row lands in, what positions the rows
// around it are left holding, and whether any of it survives a reload. A drop
// is a model call -- dropMimeData with a mime payload, a row and a parent --
// so all of that runs without a window, a mouse, or a window manager.
//
// The filter is checked for the two failures that a tree filter actually has:
// a hit whose folder disappeared with it, and a matched folder that came back
// empty. Both are visible as row counts.
//
// A throwaway database, removed on the way in. Never ~/.omega/sessions.db.

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMimeData>
#include <QModelIndex>

#include <cstdio>
#include <optional>
#include <string>

#include "app/sessionfilterproxy.h"
#include "app/sessiontreemodel.h"
#include "sessions/store.h"

using omega::app::SessionFilterProxy;
using omega::app::SessionTreeModel;
namespace sessions = omega::sessions;

namespace {

int failures = 0;

void ok(bool condition, const char *what, const std::string &detail = {}) {
    if (condition) {
        std::printf("  ok    %-46s %s\n", what, detail.c_str());
    } else {
        std::printf("  FAIL  %-46s %s\n", what, detail.c_str());
        ++failures;
    }
}

void heading(const char *text) { std::printf("\n%s\n", text); }

qint64 addFolder(sessions::SessionStore &store, const char *name,
                 std::optional<int64_t> parent = {}) {
    auto id = store.addFolder(name, parent);
    return id ? *id : -1;
}

qint64 addSession(sessions::SessionStore &store, const char *name,
                  const char *host, std::optional<int64_t> folder) {
    sessions::Session s;
    s.name = name;
    s.hostname = host;
    s.port = 22;
    s.folder_id = folder;
    auto id = store.addSession(s);
    return id ? *id : -1;
}

// The move a user makes with a mouse, made without one: build the payload the
// model would have produced for `index`, then hand it back to the model as a
// drop on `parent` at `row`.
bool drop(SessionTreeModel &model, const QModelIndex &index,
          const QModelIndex &parent, int row) {
    QMimeData *mime = model.mimeData({index});
    if (!mime) return false;
    const bool result =
        model.dropMimeData(mime, Qt::MoveAction, row, 0, parent);
    delete mime;
    return result;
}

int positionOfSession(sessions::SessionStore &store, qint64 id) {
    auto s = store.getSession(id);
    return s ? s->position : -1;
}

std::optional<int64_t> folderOfSession(sessions::SessionStore &store,
                                       qint64 id) {
    auto s = store.getSession(id);
    return s ? s->folder_id : std::nullopt;
}

}  // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    const QString dbPath =
        argc > 1 ? QString::fromLocal8Bit(argv[1])
                 : QDir::temp().filePath(QStringLiteral("omega-tree-probe.db"));
    QFile::remove(dbPath);

    sessions::Status status;
    auto store = sessions::SessionStore::open(dbPath.toStdString(), &status);
    if (!store) {
        std::fprintf(stderr, "cannot open %s: %s\n", qPrintable(dbPath),
                     status.message.c_str());
        return 2;
    }

    // A small lab tree: two folders, one nested, and sessions at every level.
    const qint64 core = addFolder(*store, "core");
    const qint64 access = addFolder(*store, "access");
    const qint64 rack2 = addFolder(*store, "rack2", access);

    const qint64 coreA = addSession(*store, "lab-core-1", "10.10.0.1", core);
    const qint64 coreB = addSession(*store, "lab-core-2", "10.10.0.2", core);
    const qint64 coreC = addSession(*store, "lab-core-3", "10.10.0.3", core);
    const qint64 leaf = addSession(*store, "lab-leaf-1", "10.10.2.1", rack2);
    const qint64 loose = addSession(*store, "lab-jump-1", "10.10.9.9", {});

    SessionTreeModel model(store.get());

    // --- shape ------------------------------------------------------------
    heading("tree shape");

    ok(model.rowCount({}) == 3, "root holds two folders and one session",
       std::to_string(model.rowCount({})));

    const QModelIndex coreIdx = model.indexForFolder(core);
    const QModelIndex accessIdx = model.indexForFolder(access);
    const QModelIndex rack2Idx = model.indexForFolder(rack2);

    ok(coreIdx.isValid() && accessIdx.isValid(), "both root folders resolve");
    ok(rack2Idx.isValid() && rack2Idx.parent() == accessIdx,
       "rack2 is nested under access");
    ok(model.rowCount(coreIdx) == 3, "core holds three sessions",
       std::to_string(model.rowCount(coreIdx)));

    // Folders sort before sessions in a container, which is the order
    // refresh() builds them in and the order this model reproduces.
    ok(model.kindOf(model.index(0, 0, {})) == SessionTreeModel::FolderItem &&
           model.kindOf(model.index(2, 0, {})) == SessionTreeModel::SessionItem,
       "folders sort before sessions at root");

    // --- reordering within a folder ---------------------------------------
    heading("reorder within a folder");

    // Drag the third session to the top of its own folder. nterm-qt writes
    // this and then loses it, because get_tree re-sorts sessions by name; the
    // check that it survives a reload is the check that Omega reads the column
    // it writes.
    const QModelIndex coreCIdx = model.indexForSession(coreC);
    ok(drop(model, coreCIdx, coreIdx, 0), "drop lab-core-3 at the top of core");

    ok(positionOfSession(*store, coreC) == 0, "lab-core-3 took position 0",
       std::to_string(positionOfSession(*store, coreC)));
    ok(positionOfSession(*store, coreA) == 1 &&
           positionOfSession(*store, coreB) == 2,
       "the other two were renumbered behind it");

    ok(model.idOf(model.index(0, 0, model.indexForFolder(core))) == coreC,
       "and the model shows it first after the reload");

    // The reload the drop already did is the model's own. This one is a fresh
    // model over the same file: the ordering has to come out of the database,
    // not out of a node tree that happened to be left in the right shape.
    {
        SessionTreeModel reopened(store.get());
        ok(reopened.idOf(reopened.index(0, 0, reopened.indexForFolder(core))) ==
               coreC,
           "and again in a model built fresh from the file");
    }

    // --- moving between folders -------------------------------------------
    heading("move between folders");

    ok(drop(model, model.indexForSession(loose), model.indexForFolder(rack2),
            -1),
       "drop the root session into rack2");

    ok(folderOfSession(*store, loose) == std::optional<int64_t>(rack2),
       "it is filed in rack2 now");
    ok(model.rowCount({}) == 2, "root is down to the two folders",
       std::to_string(model.rowCount({})));
    ok(model.rowCount(model.indexForFolder(rack2)) == 2,
       "rack2 holds two sessions");

    // Appended, not inserted: row -1 is a drop ON the folder.
    ok(positionOfSession(*store, loose) > positionOfSession(*store, leaf),
       "and it landed after the session already there");

    // Back out to root, which is the drop that only works if the invalid
    // index is drop-enabled.
    ok(drop(model, model.indexForSession(loose), QModelIndex(), -1),
       "drop it back out to the top level");
    ok(!folderOfSession(*store, loose).has_value(), "its folder is null again");

    // --- refusals ---------------------------------------------------------
    heading("refusals");

    // A folder into its own subtree. move_folder raises on this; here it has
    // to be answered before the drop is accepted, so the cursor refuses.
    {
        QMimeData *mime = model.mimeData({model.indexForFolder(access)});
        ok(mime && !model.canDropMimeData(mime, Qt::MoveAction, -1, 0,
                                          model.indexForFolder(rack2)),
           "a folder is refused into its own subtree");
        delete mime;
    }
    {
        QMimeData *mime = model.mimeData({model.indexForFolder(access)});
        ok(mime && !model.canDropMimeData(mime, Qt::MoveAction, -1, 0,
                                          model.indexForFolder(access)),
           "and into itself");
        delete mime;
    }

    // A session is not a container. The flag says so; the check is that
    // canDropMimeData agrees, since the two are read by different parts of the
    // view.
    {
        QMimeData *mime = model.mimeData({model.indexForSession(coreA)});
        ok(mime && !model.canDropMimeData(mime, Qt::MoveAction, -1, 0,
                                          model.indexForSession(coreB)),
           "nothing drops onto a session");
        delete mime;
    }

    ok(!(model.flags(model.indexForSession(coreA)) & Qt::ItemIsDropEnabled),
       "sessions are not drop targets");
    ok((model.flags(QModelIndex()) & Qt::ItemIsDropEnabled) != 0,
       "root is a drop target");

    // --- expansion --------------------------------------------------------
    heading("expansion");

    model.setExpanded(model.indexForFolder(core), false);
    {
        auto folder = store->getFolder(core);
        ok(folder && !folder->expanded, "collapse reached folders.expanded");
    }
    {
        SessionTreeModel reopened(store.get());
        ok(!reopened.isExpanded(reopened.indexForFolder(core)),
           "and a fresh model reads it back collapsed");
    }
    model.setExpanded(model.indexForFolder(core), true);

    // --- filter -----------------------------------------------------------
    heading("filter");

    SessionFilterProxy proxy;
    proxy.setSourceModel(&model);

    ok(!proxy.isFiltering(), "an empty filter is not filtering");
    ok(proxy.rowCount({}) == model.rowCount({}),
       "and shows every root row");

    // A session hit inside a folder. The folder must survive with it -- that
    // is the "without flattening" half, and the failure it guards against is a
    // hit that has nowhere to be drawn.
    proxy.setFilterText(QStringLiteral("leaf"));
    ok(proxy.rowCount({}) == 1, "one root row survives a session-only match",
       std::to_string(proxy.rowCount({})));
    {
        const QModelIndex accessProxy = proxy.index(0, 0, {});
        ok(proxy.data(accessProxy, SessionTreeModel::NameRole).toString() ==
               QStringLiteral("access"),
           "and it is the ancestor folder, not the session");
        const QModelIndex rack2Proxy = proxy.index(0, 0, accessProxy);
        ok(proxy.rowCount(rack2Proxy) == 1,
           "with exactly the matching session under it",
           std::to_string(proxy.rowCount(rack2Proxy)));
    }

    // A folder matched by name shows its contents. nterm-qt hides them: a
    // folder's visibility there is decided only by its children, so a matched
    // folder whose sessions do not match comes back empty.
    proxy.setFilterText(QStringLiteral("core"));
    {
        const QModelIndex coreProxy = proxy.index(0, 0, {});
        ok(proxy.data(coreProxy, SessionTreeModel::NameRole).toString() ==
               QStringLiteral("core"),
           "a folder matched by name is shown");
        ok(proxy.rowCount(coreProxy) == 3,
           "with its sessions, which did not match on their own",
           std::to_string(proxy.rowCount(coreProxy)));
    }

    // Hostname is one of the three fields, and the one a name search misses.
    proxy.setFilterText(QStringLiteral("10.10.2."));
    ok(proxy.rowCount({}) == 1, "a hostname substring matches");

    proxy.setFilterText(QStringLiteral("nothing-matches-this"));
    ok(proxy.rowCount({}) == 0, "a miss empties the tree");

    proxy.setFilterText(QString());
    ok(proxy.rowCount({}) == model.rowCount({}),
       "and clearing it brings everything back");

    // --- editing ----------------------------------------------------------
    heading("editing");

    {
        auto s = store->getSession(coreA);
        ok(s.has_value(), "a session reads back");
        s->name = "lab-core-1a";
        s->description = "renamed";
        ok(model.updateSession(*s), "update writes through");
        ok(model.data(model.indexForSession(coreA), Qt::DisplayRole)
                   .toString() == QStringLiteral("lab-core-1a  (renamed)"),
           "and the display picks up name and description");
    }

    {
        auto copy = model.duplicateSession(coreA);
        ok(copy.has_value(), "duplicate returns a new id");
        auto s = copy ? store->getSession(*copy) : std::nullopt;
        ok(s && s->connect_count == 0 && s->last_connected.empty(),
           "and the copy has no connect history of its own");
        ok(s && s->folder_id == std::optional<int64_t>(core),
           "and is filed beside the original");
        if (copy) model.deleteSession(*copy);
    }

    // delete_folder reparents rather than cascading, because foreign keys are
    // off on a file nterm-qt also opens. The confirmation text says so; this
    // is the check that the text is true.
    {
        const int before = model.rowCount({});
        ok(model.deleteFolder(rack2), "delete a nested folder");
        ok(!folderOfSession(*store, leaf).has_value(),
           "its session moved to the top level, not deleted");
        ok(model.rowCount({}) == before + 1,
           "which is one more row at root",
           std::to_string(model.rowCount({})));
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
