// sessions/store.cpp

#include "store.h"

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <utility>
#include <vector>

namespace omega::sessions {
namespace {

// The DDL, byte-identical to ntermqt/manager/models.py. Both applications run
// it IF NOT EXISTS, so whichever opens a fresh file first, the other agrees.
constexpr const char *kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS folders (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    parent_id INTEGER REFERENCES folders(id) ON DELETE CASCADE,
    position INTEGER DEFAULT 0,
    expanded INTEGER DEFAULT 1
);

CREATE TABLE IF NOT EXISTS sessions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    description TEXT DEFAULT '',
    hostname TEXT NOT NULL,
    port INTEGER DEFAULT 22,
    credential_name TEXT,
    folder_id INTEGER REFERENCES folders(id) ON DELETE SET NULL,
    position INTEGER DEFAULT 0,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    last_connected TIMESTAMP,
    connect_count INTEGER DEFAULT 0,
    extras TEXT DEFAULT '{}'
);

CREATE INDEX IF NOT EXISTS idx_sessions_folder ON sessions(folder_id);
CREATE INDEX IF NOT EXISTS idx_folders_parent ON folders(parent_id);
)SQL";

// Columns added after the port. Applied by ALTER TABLE on open rather than
// written into kSchema above, so a database written by a real nterm-qt gains
// them on first open with every row intact -- and so the base DDL stays the
// Python's, which is what a fresh file should still be created with.
//
// SQLite's ALTER TABLE ADD COLUMN is O(1): it records the new column in the
// schema and synthesises the default for existing rows on read. Nothing is
// rewritten, so this stays cheap on a store with a thousand sessions in it.
//
// NO DEFAULTS except transport. A default is exactly what these columns must
// not have -- NULL is the inherit state, and a column defaulted to 0 would
// make every pre-existing session claim it had opted out of anti-idle. See
// the note on Session in the header.
struct AddedColumn {
    const char *name;
    const char *decl;
};

constexpr AddedColumn kAddedColumns[] = {
    // Defaulted, and the only one that is: every row that predates this
    // column was an ssh session, and NULL would mean "inherit" from nothing.
    {"transport", "TEXT DEFAULT 'ssh'"},

    {"term_type", "TEXT"},
    {"scrollback_lines", "INTEGER"},
    {"multiline_paste_threshold", "INTEGER"},
    {"paste_baud", "INTEGER"},

    {"host_key_policy", "TEXT"},
    {"legacy_algorithms", "INTEGER"},
    {"username", "TEXT"},

    {"jump_host", "TEXT"},
    {"jump_port", "INTEGER"},
    {"jump_username", "TEXT"},
    {"jump_credential", "TEXT"},

    {"anti_idle_enabled", "INTEGER"},
    {"anti_idle_seconds", "INTEGER"},
    {"anti_idle_keystroke", "TEXT"},
    {"anti_idle_custom", "TEXT"},

    {"serial_port", "TEXT"},
    {"serial_baud", "INTEGER"},
    {"serial_data_bits", "INTEGER"},
    {"serial_parity", "TEXT"},
    {"serial_stop_bits", "TEXT"},

    {"telnet_crlf", "INTEGER"},

    {"wheel_alt_screen", "INTEGER"},

    {"font_size", "INTEGER"},
    {"theme_name", "TEXT"},
};

void setStatus(Status *out, Status s) {
    if (out) *out = std::move(s);
}

Status sqliteError(sqlite3 *db, const char *what) {
    return Status::fail(std::string(what) + ": " +
                        (db ? sqlite3_errmsg(db) : "no database"));
}

// A bound NULL compares correctly under `IS ?`, which is why every parent and
// folder lookup uses that form. `= ?` with NULL matches nothing, and the bug
// it produces is a root-level item that silently disappears from its own list.
void bindOptInt(sqlite3_stmt *st, int idx, const std::optional<int64_t> &v) {
    if (v) sqlite3_bind_int64(st, idx, *v);
    else sqlite3_bind_null(st, idx);
}

void bindOptText(sqlite3_stmt *st, int idx, const std::optional<std::string> &v) {
    if (v) sqlite3_bind_text(st, idx, v->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, idx);
}

