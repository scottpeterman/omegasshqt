// sessions/store.h
//
// The session store.
//
// It began as a strict port of ntermqt/manager/models.py -- the same database,
// opened by both applications, with neither taught the other's serialiser --
// and the behaviours here that look arbitrary are still matching what the
// Python did. They are kept because the file on disk is the same file, not
// because a second writer is still coming for it: nterm-qt is retired, and
// the schema is Omega's now.
//
// That is what the columns below the twelve original ones are. They are added
// by migrate() on open rather than written into kSchema, so a database this
// application has never seen -- one a real nterm-qt wrote -- gains them on
// first open with every existing row intact. The base DDL stays byte-identical
// to the Python's so a fresh file is still created the way that one was.
//
// The DDL-comparison case in tests/compat/differential.py no longer holds
// after a migrated open: ALTER TABLE rewrites the stored CREATE statement.
// That test is an import test now, and the assertion to keep is that every
// folder, session and extras key survives -- not that the schema text matches.
//
// No Qt. The tree is a data structure; a view over it is Phase 3's problem.

#ifndef OMEGA_SESSIONS_STORE_H
#define OMEGA_SESSIONS_STORE_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace omega::sessions {

// Folder mirrors the `folders` row. parent_id empty means root -- the Python
// uses None for the same thing, and it is why every lookup on a parent uses
// `IS ?` rather than `= ?`.
struct Folder {
    std::optional<int64_t> id;
    std::string name;
    std::optional<int64_t> parent_id;
    int position = 0;
    bool expanded = true;
};

// Session mirrors the `sessions` row.
//
// extras is held as the raw column text and never parsed here. The
// compatibility requirement is that unknown keys survive a round trip, and
// opacity satisfies that more completely than any parse-and-re-emit could:
// there is no serialiser to disagree about key order, spacing or number
// formatting. The Python re-serialises on every write, so its own bytes are
// not stable either -- meaning a byte-comparison bar was never meetable, and
// carrying the blob untouched is the strongest guarantee available.
//
// When a caller eventually needs to *set* one key, it gets a JSON editor that
// preserves the rest. Nothing needs that yet, so nothing has it.
//
// created_at and last_connected are likewise opaque. SQLite writes them as
// "YYYY-MM-DD HH:MM:SS" in UTC via CURRENT_TIMESTAMP, and the Python reads
// them without detect_types, so they are strings on both sides of the fence
// despite the datetime annotation on its dataclass. Reformatting one here is
// how the two applications would start disagreeing about what a timestamp
// looks like.
// SessionTransport is what a saved session runs over. Stored as text
// ("ssh", "telnet", "serial") rather than an integer so the column reads
// as itself in a sqlite3 shell, and defaulted to ssh because every row that
// predates the column was one.
//
// This is the column that stopped telnet and serial being saved session
// types. They were quick-connect only to keep the schema unchanged for
// nterm-qt, which is retired.
enum class SessionTransport {
    Ssh,
    Telnet,
    Serial,
};

const char *transportName(SessionTransport transport);
SessionTransport transportFromName(const std::string &name, bool *known = nullptr);

// Session mirrors the `sessions` row.
//
// EVERY OVERRIDE IS OPTIONAL, AND ABSENT IS NOT A VALUE. std::nullopt means
// "inherit whatever the application settings say", which is a different thing
// from any value the field could hold -- including from `false`. A bool stored
// as 0-or-1 with a default could not express the difference, so a session
// saved once would pin whatever the global happened to be that day and stop
// following it afterwards. That is the bug this shape exists to prevent, and
// it is why the booleans below are optional<bool> rather than bool.
//
// Resolution is not this file's business. The store reads and writes; where a
// value comes from when the session does not carry one is app/effectiveconfig.h.
struct Session {
    std::optional<int64_t> id;
    std::string name;
    std::string description;
    std::string hostname;
    int port = 22;
    std::optional<std::string> credential_name;
    std::optional<int64_t> folder_id;
    int position = 0;
    std::string created_at;
    std::string last_connected;
    int connect_count = 0;
    std::string extras = "{}";

