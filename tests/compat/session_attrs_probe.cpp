// tests/compat/session_attrs_probe.cpp
//
// The per-session attribute columns: that they are added to a database that
// predates them, that every one survives a write and a read, and that the
// three-layer resolution in effectiveconfig.h picks the layer it should.
//
// No display, no vault, no network. The resolver is a pure function of three
// structs and a handle, which is what makes the whole inheritance matrix
// checkable here rather than by connecting to something.
//
// The round-trip case is the one that earns its keep. kSessionCols and
// readSession() state the column order twice, and a column inserted into one
// and not the other reads the neighbouring value -- which for
// anti_idle_seconds next to anti_idle_keystroke is a wrong number, not a
// crash. Writing every field with a distinct value and reading it back is
// what catches that.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <sqlite3.h>

#include "app/effectiveconfig.h"
#include "sessions/store.h"

using omega::sessions::Session;
using omega::sessions::SessionStore;
using omega::sessions::SessionTransport;
using omega::sessions::Status;

namespace {

int failures = 0;

void ok(bool cond, const char *what) {
    if (!cond) {
        ++failures;
        std::printf("  FAIL %s\n", what);
    } else {
        std::printf("  ok   %s\n", what);
    }
}

// A database with the twelve original columns and nothing else -- what a real
// nterm-qt would have left behind. Written with raw sqlite3 rather than
// through SessionStore, because SessionStore is the thing being tested and it
// migrates on open.
bool writeLegacyDatabase(const std::string &path) {
    sqlite3 *db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) return false;

    const char *ddl =
        "CREATE TABLE sessions ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL,"
        "description TEXT DEFAULT '', hostname TEXT NOT NULL,"
        "port INTEGER DEFAULT 22, credential_name TEXT, folder_id INTEGER,"
        "position INTEGER DEFAULT 0,"
        "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        "last_connected TIMESTAMP, connect_count INTEGER DEFAULT 0,"
        "extras TEXT DEFAULT '{}');"
        "CREATE TABLE folders (id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT NOT NULL, parent_id INTEGER, position INTEGER DEFAULT 0,"
        "expanded INTEGER DEFAULT 1);"
        "INSERT INTO sessions (name, hostname, port, extras) VALUES "
        "('lab-core-1', 'lab-core-1.lab.internal', 22,"
        " '{\"vendor\":\"cisco\",\"unknown_key\":\"kept\"}');";

    char *err = nullptr;
    const bool good = sqlite3_exec(db, ddl, nullptr, nullptr, &err) == SQLITE_OK;
    if (err) sqlite3_free(err);
    sqlite3_close(db);
    return good;
}

void testMigration(const std::string &dir) {
    std::printf("migration onto a pre-existing database\n");
    const std::string path = dir + "/legacy.db";
    if (!writeLegacyDatabase(path)) {
        ok(false, "wrote a twelve-column database to migrate");
        return;
    }

    Status status;
    auto store = SessionStore::open(path, &status);
    ok(store != nullptr, "opens a database written before the columns existed");
    if (!store) return;

    const auto rows = store->listAllSessions();
    ok(rows.size() == 1, "the existing row survived the migration");
    if (rows.empty()) return;

    // The row is intact and the new columns read as inherit -- except
    // transport, which is the one column with a default because every row
    // that predates it was ssh.
    ok(rows[0].hostname == "lab-core-1.lab.internal", "hostname untouched");
    ok(rows[0].extras.find("unknown_key") != std::string::npos,
       "extras blob untouched, unknown key included");
    ok(rows[0].transport == SessionTransport::Ssh, "transport defaults to ssh");
    ok(!rows[0].term_type.has_value(), "term_type is absent, not empty");
    ok(!rows[0].legacy_algorithms.has_value(),
       "legacy_algorithms is absent, not false");
    ok(!rows[0].anti_idle_enabled.has_value(),
       "anti_idle_enabled is absent, not false");

    // Idempotent: opening again must not fail on a duplicate column.
    Status second;
    auto again = SessionStore::open(path, &second);
    ok(again != nullptr, "a second open re-runs the migration harmlessly");
}