void bindText(sqlite3_stmt *st, int idx, const std::string &v) {
    sqlite3_bind_text(st, idx, v.c_str(), -1, SQLITE_TRANSIENT);
}

std::string colText(sqlite3_stmt *st, int idx) {
    const unsigned char *p = sqlite3_column_text(st, idx);
    return p ? reinterpret_cast<const char *>(p) : std::string();
}

std::optional<std::string> colOptText(sqlite3_stmt *st, int idx) {
    if (sqlite3_column_type(st, idx) == SQLITE_NULL) return std::nullopt;
    return colText(st, idx);
}

std::optional<int64_t> colOptInt(sqlite3_stmt *st, int idx) {
    if (sqlite3_column_type(st, idx) == SQLITE_NULL) return std::nullopt;
    return sqlite3_column_int64(st, idx);
}

void bindOptInt32(sqlite3_stmt *st, int idx, const std::optional<int> &v) {
    if (v) sqlite3_bind_int(st, idx, *v);
    else sqlite3_bind_null(st, idx);
}

// A bool crosses as 1 or 0; absent crosses as NULL. The three states are the
// point -- see the note on Session in the header.
void bindOptBool(sqlite3_stmt *st, int idx, const std::optional<bool> &v) {
    if (v) sqlite3_bind_int(st, idx, *v ? 1 : 0);
    else sqlite3_bind_null(st, idx);
}

std::optional<int> colOptInt32(sqlite3_stmt *st, int idx) {
    if (sqlite3_column_type(st, idx) == SQLITE_NULL) return std::nullopt;
    return sqlite3_column_int(st, idx);
}

std::optional<bool> colOptBool(sqlite3_stmt *st, int idx) {
    if (sqlite3_column_type(st, idx) == SQLITE_NULL) return std::nullopt;
    return sqlite3_column_int(st, idx) != 0;
}

Folder readFolder(sqlite3_stmt *st) {
    Folder f;
    f.id = sqlite3_column_int64(st, 0);
    f.name = colText(st, 1);
    f.parent_id = colOptInt(st, 2);
    f.position = sqlite3_column_int(st, 3);
    f.expanded = sqlite3_column_int(st, 4) != 0;
    return f;
}

Session readSession(sqlite3_stmt *st) {
    Session s;
    s.id = sqlite3_column_int64(st, 0);
    s.name = colText(st, 1);
    s.description = colText(st, 2);
    s.hostname = colText(st, 3);
    s.port = sqlite3_column_int(st, 4);
    s.credential_name = colOptText(st, 5);
    s.folder_id = colOptInt(st, 6);
    s.position = sqlite3_column_int(st, 7);
    s.created_at = colText(st, 8);
    s.last_connected = colText(st, 9);
    s.connect_count = sqlite3_column_int(st, 10);
    s.extras = colText(st, 11);

    s.transport = transportFromName(colText(st, 12));
    s.term_type = colOptText(st, 13);
    s.scrollback_lines = colOptInt32(st, 14);
    s.multiline_paste_threshold = colOptInt32(st, 15);
    s.paste_baud = colOptInt32(st, 16);
    s.host_key_policy = colOptText(st, 17);
    s.legacy_algorithms = colOptBool(st, 18);
    s.username = colOptText(st, 19);
    s.jump_host = colOptText(st, 20);
    s.jump_port = colOptInt32(st, 21);
    s.jump_username = colOptText(st, 22);
    s.jump_credential = colOptText(st, 23);
    s.anti_idle_enabled = colOptBool(st, 24);
    s.anti_idle_seconds = colOptInt32(st, 25);
    s.anti_idle_keystroke = colOptText(st, 26);
    s.anti_idle_custom = colOptText(st, 27);
    s.serial_port = colOptText(st, 28);
    s.serial_baud = colOptInt32(st, 29);
    s.serial_data_bits = colOptInt32(st, 30);
    s.serial_parity = colOptText(st, 31);
    s.serial_stop_bits = colOptText(st, 32);
    s.telnet_crlf = colOptBool(st, 33);
    s.wheel_alt_screen = colOptBool(st, 34);
    s.font_size = colOptInt32(st, 35);
    s.theme_name = colOptText(st, 36);
    return s;
}

