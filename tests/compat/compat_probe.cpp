/* tests/compat/compat_probe.cpp
 *
 * The C++ twin of tests/compat/oracle.py.
 *
 * Same subcommands, same arguments, same output bytes. The differential driver
 * runs one of each against copies of one database and diffs; anywhere the two
 * disagree is a place the two applications would disagree about a file they
 * both open.
 *
 * Everything here goes through omega::sessions::SessionStore, except dump-raw,
 * which reads the database directly for the same reason the Python version
 * does: after a mutation the question is what is on disk, not what either
 * implementation believes it wrote.
 */

#include "../../sessions/store.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace omega::sessions;

namespace {

const char *kNull = "\\N";

std::string opt(const std::optional<int64_t> &v) {
    return v ? std::to_string(*v) : kNull;
}

std::string opt(const std::optional<std::string> &v) {
    return v ? *v : kNull;
}

std::optional<int64_t> parseFolder(const char *arg) {
    if (std::strcmp(arg, "-") == 0) return std::nullopt;
    return std::stoll(arg);
}

int fail(const std::string &msg) {
    std::cerr << "error: " << msg << "\n";
    return 1;
}

// dump-raw: every row by id, straight from SQL.
int dumpRaw(const char *path) {
    sqlite3 *db = nullptr;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return fail("open database");
    }

    sqlite3_stmt *st = nullptr;
    const char *fsql =
        "SELECT id, name, parent_id, position, expanded FROM folders ORDER BY id";
    if (sqlite3_prepare_v2(db, fsql, -1, &st, nullptr) != SQLITE_OK)
        return fail(sqlite3_errmsg(db));
    while (sqlite3_step(st) == SQLITE_ROW) {
        std::printf("folder\t%lld\t%s\t%s\t%d\t%d\n",
                    (long long)sqlite3_column_int64(st, 0),
                    sqlite3_column_text(st, 1),
                    sqlite3_column_type(st, 2) == SQLITE_NULL
                        ? kNull
                        : std::to_string(sqlite3_column_int64(st, 2)).c_str(),
                    sqlite3_column_int(st, 3), sqlite3_column_int(st, 4));
    }
    sqlite3_finalize(st);

    const char *ssql =
        "SELECT id, name, description, hostname, port, credential_name, "
        "folder_id, position, connect_count, extras FROM sessions ORDER BY id";
    if (sqlite3_prepare_v2(db, ssql, -1, &st, nullptr) != SQLITE_OK)
        return fail(sqlite3_errmsg(db));
    while (sqlite3_step(st) == SQLITE_ROW) {
        const std::string cred =
            sqlite3_column_type(st, 5) == SQLITE_NULL
                ? kNull
                : reinterpret_cast<const char *>(sqlite3_column_text(st, 5));
        const std::string folder =
            sqlite3_column_type(st, 6) == SQLITE_NULL
                ? kNull
                : std::to_string(sqlite3_column_int64(st, 6));
        std::printf("session\t%lld\t%s\t%s\t%s\t%d\t%s\t%s\t%d\t%d\t%s\n",
                    (long long)sqlite3_column_int64(st, 0),
                    sqlite3_column_text(st, 1), sqlite3_column_text(st, 2),
                    sqlite3_column_text(st, 3), sqlite3_column_int(st, 4),
                    cred.c_str(), folder.c_str(), sqlite3_column_int(st, 7),
                    sqlite3_column_int(st, 8), sqlite3_column_text(st, 9));
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return 0;
}

// dump-ordered: the tree as the store hands it out, depth first.
void walk(SessionStore &store, std::optional<int64_t> parent, int depth) {
    for (const Folder &f : store.listFolders(parent)) {
        std::printf("folder\t%d\t%lld\t%s\t%d\n", depth, (long long)*f.id,
                    f.name.c_str(), f.position);
        walk(store, f.id, depth + 1);
    }
    for (const Session &s : store.listSessions(parent)) {
        std::printf("session\t%d\t%lld\t%s\t%d\n", depth, (long long)*s.id,
                    s.name.c_str(), s.position);
    }
}

int usage() {
    std::cerr <<
        "usage: compat_probe <command> <db> [args]\n"
        "  dump-raw                          every row, by id\n"
        "  dump-ordered                      tree order, depth first\n"
        "  delete-folder <id>\n"
        "  add-session <name> <host> <folder|-> <extras-json>\n"
        "  resave-all                        read and write back unchanged\n"
        "  record-connect <id>\n"
        "  move-session <id> <folder|->\n"
        "  move-folder <id> <parent|->\n"
        "  timestamps                        created_at / last_connected\n";
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) return usage();

