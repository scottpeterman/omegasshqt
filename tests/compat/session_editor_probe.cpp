// tests/compat/session_editor_probe.cpp
//
// The session editor's round trip: what goes into the form comes back out of
// session(), and -- the case that actually matters -- what was ABSENT comes
// back absent.
//
// A form full of spin boxes and combo boxes has a default position for every
// widget, and the failure this guards against is the quiet one: a row nobody
// touched saving the value its widget happened to be showing. That turns
// "follows the global" into "pinned to whatever the global was the day this
// session was last opened", and it looks correct until the global changes
// months later. Every inherit row is therefore asserted to survive an
// open-and-save with nullopt intact.
//
// The transport cases are the other half. Credentials, host key policy and a
// jump host are SSH's alone -- telnet has no authentication step and serial
// has no network identity -- so saving as either must clear them rather than
// leave columns behind that the resolver will never read.
//
// Runs under -platform offscreen. It builds widgets, so it needs QtWidgets and
// a QApplication; it needs no display, no store and no vault.

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <QApplication>
#include <QLayout>
#include <QSpinBox>

#include "app/sessioneditordialog.h"
#include "theme/theme.h"

using omega::app::AppSettings;
using omega::app::OmegaSettings;
using omega::app::SessionEditorDialog;
using omega::sessions::Session;
using omega::sessions::SessionTransport;

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

// Open the form on a session and save it again, touching nothing. Everything
// this probe asserts is a property of that pair.
Session roundTrip(const Session &in, const AppSettings &settings,
                  const OmegaSettings &omega,
                  const omega::theme::ThemeEngine *themes = nullptr) {
    SessionEditorDialog dialog(in, nullptr, nullptr, settings, omega, themes);
    return dialog.session();
}

void bareSessionStaysBare() {
    std::printf("a session that overrides nothing\n");

    AppSettings settings;
    settings.default_term_type = QStringLiteral("xterm-256color");
    settings.scrollback_lines = 50000;
    settings.multiline_paste_threshold = 4;

    OmegaSettings omega;
    omega.anti_idle.enabled = true;
    omega.anti_idle.seconds = 240;

    Session in;
    in.name = "lab-core-1";
    in.hostname = "10.20.0.11";
    in.port = 22;

    const Session out = roundTrip(in, settings, omega);

    ok(out.name == "lab-core-1", "keeps its name");
    ok(out.hostname == "10.20.0.11", "keeps its host");
    ok(out.port == 22, "keeps its port");

    // The whole point of the probe. A global of 50000 scrollback lines is
    // showing in the spin box as a label, and must not be saved as a value.
    ok(!out.term_type.has_value(), "term type stays absent");
    ok(!out.scrollback_lines.has_value(), "scrollback stays absent");
    ok(!out.multiline_paste_threshold.has_value(), "paste threshold stays absent");
    ok(!out.paste_baud.has_value(), "paste rate stays absent");
    ok(!out.host_key_policy.has_value(), "host key policy stays absent");
    ok(!out.legacy_algorithms.has_value(), "legacy algorithms stays absent");
    ok(!out.anti_idle_enabled.has_value(), "anti-idle enabled stays absent");
    ok(!out.anti_idle_seconds.has_value(), "anti-idle interval stays absent");
    ok(!out.anti_idle_keystroke.has_value(), "anti-idle keystroke stays absent");
    ok(!out.jump_host.has_value(), "jump host stays absent");
    ok(!out.username.has_value(), "username stays absent");

    // The two appearance rows, which are the same trap: a global font size of
    // 14 is sitting in the spin box as a LABEL and the global theme name is
    // sitting in the combo as a label, and neither may be saved as a value.
    // A session that came back pinned to whatever the globals were on the day
    // somebody opened its editor is the bug this whole idiom exists to stop.
    ok(!out.font_size.has_value(), "font size stays absent");
    ok(!out.theme_name.has_value(), "theme stays absent");
}