// Explicit column lists rather than SELECT *, so a column added by a future
// nterm-qt cannot shift the indices this file reads by position.
constexpr const char *kFolderCols =
    "id, name, parent_id, position, expanded";
// The twelve original columns, then kAddedColumns in kAddedColumns order.
// readSession() reads by position, so the two orders are one fact stated
// twice -- session_attrs_probe writes every field and reads it back, which is
// what catches them drifting apart.
constexpr const char *kSessionCols =
    "id, name, description, hostname, port, credential_name, folder_id, "
    "position, created_at, last_connected, connect_count, extras, "
    "transport, term_type, scrollback_lines, multiline_paste_threshold, "
    "paste_baud, host_key_policy, legacy_algorithms, username, "
    "jump_host, jump_port, jump_username, jump_credential, "
    "anti_idle_enabled, anti_idle_seconds, anti_idle_keystroke, "
    "anti_idle_custom, serial_port, serial_baud, serial_data_bits, "
    "serial_parity, serial_stop_bits, telnet_crlf, wheel_alt_screen, "
    "font_size, theme_name";

// The writable ones: everything above except id and the three the store owns
// (created_at, last_connected, connect_count).
constexpr const char *kSessionWriteCols =
    "name, description, hostname, port, credential_name, folder_id, "
    "position, extras, "
    "transport, term_type, scrollback_lines, multiline_paste_threshold, "
    "paste_baud, host_key_policy, legacy_algorithms, username, "
    "jump_host, jump_port, jump_username, jump_credential, "
    "anti_idle_enabled, anti_idle_seconds, anti_idle_keystroke, "
    "anti_idle_custom, serial_port, serial_baud, serial_data_bits, "
    "serial_parity, serial_stop_bits, telnet_crlf, wheel_alt_screen, "
    "font_size, theme_name";

// Binds every writable column starting at `base` (1 for the first). Shared
// by addSession and updateSession so the two cannot disagree about order --
// they did not share one before, and there were eight columns to get wrong.
// The count is no longer written down anywhere: both statements derive it
// from kSessionWriteCols, and this function walks the same order.
void bindWritable(sqlite3_stmt *st, int base, const Session &s, int position) {
    int i = base;
    bindText(st, i++, s.name);
    bindText(st, i++, s.description);
    bindText(st, i++, s.hostname);
    sqlite3_bind_int(st, i++, s.port);
    bindOptText(st, i++, s.credential_name);
    bindOptInt(st, i++, s.folder_id);
    sqlite3_bind_int(st, i++, position);
    bindText(st, i++, s.extras);

    bindText(st, i++, transportName(s.transport));
    bindOptText(st, i++, s.term_type);
    bindOptInt32(st, i++, s.scrollback_lines);
    bindOptInt32(st, i++, s.multiline_paste_threshold);
    bindOptInt32(st, i++, s.paste_baud);
    bindOptText(st, i++, s.host_key_policy);
    bindOptBool(st, i++, s.legacy_algorithms);
    bindOptText(st, i++, s.username);
    bindOptText(st, i++, s.jump_host);
    bindOptInt32(st, i++, s.jump_port);
    bindOptText(st, i++, s.jump_username);
    bindOptText(st, i++, s.jump_credential);
    bindOptBool(st, i++, s.anti_idle_enabled);
    bindOptInt32(st, i++, s.anti_idle_seconds);
    bindOptText(st, i++, s.anti_idle_keystroke);
    bindOptText(st, i++, s.anti_idle_custom);
    bindOptText(st, i++, s.serial_port);
    bindOptInt32(st, i++, s.serial_baud);
    bindOptInt32(st, i++, s.serial_data_bits);
    bindOptText(st, i++, s.serial_parity);
    bindOptText(st, i++, s.serial_stop_bits);
    bindOptBool(st, i++, s.telnet_crlf);
    bindOptBool(st, i++, s.wheel_alt_screen);
    bindOptInt32(st, i++, s.font_size);
    bindOptText(st, i++, s.theme_name);
}