    // --- transport --------------------------------------------------------
    SessionTransport transport = SessionTransport::Ssh;

    // --- terminal ---------------------------------------------------------
    std::optional<std::string> term_type;
    std::optional<int> scrollback_lines;
    std::optional<int> multiline_paste_threshold;

    // 0 is a meaningful value here -- unpaced -- which is the clearest case
    // for why absent has to be its own state.
    std::optional<int> paste_baud;

    // --- ssh --------------------------------------------------------------
    // "strict" | "tofu" | "insecure". Text for the same reason transport is.
    std::optional<std::string> host_key_policy;
    std::optional<bool> legacy_algorithms;
    std::optional<std::string> username;

    // --- jump host --------------------------------------------------------
    // Empty jump_host means no bastion, matching Config: the remaining jump
    // fields are ignored without it.
    std::optional<std::string> jump_host;
    std::optional<int> jump_port;
    std::optional<std::string> jump_username;
    std::optional<std::string> jump_credential;

    // --- anti-idle --------------------------------------------------------
    // Split across four columns rather than a JSON blob so a query can find
    // every session with it switched on. Keystroke and custom use the
    // spellings in app/antiidle.h.
    std::optional<bool> anti_idle_enabled;
    std::optional<int> anti_idle_seconds;
    std::optional<std::string> anti_idle_keystroke;
    std::optional<std::string> anti_idle_custom;  // hex, no prefix

    // --- serial -----------------------------------------------------------
    std::optional<std::string> serial_port;
    std::optional<int> serial_baud;
    std::optional<int> serial_data_bits;
    std::optional<std::string> serial_parity;
    std::optional<std::string> serial_stop_bits;

    // --- telnet -----------------------------------------------------------
    std::optional<bool> telnet_crlf;

    // --- terminal behaviour -------------------------------------------------
    // Wheel over a full-screen application scrolls that application instead of
    // the local scrollback behind it. NULL inherits the global, as everywhere
    // here; see OmegaSettings::wheel_alt_screen for what it does and why it is
    // on by default.
    //
    // Not transport-specific: a pager runs over ssh, telnet and a serial
    // console alike, so this survives a transport change rather than being
    // cleared with the ssh-only columns.
    std::optional<bool> wheel_alt_screen;

    // --- appearance -------------------------------------------------------
    // The terminal's font size in points and its colour theme, per session.
    // NULL inherits AppSettings::font_size and AppSettings::theme_name, as
    // every optional here does -- and inheriting is what most sessions want,
    // so these are pinned by the few that have a reason.
    //
    // The reason is usually that the far end and the screen disagree. A
    // console server that has to show 132 columns needs a smaller font than
    // the one the global is set to, and it needs it after the global moves.
    // A theme pinned to production is the other half of that: it is the
    // cheapest possible warning that this window is not the lab.
    //
    // Neither is transport-specific, so both survive a transport change
    // rather than being cleared alongside the ssh-only columns.
    //
    // theme_name holds a theme's `name:` key -- what ThemeEngine is keyed by
    // and what AppSettings::theme_name holds -- not a filename. A name that
    // is not installed resolves to nothing, and the session then falls back
    // to whatever theme the window is on rather than to the built-in
    // fallback. That is deliberate: an unresolvable name means a database
    // that arrived from a machine with a theme this one does not have, and
    // the window's current theme is a better answer than enterprise_dark.
    //
    // font_size is the BASE size. Ctrl+wheel zoom is per-tab and per-run and
    // is not written back here, so zooming a session does not silently pin
    // it -- see TerminalTab::zoomBy.
    std::optional<int> font_size;
    std::optional<std::string> theme_name;
};

// Tree is the whole store in memory: every folder and every session, in the
// order the store returns them. Assembling parents and children from this is
// the caller's business and needs no database.
struct Tree {
    std::vector<Folder> folders;
    std::vector<Session> sessions;
};

// Status carries a message rather than an error code because every failure
// here is either a programming mistake or a broken file, and both are things a
// human reads.
struct Status {
    bool ok = true;
    std::string message;