    const std::string cmd = argv[1];
    const char *dbPath = argv[2];

    if (cmd == "dump-raw") return dumpRaw(dbPath);

    Status st;
    auto store = SessionStore::open(dbPath, &st);
    if (!store) return fail(st.message);

    if (cmd == "create") {
        // Opening already creates the schema; this exists so the intent is
        // visible in the test rather than being a side effect of a dump.
        return 0;
    }

    if (cmd == "schema") {
        // The DDL as SQLite recorded it. Whoever created the file decided
        // this text, and the other application then runs CREATE TABLE IF NOT
        // EXISTS over it -- so if the two DDLs ever drift, the second
        // application silently keeps whatever the first one wrote.
        sqlite3 *raw = nullptr;
        if (sqlite3_open(dbPath, &raw) != SQLITE_OK) {
            sqlite3_close(raw);
            return fail("open database");
        }
        sqlite3_stmt *q = nullptr;
        const char *sql = "SELECT type, name, sql FROM sqlite_master "
                          "WHERE sql IS NOT NULL ORDER BY type, name";
        if (sqlite3_prepare_v2(raw, sql, -1, &q, nullptr) != SQLITE_OK)
            return fail(sqlite3_errmsg(raw));
        while (sqlite3_step(q) == SQLITE_ROW) {
            std::printf("%s\t%s\t%s\n", sqlite3_column_text(q, 0),
                        sqlite3_column_text(q, 1), sqlite3_column_text(q, 2));
        }
        sqlite3_finalize(q);
        sqlite3_close(raw);
        return 0;
    }

    if (cmd == "dump-ordered") {
        walk(*store, std::nullopt, 0);
        return 0;
    }

    if (cmd == "delete-folder" && argc == 4) {
        Status s = store->deleteFolder(std::stoll(argv[3]));
        return s ? 0 : fail(s.message);
    }

    if (cmd == "add-session" && argc == 7) {
        Session sess;
        sess.name = argv[3];
        sess.hostname = argv[4];
        sess.folder_id = parseFolder(argv[5]);
        sess.extras = argv[6];
        auto id = store->addSession(sess, &st);
        if (!id) return fail(st.message);
        std::printf("%lld\n", (long long)*id);
        return 0;
    }

    if (cmd == "resave-all") {
        for (Session s : store->listAllSessions()) {
            Status r = store->updateSession(s);
            if (!r) return fail(r.message);
        }
        return 0;
    }

    if (cmd == "record-connect" && argc == 4) {
        Status s = store->recordConnect(std::stoll(argv[3]));
        return s ? 0 : fail(s.message);
    }

    if (cmd == "move-session" && argc == 5) {
        Status s = store->moveSession(std::stoll(argv[3]), parseFolder(argv[4]));
        return s ? 0 : fail(s.message);
    }

    if (cmd == "move-folder" && argc == 5) {
        Status s = store->moveFolder(std::stoll(argv[3]), parseFolder(argv[4]));
        // A rejected cycle is a result, not a crash -- the Python raises
        // ValueError and the tree catches it, so both sides print and exit 0.
        std::printf("%s\n", s ? "ok" : ("rejected: " + s.message).c_str());
        return 0;
    }

    if (cmd == "timestamps") {
        // By id, matching the oracle. listAllSessions orders by name, which is
        // get_tree's ordering and the wrong one for a row-by-row comparison.
        std::vector<Session> all = store->listAllSessions();
        std::sort(all.begin(), all.end(),
                  [](const Session &a, const Session &b) { return *a.id < *b.id; });
        for (const Session &s : all) {
            std::printf("%lld\t%s\t%s\n", (long long)*s.id,
                        s.created_at.empty() ? kNull : s.created_at.c_str(),
                        s.last_connected.empty() ? kNull
                                                 : s.last_connected.c_str());
        }
        return 0;
    }

    return usage();
}