// Next position among siblings: MAX(position) + 1, or 0 when the group is
// empty. COALESCE(MAX(position), -1) + 1 is the Python's exact expression.
int nextPosition(sqlite3 *db, const char *table, const char *parent_col,
                 const std::optional<int64_t> &parent) {
    std::string sql = std::string("SELECT COALESCE(MAX(position), -1) + 1 FROM ") +
                      table + " WHERE " + parent_col + " IS ?";
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return 0;
    bindOptInt(st, 1, parent);
    int pos = 0;
    if (sqlite3_step(st) == SQLITE_ROW) pos = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return pos;
}

Status execSimple(sqlite3 *db, const char *sql) {
    char *err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        Status s = Status::fail(err ? err : "exec failed");
        sqlite3_free(err);
        return s;
    }
    return Status::good();
}

// Adds any of kAddedColumns the sessions table does not already have.
//
// PRAGMA table_info rather than catching the "duplicate column name" error
// from ALTER TABLE: the error is a string comparison away from every other
// error sqlite3_exec can return, and asking first costs one statement.
Status migrate(sqlite3 *db) {
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(sessions)", -1, &st,
                           nullptr) != SQLITE_OK)
        return sqliteError(db, "prepare table_info");

    std::vector<std::string> have;
    while (sqlite3_step(st) == SQLITE_ROW) have.push_back(colText(st, 1));
    sqlite3_finalize(st);

    // One transaction: a store that gained half its columns and then hit a
    // full disk is one every later read has to be defensive about.
    if (Status s = execSimple(db, "BEGIN"); !s) return s;

    for (const AddedColumn &col : kAddedColumns) {
        if (std::find(have.begin(), have.end(), col.name) != have.end())
            continue;
        const std::string sql = std::string("ALTER TABLE sessions ADD COLUMN ") +
                                col.name + " " + col.decl;
        if (Status s = execSimple(db, sql.c_str()); !s) {
            execSimple(db, "ROLLBACK");
            return Status::fail("add column " + std::string(col.name) + ": " +
                                s.message);
        }
    }

    return execSimple(db, "COMMIT");
}

} // namespace

const char *transportName(SessionTransport transport) {
    switch (transport) {
        case SessionTransport::Telnet: return "telnet";
        case SessionTransport::Serial: return "serial";
        case SessionTransport::Ssh:    break;
    }
    return "ssh";
}

// An unrecognised name yields ssh rather than failing the row. A session that
// opens over the wrong transport is recoverable by looking at it; one that
// refuses to appear in the tree at all is not.
SessionTransport transportFromName(const std::string &name, bool *known) {
    if (known) *known = true;
    if (name == "telnet") return SessionTransport::Telnet;
    if (name == "serial") return SessionTransport::Serial;
    if (name != "ssh" && known) *known = false;
    return SessionTransport::Ssh;
}

std::unique_ptr<SessionStore> SessionStore::open(const std::string &path,
                                                 Status *status) {
    std::error_code ec;
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
        if (ec) {
            setStatus(status, Status::fail("create parent directory: " + ec.message()));
            return nullptr;
        }
    }

    sqlite3 *db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        setStatus(status, sqliteError(db, "open database"));
        sqlite3_close(db);
        return nullptr;
    }

    // No PRAGMA foreign_keys. See the note on open() in the header -- enabling
    // it here would make deleteFolder destroy subtrees the Python preserves.

    if (Status s = execSimple(db, kSchema); !s) {
        setStatus(status, Status::fail("create schema: " + s.message));
        sqlite3_close(db);
        return nullptr;
    }

    // Additive, idempotent, and run on every open including a fresh one, so
    // there is a single schema path rather than one for new files and one for
    // migrated ones.
    if (Status s = migrate(db); !s) {
        setStatus(status, Status::fail("migrate schema: " + s.message));
        sqlite3_close(db);
        return nullptr;
    }

    std::unique_ptr<SessionStore> store(new SessionStore());
    store->db_ = db;
    setStatus(status, Status::good());
    return store;
}

SessionStore::~SessionStore() {
    if (db_) sqlite3_close(db_);
}