void everyOverrideSurvives() {
    std::printf("a session that overrides everything\n");

    AppSettings settings;
    OmegaSettings omega;

    Session in;
    in.name = "lab-edge-2";
    in.hostname = "10.20.0.12";
    in.port = 2222;
    in.credential_name = std::string("lab-admin");
    in.username = std::string("operator");
    in.term_type = std::string("vt100");
    in.scrollback_lines = 200000;
    in.multiline_paste_threshold = 12;
    in.paste_baud = 9600;
    in.host_key_policy = std::string("tofu");
    in.legacy_algorithms = true;
    in.anti_idle_enabled = true;
    in.anti_idle_seconds = 90;
    in.anti_idle_keystroke = std::string("space-backspace");
    in.jump_host = std::string("lab-bastion");
    in.jump_port = 2200;
    in.jump_username = std::string("hop");
    in.font_size = 11;
    in.theme_name = std::string("solarized");

    const Session out = roundTrip(in, settings, omega);

    ok(out.port == 2222, "a non-default port is not reset by the transport row");
    ok(out.credential_name == in.credential_name, "credential name");
    ok(out.username == in.username, "username override");
    ok(out.term_type == in.term_type, "term type");
    ok(out.scrollback_lines == in.scrollback_lines, "scrollback");
    ok(out.multiline_paste_threshold == in.multiline_paste_threshold,
       "paste threshold");
    ok(out.paste_baud == in.paste_baud, "paste rate");
    ok(out.host_key_policy == in.host_key_policy, "host key policy");
    ok(out.legacy_algorithms == in.legacy_algorithms, "legacy algorithms");
    ok(out.anti_idle_enabled == in.anti_idle_enabled, "anti-idle enabled");
    ok(out.anti_idle_seconds == in.anti_idle_seconds, "anti-idle interval");
    ok(out.anti_idle_keystroke == in.anti_idle_keystroke, "anti-idle keystroke");
    ok(out.jump_host == in.jump_host, "jump host");
    ok(out.jump_port == in.jump_port, "jump port");
    ok(out.jump_username == in.jump_username, "jump username");
    ok(out.font_size == in.font_size, "font size");

    // Survives WITH NO THEME ENGINE, which is the case that matters: the form
    // was handed no list of installed themes, so the pinned name is not among
    // the combo's entries. loadFrom adds it as a "(not installed)" row rather
    // than falling back to inherit, because falling back would rewrite the
    // column on save and lose a setting made on another machine -- and a
    // database moving between machines is exactly how a name gets here.
    ok(out.theme_name == in.theme_name,
       "a theme this build does not have is kept, not dropped");
}

// Stepping off the inherit row.
//
// The widgets are private, so this reaches them the way a user does: find the
// spin box whose special value names the row, step it once, and read what the
// form would save. That is the assertion that matters -- not which class the
// row is built from.
void steppingOffInheritLandsOnTheInheritedValue() {
    std::printf("stepping off an inherit row\n");

    AppSettings settings;
    settings.font_size = 14;
    settings.scrollback_lines = 10000;
    OmegaSettings omega;

    Session in;
    in.name = "lab-core-1";
    in.hostname = "10.20.0.11";

    SessionEditorDialog dialog(in, nullptr, nullptr, settings, omega);

    QSpinBox *font = nullptr;
    QSpinBox *scrollback = nullptr;
    for (QSpinBox *box : dialog.findChildren<QSpinBox *>()) {
        const QString label = box->specialValueText();
        if (label.contains(QStringLiteral("pt"))) font = box;
        else if (label.contains(QStringLiteral("10000"))) scrollback = box;
    }
    ok(font != nullptr, "found the font row by its inherit label");
    ok(scrollback != nullptr, "found the scrollback row by its inherit label");
    if (!font || !scrollback) return;

    ok(font->value() == font->minimum(), "the font row starts on inherit");

    // The bug this exists for: a plain QSpinBox steps from the special value
    // to the range floor, so one press turned a 14 pt terminal into a 6 pt
    // one and wanted eight more presses to undo.
    font->stepUp();
    ok(font->value() == 14,
       "one step up off inherit lands on the inherited 14, not on the floor");
    ok(dialog.session().font_size == std::optional<int>(14),
       "and the form saves it as a pin");

    // Not special-cased to the font row.
    scrollback->stepUp();
    ok(scrollback->value() == 10000,
       "the scrollback row lands on its own inherited value");

    // From there, ordinary stepping.
    font->stepUp();
    ok(font->value() == 15, "the next step is an ordinary one");
    font->stepDown();
    ok(font->value() == 14, "and steps back down normally");

    // Down from inherit has nowhere to go and must not wrap or pin.
    SessionEditorDialog fresh(in, nullptr, nullptr, settings, omega);
    for (QSpinBox *box : fresh.findChildren<QSpinBox *>()) {
        if (box->specialValueText().contains(QStringLiteral("pt"))) {
            box->stepDown();
            ok(box->value() == box->minimum(),
               "stepping down off inherit stays on inherit");
        }
    }
    ok(!fresh.session().font_size.has_value(),
       "and the form still saves it as absent");
}