void testRoundTrip(const std::string &dir) {
    std::printf("round trip of every attribute\n");
    Status status;
    auto store = SessionStore::open(dir + "/roundtrip.db", &status);
    if (!store) {
        ok(false, "opened a fresh store");
        return;
    }

    Session s;
    s.name = "lab-edge-7";
    s.hostname = "lab-edge-7.lab.internal";
    s.port = 2023;
    s.transport = SessionTransport::Telnet;
    // Distinct values throughout: two fields that shared one would not show a
    // swapped column index.
    s.term_type = "vt100";
    s.scrollback_lines = 4242;
    s.multiline_paste_threshold = 7;
    s.paste_baud = 9600;
    s.host_key_policy = "tofu";
    s.legacy_algorithms = true;
    s.username = "lab-operator";
    s.jump_host = "lab-bastion.lab.internal";
    s.jump_port = 2222;
    s.jump_username = "lab-jump";
    s.jump_credential = "lab-bastion-key";
    s.anti_idle_enabled = true;
    s.anti_idle_seconds = 45;
    s.anti_idle_keystroke = "custom";
    s.anti_idle_custom = "2008";
    s.serial_port = "/dev/ttyUSB3";
    s.serial_baud = 115200;
    s.serial_data_bits = 7;
    s.serial_parity = "even";
    s.serial_stop_bits = "2";
    s.telnet_crlf = false;
    s.wheel_alt_screen = false;
    s.font_size = 11;
    s.theme_name = "solarized";

    const auto id = store->addSession(s, &status);
    ok(id.has_value(), "insert with every column set");
    if (!id) return;

    const auto back = store->getSession(*id);
    ok(back.has_value(), "reads back");
    if (!back) return;

    const Session &r = *back;
    ok(r.transport == SessionTransport::Telnet, "transport");
    ok(r.term_type == std::optional<std::string>("vt100"), "term_type");
    ok(r.scrollback_lines == std::optional<int>(4242), "scrollback_lines");
    ok(r.multiline_paste_threshold == std::optional<int>(7),
       "multiline_paste_threshold");
    ok(r.paste_baud == std::optional<int>(9600), "paste_baud");
    ok(r.host_key_policy == std::optional<std::string>("tofu"),
       "host_key_policy");
    ok(r.legacy_algorithms == std::optional<bool>(true), "legacy_algorithms");
    ok(r.username == std::optional<std::string>("lab-operator"), "username");
    ok(r.jump_host == std::optional<std::string>("lab-bastion.lab.internal"),
       "jump_host");
    ok(r.jump_port == std::optional<int>(2222), "jump_port");
    ok(r.jump_username == std::optional<std::string>("lab-jump"),
       "jump_username");
    ok(r.jump_credential == std::optional<std::string>("lab-bastion-key"),
       "jump_credential");
    ok(r.anti_idle_enabled == std::optional<bool>(true), "anti_idle_enabled");
    ok(r.anti_idle_seconds == std::optional<int>(45), "anti_idle_seconds");
    ok(r.anti_idle_keystroke == std::optional<std::string>("custom"),
       "anti_idle_keystroke");
    ok(r.anti_idle_custom == std::optional<std::string>("2008"),
       "anti_idle_custom");
    ok(r.serial_port == std::optional<std::string>("/dev/ttyUSB3"),
       "serial_port");
    ok(r.serial_baud == std::optional<int>(115200), "serial_baud");
    ok(r.serial_data_bits == std::optional<int>(7), "serial_data_bits");
    ok(r.serial_parity == std::optional<std::string>("even"), "serial_parity");
    ok(r.serial_stop_bits == std::optional<std::string>("2"), "serial_stop_bits");
    ok(r.telnet_crlf == std::optional<bool>(false), "telnet_crlf");
    ok(r.wheel_alt_screen == std::optional<bool>(false), "wheel_alt_screen");
    ok(r.font_size == std::optional<int>(11), "font_size");
    ok(r.theme_name == std::optional<std::string>("solarized"), "theme_name");

    // False and zero specifically: the two values a "0 means unset" scheme
    // would lose. paste_baud 0 is unpaced and is a real choice.
    Session zeros;
    zeros.name = "lab-zero";
    zeros.hostname = "lab-zero.lab.internal";
    zeros.legacy_algorithms = false;
    zeros.anti_idle_enabled = false;
    zeros.paste_baud = 0;
    const auto zid = store->addSession(zeros, &status);
    const auto zback = zid ? store->getSession(*zid) : std::nullopt;
    ok(zback && zback->legacy_algorithms == std::optional<bool>(false),
       "false survives as false, not as absent");
    ok(zback && zback->paste_baud == std::optional<int>(0),
       "paste_baud 0 survives as a value, not as absent");

    // updateSession writes every column, so an edit must not lose them.
    Session edited = *back;
    edited.description = "changed";
    ok(bool(store->updateSession(edited)), "update accepted");
    const auto after = store->getSession(*id);
    ok(after && after->anti_idle_seconds == std::optional<int>(45),
       "an unrelated edit leaves the attributes alone");
    ok(after && after->transport == SessionTransport::Telnet,
       "an unrelated edit leaves the transport alone");
    ok(after && after->theme_name == std::optional<std::string>("solarized"),
       "an unrelated edit leaves the theme pin alone");
    ok(after && after->font_size == std::optional<int>(11),
       "an unrelated edit leaves the font size alone");
}