// --- folders --------------------------------------------------------------

std::optional<int64_t> SessionStore::addFolder(const std::string &name,
                                               std::optional<int64_t> parent_id,
                                               Status *status) {
    const int position = nextPosition(db_, "folders", "parent_id", parent_id);

    sqlite3_stmt *st = nullptr;
    const char *sql =
        "INSERT INTO folders (name, parent_id, position) VALUES (?, ?, ?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        setStatus(status, sqliteError(db_, "prepare addFolder"));
        return std::nullopt;
    }
    bindText(st, 1, name);
    bindOptInt(st, 2, parent_id);
    sqlite3_bind_int(st, 3, position);

    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        setStatus(status, sqliteError(db_, "insert folder"));
        return std::nullopt;
    }
    setStatus(status, Status::good());
    return sqlite3_last_insert_rowid(db_);
}

std::optional<Folder> SessionStore::getFolder(int64_t id, Status *status) {
    const std::string sql =
        std::string("SELECT ") + kFolderCols + " FROM folders WHERE id = ?";
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        setStatus(status, sqliteError(db_, "prepare getFolder"));
        return std::nullopt;
    }
    sqlite3_bind_int64(st, 1, id);

    std::optional<Folder> out;
    if (sqlite3_step(st) == SQLITE_ROW) out = readFolder(st);
    sqlite3_finalize(st);
    setStatus(status, Status::good());
    return out;
}

std::vector<Folder> SessionStore::listFolders(std::optional<int64_t> parent_id,
                                              Status *status) {
    const std::string sql = std::string("SELECT ") + kFolderCols +
                            " FROM folders WHERE parent_id IS ? "
                            "ORDER BY position, name";
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        setStatus(status, sqliteError(db_, "prepare listFolders"));
        return {};
    }
    bindOptInt(st, 1, parent_id);

    std::vector<Folder> out;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(readFolder(st));
    sqlite3_finalize(st);
    setStatus(status, Status::good());
    return out;
}

Status SessionStore::updateFolder(const Folder &folder) {
    if (!folder.id) return Status::fail("updateFolder: folder has no id");

    sqlite3_stmt *st = nullptr;
    const char *sql =
        "UPDATE folders SET name = ?, parent_id = ?, position = ?, expanded = ? "
        "WHERE id = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK)
        return sqliteError(db_, "prepare updateFolder");

    bindText(st, 1, folder.name);
    bindOptInt(st, 2, folder.parent_id);
    sqlite3_bind_int(st, 3, folder.position);
    sqlite3_bind_int(st, 4, folder.expanded ? 1 : 0);
    sqlite3_bind_int64(st, 5, *folder.id);

    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? Status::good()
                             : sqliteError(db_, "update folder");
}

Status SessionStore::deleteFolder(int64_t id) {
    // One transaction around all three statements. The Python commits once at
    // the end for the same reason: a crash between the reparent and the delete
    // would leave rows pointing at a folder that no longer exists, and with
    // foreign keys off nothing would ever notice.
    if (Status s = execSimple(db_, "BEGIN"); !s) return s;

    auto run = [&](const char *sql) -> Status {
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK)
            return sqliteError(db_, "prepare deleteFolder");
        sqlite3_bind_int64(st, 1, id);
        const int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        return rc == SQLITE_DONE ? Status::good()
                                 : sqliteError(db_, "deleteFolder step");
    };

    // Sessions in the folder move to root -- what SET NULL would have done.
    Status s = run("UPDATE sessions SET folder_id = NULL WHERE folder_id = ?");
    // Subfolders move to root -- what CASCADE would NOT have done. This is the
    // divergence between the DDL and the behaviour, and matching the behaviour
    // is what keeps a shared file from losing a subtree.
    if (s) s = run("UPDATE folders SET parent_id = NULL WHERE parent_id = ?");
    if (s) s = run("DELETE FROM folders WHERE id = ?");

    if (!s) {
        execSimple(db_, "ROLLBACK");
        return s;
    }
    return execSimple(db_, "COMMIT");
}