    explicit operator bool() const { return ok; }
    static Status good() { return {}; }
    static Status fail(std::string m) { return {false, std::move(m)}; }
};

class SessionStore {
public:
    // Opens (creating if needed) the database at path. The parent directory is
    // created too, matching _ensure_db.
    //
    // Runs the column migration before returning, so every caller sees the
    // full schema and no caller has to ask whether it ran. It is idempotent
    // and additive: each column is added only if PRAGMA table_info does not
    // already list it, nothing is dropped, and no row is rewritten.
    //
    // Foreign keys are deliberately NOT enabled. The DDL declares ON DELETE
    // CASCADE on folders.parent_id and ON DELETE SET NULL on
    // sessions.folder_id, but SQLite ignores both unless PRAGMA foreign_keys
    // is on, and nterm-qt never sets it. Its delete_folder then implements the
    // opposite of the declared cascade -- subfolders are reparented to root,
    // not deleted. Turning enforcement on here would destroy a subtree the
    // other application would have kept, on a file they share.
    static std::unique_ptr<SessionStore> open(const std::string &path,
                                              Status *status = nullptr);
    ~SessionStore();

    SessionStore(const SessionStore &) = delete;
    SessionStore &operator=(const SessionStore &) = delete;

    // --- folders ----------------------------------------------------------

    // Appends at the end of its parent: position is MAX(position) + 1 among
    // siblings, computed here rather than taken from the caller.
    std::optional<int64_t> addFolder(const std::string &name,
                                     std::optional<int64_t> parent_id = {},
                                     Status *status = nullptr);

    std::optional<Folder> getFolder(int64_t id, Status *status = nullptr);

    // ORDER BY position, name -- both columns, in that order. position is not
    // unique and ties break on name, so sorting by position alone would
    // disagree with the Python on any two siblings that share one.
    std::vector<Folder> listFolders(std::optional<int64_t> parent_id = {},
                                    Status *status = nullptr);

    Status updateFolder(const Folder &folder);

    // Three statements in one transaction: sessions to root, subfolders to
    // root, then the folder itself. This is the shape delete_folder has, and
    // it is not what the DDL says would happen under enforced foreign keys.
    Status deleteFolder(int64_t id);

    // Rejects a move into the folder's own subtree by walking the parent
    // chain, as move_folder does. Appends at the end of the new parent.
    Status moveFolder(int64_t folder_id, std::optional<int64_t> parent_id = {});

    // --- sessions ---------------------------------------------------------

    // The caller's position is ignored, as in add_session: the row lands at
    // the end of its folder. created_at is left to the column default so the
    // timestamp comes from SQLite, not from a C++ clock.
    std::optional<int64_t> addSession(const Session &session,
                                      Status *status = nullptr);

    std::optional<Session> getSession(int64_t id, Status *status = nullptr);
    std::vector<Session> listSessions(std::optional<int64_t> folder_id = {},
                                      Status *status = nullptr);

    // ORDER BY name, with no folder grouping -- list_all_sessions does not
    // order by position at all, and the tree view is what reimposes structure.
    std::vector<Session> listAllSessions(Status *status = nullptr);

    // Writes position as given, unlike addSession. The tree rewrites positions
    // in bulk after a drag, which is the caller this asymmetry exists for.
    Status updateSession(const Session &session);
    Status deleteSession(int64_t id);

    // last_connected = CURRENT_TIMESTAMP, connect_count + 1. The timestamp is
    // SQLite's for the same reason the default is.
    Status recordConnect(int64_t id);

    // LIKE %query% across name, description and hostname, ORDER BY name.
    std::vector<Session> searchSessions(const std::string &query,
                                        Status *status = nullptr);

    Status moveSession(int64_t session_id,
                       std::optional<int64_t> folder_id = {});

    // --- whole store ------------------------------------------------------

    // folders ORDER BY position, name; sessions ORDER BY name. Both orderings
    // are get_tree's, and the second one is deliberately not by position.
    Tree tree(Status *status = nullptr);

private:
    SessionStore() = default;
    sqlite3 *db_ = nullptr;
};

} // namespace omega::sessions

#endif // OMEGA_SESSIONS_STORE_H