// A frameless modal must have an edge of its own.
//
// With no title bar the dialog and the window behind it both paint bg.base,
// so the body's sides dissolve and the modal reads as a floating title strip.
// The fix is two halves and neither shows on its own: the sheet draws a border
// on QDialog[chromeless="true"], and the root layout reserves its width --
// with zero margins the border is drawn and then painted over by the title
// strip, the scroll area and the footer, which looks exactly like no border.
//
// Both halves are checked here because losing either one produces the same
// symptom, and the symptom is "the dialog looks slightly off" rather than
// anything that fails loudly.
void framelessModalsCarryAnEdge() {
    std::printf("a frameless modal's own border\n");

    AppSettings settings;
    OmegaSettings omega;
    Session in;
    in.name = "lab-core-1";
    in.hostname = "10.20.0.11";

    // A frameless parent is what turns ModalFrame's title strip on, and with
    // it the chromeless path. Same trigger the shipped application uses.
    QWidget host;
    host.setWindowFlags(Qt::Window | Qt::FramelessWindowHint);

    SessionEditorDialog framelessDialog(in, nullptr, nullptr, settings, omega,
                                        nullptr, &host);
    ok(framelessDialog.property("chromeless").toBool(),
       "a dialog under a frameless window is marked chromeless");
    QLayout *rootLayout = framelessDialog.layout();
    ok(rootLayout != nullptr, "and has a root layout");
    if (rootLayout) {
        ok(rootLayout->contentsMargins().left() >= 1 &&
               rootLayout->contentsMargins().bottom() >= 1,
           "whose margins reserve room for the border to show");
    }

    // No parent means the window manager draws the frame, and a second one
    // inside it is just a line.
    SessionEditorDialog nativeDialog(in, nullptr, nullptr, settings, omega);
    ok(!nativeDialog.property("chromeless").toBool(),
       "a dialog with a native frame is not marked chromeless");
    if (QLayout *l = nativeDialog.layout()) {
        ok(l->contentsMargins().left() == 0,
           "and reserves nothing, so the native frame is the only edge");
    }
}

// The port fields must be able to show a port.
//
// Both were setFixedWidth(80), and 80 was never enough: a spin box sized for
// five digits reports 114 at ui_font_size 13 and 139 at 20, and those numbers
// include the style's padding and the room the spin buttons take. 65535 was
// clipped at every chrome size the application offers; "22" fit only because
// two digits happened to survive.
//
// The assertion is deliberately against the widget's OWN size hint rather than
// a number written here. A probe holding 114 would fail the day the chrome
// font changes and would say "port field broken" when it means "font moved".
void portFieldsFitTheirValue() {
    std::printf("the port fields fit five digits\n");

    AppSettings settings;
    OmegaSettings omega;
    Session in;
    in.name = "lab-core-1";
    in.hostname = "10.20.0.11";
    in.port = 65535;
    in.jump_host = std::string("lab-bastion");
    in.jump_port = 65535;

    SessionEditorDialog dialog(in, nullptr, nullptr, settings, omega);

    int found = 0;
    for (QSpinBox *box : dialog.findChildren<QSpinBox *>()) {
        if (box->maximum() != 65535) continue;
        ++found;
        // A fixed width pins minimum and maximum together, and pins them below
        // what the widget says it needs. Either half of that is the bug.
        ok(box->maximumWidth() >= box->sizeHint().width(),
           "the port field is allowed to reach its own size hint");
        ok(box->minimumWidth() <= box->sizeHint().width(),
           "and is not floored above it either");
    }
    ok(found == 2, "found both the target port and the jump port");
}

// The same round trip against a real theme directory, so the installed path
// is covered as well as the not-installed one above.
void installedThemeSelects(const std::string &themeDir) {
    std::printf("the theme row against an installed set\n");

    omega::theme::ThemeEngine engine;
    const int loaded = engine.loadDirectory(themeDir);
    ok(loaded > 0, "loaded a theme directory");
    if (loaded <= 0) return;

    // Whatever the set actually holds, rather than a name written down here:
    // a probe that hard-codes a theme fails the day one is renamed, and says
    // "theme row broken" when it means "theme renamed".
    const std::vector<std::string> names = engine.names();
    if (names.empty()) {
        ok(false, "the directory yielded at least one name");
        return;
    }
    const std::string &installed = names.front();

    AppSettings settings;
    OmegaSettings omega;

    Session in;
    in.name = "lab-core-1";
    in.hostname = "10.20.0.11";
    in.theme_name = installed;
    in.font_size = 9;

    const Session out = roundTrip(in, settings, omega, &engine);
    ok(out.theme_name == in.theme_name, "an installed theme round-trips");
    ok(out.font_size == in.font_size, "and the font size beside it");

    // And the inherit row is still reachable with a full combo: a session
    // with no pin must not pick up the first installed name just because the
    // list is no longer empty.
    Session bare;
    bare.name = "lab-core-2";
    bare.hostname = "10.20.0.12";
    const Session bareOut = roundTrip(bare, settings, omega, &engine);
    ok(!bareOut.theme_name.has_value(),
       "a session with no pin stays unpinned against a populated list");
    ok(!bareOut.font_size.has_value(), "and its font size stays absent");
}