Status SessionStore::moveFolder(int64_t folder_id,
                                std::optional<int64_t> parent_id) {
    // Walk up from the proposed parent; meeting folder_id means the move would
    // detach a cycle from the tree. The Python raises here and nothing else
    // guards it, so this check is the only thing standing between a drag and
    // an unreachable subtree.
    std::optional<int64_t> current = parent_id;
    while (current) {
        if (*current == folder_id)
            return Status::fail("cannot move folder into itself");
        auto f = getFolder(*current);
        current = f ? f->parent_id : std::nullopt;
    }

    const int position = nextPosition(db_, "folders", "parent_id", parent_id);

    sqlite3_stmt *st = nullptr;
    const char *sql =
        "UPDATE folders SET parent_id = ?, position = ? WHERE id = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK)
        return sqliteError(db_, "prepare moveFolder");
    bindOptInt(st, 1, parent_id);
    sqlite3_bind_int(st, 2, position);
    sqlite3_bind_int64(st, 3, folder_id);

    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? Status::good() : sqliteError(db_, "move folder");
}

// --- sessions -------------------------------------------------------------

std::optional<int64_t> SessionStore::addSession(const Session &session,
                                                Status *status) {
    const int position =
        nextPosition(db_, "sessions", "folder_id", session.folder_id);

    sqlite3_stmt *st = nullptr;

    // "?" per writable column, COUNTED FROM THE LIST rather than written out.
    // This used to be thirty literal placeholders on two lines, which is a
    // silent trap: adding a column to kSessionWriteCols without adding a "?"
    // here prepares a statement whose column and value counts disagree, and
    // the failure surfaces as a prepare error at runtime rather than at the
    // edit. updateSession already derived its SET list from the same string;
    // this is the insert catching up.
    std::string values;
    {
        const std::string cols(kSessionWriteCols);
        const size_t n = std::count(cols.begin(), cols.end(), ',') + 1;
        for (size_t i = 0; i < n; ++i) {
            if (i) values += ", ";
            values += "?";
        }
    }

    const std::string sql = std::string("INSERT INTO sessions (") +
                            kSessionWriteCols + ") VALUES (" + values + ")";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        setStatus(status, sqliteError(db_, "prepare addSession"));
        return std::nullopt;
    }
    // The caller's position is ignored here and honoured in updateSession --
    // see the header. That asymmetry is nterm-qt's and is deliberate.
    bindWritable(st, 1, session, position);

    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        setStatus(status, sqliteError(db_, "insert session"));
        return std::nullopt;
    }
    setStatus(status, Status::good());
    return sqlite3_last_insert_rowid(db_);
}

std::optional<Session> SessionStore::getSession(int64_t id, Status *status) {
    const std::string sql =
        std::string("SELECT ") + kSessionCols + " FROM sessions WHERE id = ?";
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        setStatus(status, sqliteError(db_, "prepare getSession"));
        return std::nullopt;
    }
    sqlite3_bind_int64(st, 1, id);

    std::optional<Session> out;
    if (sqlite3_step(st) == SQLITE_ROW) out = readSession(st);
    sqlite3_finalize(st);
    setStatus(status, Status::good());
    return out;
}

std::vector<Session> SessionStore::listSessions(std::optional<int64_t> folder_id,
                                                Status *status) {
    const std::string sql = std::string("SELECT ") + kSessionCols +
                            " FROM sessions WHERE folder_id IS ? "
                            "ORDER BY position, name";
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        setStatus(status, sqliteError(db_, "prepare listSessions"));
        return {};
    }
    bindOptInt(st, 1, folder_id);

    std::vector<Session> out;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(readSession(st));
    sqlite3_finalize(st);
    setStatus(status, Status::good());
    return out;
}

std::vector<Session> SessionStore::listAllSessions(Status *status) {
    const std::string sql =
        std::string("SELECT ") + kSessionCols + " FROM sessions ORDER BY name";
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        setStatus(status, sqliteError(db_, "prepare listAllSessions"));
        return {};
    }
    std::vector<Session> out;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(readSession(st));
    sqlite3_finalize(st);
    setStatus(status, Status::good());
    return out;
}