void testResolution() {
    std::printf("three-layer resolution\n");

    omega::app::AppSettings globals;
    globals.default_term_type = QStringLiteral("xterm-256color");
    globals.scrollback_lines = 10000;
    globals.multiline_paste_threshold = 1;
    globals.font_size = 14;

    omega::app::OmegaSettings omega;
    omega.anti_idle.enabled = false;
    omega.anti_idle.seconds = 60;
    omega.anti_idle.keystroke = omega::app::AntiIdleKeystroke::Backspace;

    // --- a session with nothing set inherits everything -------------------
    Session bare;
    bare.name = "lab-core-1";
    bare.hostname = "lab-core-1.lab.internal";
    bare.port = 22;

    auto r = omega::app::resolveSession(bare, globals, omega, 0);
    ok(r.config.term == QStringLiteral("xterm-256color"),
       "bare session takes the global term type");
    ok(r.tab.scrollbackLines == 10000, "bare session takes global scrollback");
    ok(r.config.hostKeyPolicy == omegassh::HostKeyPolicy::Strict,
       "bare session takes the library default host key policy");
    ok(!r.config.legacyAlgorithms, "bare session takes legacy off");
    ok(r.tab.antiIdle.enabled == false, "bare session takes anti-idle off");
    ok(r.tab.pasteBaud == -1,
       "bare session leaves paste baud to the transport");
    ok(r.tab.wheelAltScreen, "bare session takes wheel-alt-screen on");
    ok(r.tab.fontPointSize == 14, "bare session takes the global font size");
    ok(r.tab.themeName.isEmpty(),
       "bare session pins no theme, so the tab follows the window");

    // --- the global changes and the bare session follows it ---------------
    globals.scrollback_lines = 500;
    globals.font_size = 18;
    omega.anti_idle.enabled = true;
    omega.wheel_alt_screen = false;
    r = omega::app::resolveSession(bare, globals, omega, 0);
    ok(r.tab.fontPointSize == 18,
       "a session that never overrode the font follows the global");
    ok(r.tab.scrollbackLines == 500,
       "a session that never overrode scrollback follows the global");
    ok(r.tab.antiIdle.enabled, "and follows anti-idle being switched on");
    ok(!r.tab.wheelAltScreen,
       "and follows wheel-alt-screen being switched off");

    // --- overrides win ----------------------------------------------------
    Session over = bare;
    over.term_type = "vt100";
    over.scrollback_lines = 99;
    over.host_key_policy = "tofu";
    over.legacy_algorithms = true;
    over.paste_baud = 0;
    over.anti_idle_enabled = false;
    // The global is OFF by now, so `true` here proves the session wins rather
    // than coinciding with the default -- the trap a bool override falls into.
    over.wheel_alt_screen = true;
    over.font_size = 11;
    over.theme_name = "solarized";

    r = omega::app::resolveSession(over, globals, omega, 0);
    ok(r.tab.fontPointSize == 11,
       "session font size beats the global that just moved to 18");
    ok(r.tab.themeName == QStringLiteral("solarized"),
       "session theme name reaches the tab settings");
    ok(r.config.term == QStringLiteral("vt100"), "session term type wins");
    ok(r.tab.scrollbackLines == 99, "session scrollback wins");
    ok(r.config.hostKeyPolicy == omegassh::HostKeyPolicy::Tofu,
       "session host key policy wins");
    ok(r.config.legacyAlgorithms, "session legacy algorithms wins");
    ok(r.tab.wheelAltScreen,
       "session wheel-alt-screen ON beats the global being off");
    ok(r.tab.pasteBaud == 0,
       "an explicit unpaced paste baud is an override, not an absence");
    ok(r.tab.antiIdle.enabled == false,
       "a session can switch anti-idle OFF against a global that has it on");

    // --- anti-idle resolves field by field --------------------------------
    Session partial = bare;
    partial.anti_idle_seconds = 15;
    r = omega::app::resolveSession(partial, globals, omega, 0);
    ok(r.tab.antiIdle.enabled,
       "overriding the interval alone leaves enabled following the global");
    ok(r.tab.antiIdle.seconds == 15, "and takes the session's interval");
    ok(r.tab.antiIdle.keystroke == omega::app::AntiIdleKeystroke::Backspace,
       "and leaves the keystroke following the global");

    // --- transports -------------------------------------------------------
    Session serial = bare;
    serial.transport = SessionTransport::Serial;
    serial.serial_port = "/dev/ttyUSB0";
    serial.serial_baud = 9600;
    r = omega::app::resolveSession(serial, globals, omega, 0);
    ok(r.config.transport == omegassh::Transport::Serial,
       "a serial session resolves to the serial transport");
    ok(r.config.serialPort == QStringLiteral("/dev/ttyUSB0"), "serial port");
    ok(r.config.baud == 9600, "serial baud");

    // --- the credential pairing -------------------------------------------
    Session cred = bare;
    cred.credential_name = "lab-switch-login";
    r = omega::app::resolveSession(cred, globals, omega, 4242);
    ok(r.config.credential == QStringLiteral("lab-switch-login"),
       "credential name crosses");
    ok(r.config.vaultHandle == 4242, "and the handle crosses with it");

    // --- the credential a session does NOT name ---------------------------
    //
    // Every branch of SshDefaultAuth, on the same bare session. This is the
    // matrix a store imported from TerminalTelemetry lands in: credential_name
    // NULL on all of it, answered once by a global rather than by editing
    // each row.
    omega.ssh_default_auth = omega::app::SshDefaultAuth::VaultDefault;
    r = omega::app::resolveSession(bare, globals, omega, 4242,
                                   QStringLiteral("lab-default"));
    ok(r.config.credential == QStringLiteral("lab-default"),
       "vault-default mode names the default credential");
    ok(r.config.vaultHandle == 4242,
       "and pairs the handle with it, same as a session that named one");

    r = omega::app::resolveSession(bare, globals, omega, 4242, QString());
    ok(r.config.credential.isEmpty(),
       "no default available resolves to nothing -- the caller prompts");
    ok(r.config.vaultHandle == 0,
       "no credential name means no handle -- the two are set together");
    ok(!r.config.useAgent, "and does not silently fall back to the agent");

    r = omega::app::resolveSession(cred, globals, omega, 4242,
                                   QStringLiteral("lab-default"));
    ok(r.config.credential == QStringLiteral("lab-switch-login"),
       "a session that names a credential ignores the vault default");

    omega.ssh_default_auth = omega::app::SshDefaultAuth::Ask;
    r = omega::app::resolveSession(bare, globals, omega, 4242,
                                   QStringLiteral("lab-default"));
    ok(r.config.credential.isEmpty(),
       "ask mode resolves to nothing even when a default exists");

    omega.ssh_default_auth = omega::app::SshDefaultAuth::Agent;
    r = omega::app::resolveSession(bare, globals, omega, 4242,
                                   QStringLiteral("lab-default"));
    ok(r.config.useAgent, "agent mode actually sets use_agent");
    ok(r.config.credential.isEmpty(), "and names no credential");

    r = omega::app::resolveSession(cred, globals, omega, 4242, QString());
    ok(!r.config.useAgent,
       "a session that names a credential is not given the agent");

    // Telnet and serial have no authentication step and the library refuses
    // credentials on both, so the mode must not reach them.
    Session telnet = bare;
    telnet.transport = SessionTransport::Telnet;
    r = omega::app::resolveSession(telnet, globals, omega, 4242,
                                   QStringLiteral("lab-default"));
    ok(!r.config.useAgent && r.config.credential.isEmpty(),
       "telnet is untouched by the ssh credential default");

    omega.ssh_default_auth = omega::app::SshDefaultAuth::VaultDefault;
    r = omega::app::resolveSession(telnet, globals, omega, 4242,
                                   QStringLiteral("lab-default"));
    ok(r.config.credential.isEmpty(),
       "telnet is untouched by the vault default too");

    // --- the tab's name ----------------------------------------------------
    r = omega::app::resolveSession(bare, globals, omega, 0);
    ok(r.displayName == QStringLiteral("lab-core-1"),
       "a saved session resolves its name for the tab");

    // --- quick connect ----------------------------------------------------
    omegassh::Config dialog;
    dialog.transport = omegassh::Transport::Telnet;
    dialog.host = QStringLiteral("lab-term-1.lab.internal");
    dialog.port = 23;
    auto q = omega::app::resolveQuickConnect(dialog, globals, omega);
    ok(q.config.host == QStringLiteral("lab-term-1.lab.internal"),
       "quick connect config passes through");
    ok(q.config.term == QStringLiteral("xterm-256color"),
       "quick connect takes the global term type");
    ok(q.tab.scrollbackLines == 500,
       "quick connect takes the global tab settings");
    ok(q.tab.pasteBaud == -1,
       "quick connect leaves paste baud to the transport");
    ok(q.displayName.isEmpty(),
       "quick connect has no name, so the tab falls back to the summary");
    ok(q.tab.themeName.isEmpty(),
       "quick connect pins no theme");

    // --- the live settings path -------------------------------------------
    //
    // resolveTabSettings is what MainWindow::applySettingsToTabs calls when
    // the settings dialog is accepted with tabs already open. It used to copy
    // the globals onto every tab, which discarded every override on every
    // open tab until the next reconnect -- the database still held them, so
    // the session came back correct the next day and the fault read as
    // intermittent. These cases are what keeps that from coming back.
    omega::app::TabSettings live = omega::app::resolveTabSettings(over, globals, omega);
    ok(live.fontPointSize == 11,
       "the live path keeps a session's font override");
    ok(live.scrollbackLines == 99,
       "the live path keeps a session's scrollback override");
    ok(live.wheelAltScreen,
       "the live path keeps a session's wheel override against the global");
    ok(live.antiIdle.enabled == false,
       "the live path keeps a session's anti-idle override");
    ok(live.themeName == QStringLiteral("solarized"),
       "the live path keeps a session's theme pin");

    omega::app::TabSettings liveBare =
        omega::app::resolveTabSettings(bare, globals, omega);
    ok(liveBare.fontPointSize == 18,
       "and a session with no override still follows the global");
    ok(liveBare.themeName.isEmpty(),
       "and one with no pin still follows the window");

    // A quick-connect tab has no session row; the globals-only overload is
    // what it lands on, and it must agree with the layer resolveSession
    // starts from.
    omega::app::TabSettings liveGlobal =
        omega::app::resolveTabSettings(globals, omega);
    ok(liveGlobal.fontPointSize == 18 && liveGlobal.scrollbackLines == 500,
       "the globals-only overload is the same global layer");
    ok(liveGlobal.themeName.isEmpty(), "and pins no theme");

    // --- blank is absent ---------------------------------------------------
    // A column edited and cleared holds "", and that means inherit rather
    // than "a theme with no name" -- the same rule term_type follows.
    Session blankTheme = bare;
    blankTheme.theme_name = "";
    r = omega::app::resolveSession(blankTheme, globals, omega, 0);
    ok(r.tab.themeName.isEmpty(),
       "an empty theme_name is inherit, not a theme called nothing");
}

}  // namespace

int main() {
    const std::string dir = "/tmp/omega-session-attrs";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    testMigration(dir);
    testRoundTrip(dir);
    testResolution();

    std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}