void offIsNotAbsent() {
    std::printf("a flag switched off is not a flag left alone\n");

    AppSettings settings;
    OmegaSettings omega;
    omega.anti_idle.enabled = true;  // the global says on

    Session in;
    in.name = "lab-core-1";
    in.hostname = "10.20.0.11";
    in.anti_idle_enabled = false;  // and this session says no
    in.legacy_algorithms = false;

    const Session out = roundTrip(in, settings, omega);

    ok(out.anti_idle_enabled.has_value() && *out.anti_idle_enabled == false,
       "an explicit off survives a global that is on");
    ok(out.legacy_algorithms.has_value() && *out.legacy_algorithms == false,
       "and so does an explicit off with no global behind it");
}

void unpacedIsAValue() {
    std::printf("paste rate zero\n");

    AppSettings settings;
    OmegaSettings omega;

    Session in;
    in.name = "lab-core-1";
    in.hostname = "10.20.0.11";
    in.paste_baud = 0;

    const Session out = roundTrip(in, settings, omega);

    // 0 is unpaced, which is a decision, and it must not collapse into the
    // absence that means "let the tab pick from the transport".
    ok(out.paste_baud.has_value() && *out.paste_baud == 0,
       "unpaced round-trips as a value, not as absence");
}

void serialClearsWhatItCannotUse() {
    std::printf("a serial session\n");

    AppSettings settings;
    OmegaSettings omega;

    Session in;
    in.name = "lab-console";
    in.transport = SessionTransport::Serial;
    in.serial_port = std::string("/dev/ttyUSB0");
    in.serial_baud = 9600;
    in.credential_name = std::string("lab-admin");
    in.host_key_policy = std::string("tofu");
    in.jump_host = std::string("lab-bastion");
    in.term_type = std::string("vt100");

    const Session out = roundTrip(in, settings, omega);

    ok(out.transport == SessionTransport::Serial, "stays serial");
    ok(out.serial_port == in.serial_port, "keeps its device name");
    ok(out.serial_baud == in.serial_baud, "keeps its rate");
    ok(out.hostname == "/dev/ttyUSB0", "carries the device name for display");
    ok(!out.credential_name.has_value(), "drops a credential it cannot use");
    ok(!out.host_key_policy.has_value(), "drops a host key policy");
    ok(!out.jump_host.has_value(), "drops a jump host");
    ok(out.term_type == in.term_type,
       "and keeps the terminal type, which every transport has");
}

void telnetClearsCredentials() {
    std::printf("a telnet session\n");

    AppSettings settings;
    OmegaSettings omega;

    Session in;
    in.name = "lab-term-svr";
    in.hostname = "10.20.0.30";
    in.port = 23;
    in.transport = SessionTransport::Telnet;
    in.credential_name = std::string("lab-admin");
    in.telnet_crlf = false;

    const Session out = roundTrip(in, settings, omega);

    ok(out.transport == SessionTransport::Telnet, "stays telnet");
    ok(out.port == 23, "keeps its port");
    ok(!out.credential_name.has_value(),
       "drops a credential -- telnet has no authentication step");
    ok(out.telnet_crlf.has_value() && *out.telnet_crlf == false,
       "keeps CR LF switched off");
}

void inheritedKeystrokeClearsCustomBytes() {
    std::printf("anti-idle custom bytes\n");

    AppSettings settings;
    OmegaSettings omega;

    Session in;
    in.name = "lab-core-1";
    in.hostname = "10.20.0.11";
    in.anti_idle_custom = std::string("0d");  // with no keystroke naming custom

    const Session out = roundTrip(in, settings, omega);

    // Bytes with nothing selecting them are bytes nothing will ever send, and
    // a column holding them reads as a feature that is on.
    ok(!out.anti_idle_custom.has_value(),
       "custom bytes go when the keystroke is inherited");

    Session unknown = in;
    unknown.anti_idle_keystroke = std::string("f13");
    const Session survived = roundTrip(unknown, settings, omega);
    ok(survived.anti_idle_keystroke == unknown.anti_idle_keystroke,
       "a keystroke spelling this build does not know is kept, not dropped");

    Session custom = in;
    custom.anti_idle_keystroke = std::string("custom");
    const Session kept = roundTrip(custom, settings, omega);
    ok(kept.anti_idle_custom == custom.anti_idle_custom,
       "and stay when the keystroke names them");
}

}  // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    // Optional, and defaulted to the tree's own set so the probe runs with no
    // arguments the way every other one here does.
    const std::string themeDir = argc > 1 ? argv[1] : "theme/themes";

    bareSessionStaysBare();
    everyOverrideSurvives();
    offIsNotAbsent();
    unpacedIsAValue();
    serialClearsWhatItCannotUse();
    telnetClearsCredentials();
    inheritedKeystrokeClearsCustomBytes();
    installedThemeSelects(themeDir);
    steppingOffInheritLandsOnTheInheritedValue();
    portFieldsFitTheirValue();
    framelessModalsCarryAnEdge();

    std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