Status SessionStore::updateSession(const Session &session) {
    if (!session.id) return Status::fail("updateSession: session has no id");

    // "col = ?" for each writable column, built from the same list the insert
    // uses, so adding a column to kSessionWriteCols updates both statements.
    std::string sets;
    {
        std::string cols(kSessionWriteCols);
        size_t start = 0;
        while (start < cols.size()) {
            size_t comma = cols.find(',', start);
            if (comma == std::string::npos) comma = cols.size();
            std::string name = cols.substr(start, comma - start);
            // Trim: the literal is wrapped across lines with ", " separators.
            size_t b = name.find_first_not_of(" \t\n");
            size_t e = name.find_last_not_of(" \t\n");
            if (b != std::string::npos) {
                if (!sets.empty()) sets += ", ";
                sets += name.substr(b, e - b + 1) + " = ?";
            }
            start = comma + 1;
        }
    }

    sqlite3_stmt *st = nullptr;
    const std::string sql =
        "UPDATE sessions SET " + sets + " WHERE id = ?";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return sqliteError(db_, "prepare updateSession");

    bindWritable(st, 1, session, session.position);

    // The id goes in the LAST placeholder, after every writable column. Asked
    // of the statement rather than written down: this was a literal 31, which
    // silently became wrong the moment a column was added -- the id bound into
    // the new column's slot and WHERE id took NULL, so every update matched no
    // rows and reported success. session_tree_probe is what caught it, because
    // a reorder is a write whose only evidence is that it persisted.
    sqlite3_bind_int64(st, sqlite3_bind_parameter_count(st), *session.id);

    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? Status::good() : sqliteError(db_, "update session");
}

Status SessionStore::deleteSession(int64_t id) {
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM sessions WHERE id = ?", -1, &st,
                           nullptr) != SQLITE_OK)
        return sqliteError(db_, "prepare deleteSession");
    sqlite3_bind_int64(st, 1, id);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? Status::good() : sqliteError(db_, "delete session");
}

Status SessionStore::recordConnect(int64_t id) {
    sqlite3_stmt *st = nullptr;
    const char *sql =
        "UPDATE sessions SET last_connected = CURRENT_TIMESTAMP, "
        "connect_count = connect_count + 1 WHERE id = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK)
        return sqliteError(db_, "prepare recordConnect");
    sqlite3_bind_int64(st, 1, id);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? Status::good() : sqliteError(db_, "record connect");
}

std::vector<Session> SessionStore::searchSessions(const std::string &query,
                                                  Status *status) {
    const std::string pattern = "%" + query + "%";
    const std::string sql = std::string("SELECT ") + kSessionCols +
                            " FROM sessions WHERE name LIKE ? OR "
                            "description LIKE ? OR hostname LIKE ? "
                            "ORDER BY name";
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        setStatus(status, sqliteError(db_, "prepare searchSessions"));
        return {};
    }
    for (int i = 1; i <= 3; ++i) bindText(st, i, pattern);

    std::vector<Session> out;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(readSession(st));
    sqlite3_finalize(st);
    setStatus(status, Status::good());
    return out;
}

Status SessionStore::moveSession(int64_t session_id,
                                 std::optional<int64_t> folder_id) {
    const int position = nextPosition(db_, "sessions", "folder_id", folder_id);

    sqlite3_stmt *st = nullptr;
    const char *sql =
        "UPDATE sessions SET folder_id = ?, position = ? WHERE id = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK)
        return sqliteError(db_, "prepare moveSession");
    bindOptInt(st, 1, folder_id);
    sqlite3_bind_int(st, 2, position);
    sqlite3_bind_int64(st, 3, session_id);

    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? Status::good() : sqliteError(db_, "move session");
}

Tree SessionStore::tree(Status *status) {
    Tree t;

    const std::string fsql = std::string("SELECT ") + kFolderCols +
                             " FROM folders ORDER BY position, name";
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, fsql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        setStatus(status, sqliteError(db_, "prepare tree folders"));
        return t;
    }
    while (sqlite3_step(st) == SQLITE_ROW) t.folders.push_back(readFolder(st));
    sqlite3_finalize(st);

    t.sessions = listAllSessions(status);
    return t;
}

} // namespace omega::sessions
