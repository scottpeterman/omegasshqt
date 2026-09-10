// tests/compat/modal_probe.cpp
//
// Renders one modal, offscreen, under a real generated token sheet, and prints
// the facts a PNG cannot carry.
//
//   modal_probe <themes-dir> <theme|all> <ui-font-px> <merged|native> <outdir> [mode]
//
// `merged` builds the dialog under a frameless parent, which is what turns
// ModalFrame's title strip on; `native` leaves the parent with a window
// manager frame and the strip off. Both are reachable in the shipped
// application -- --native-frame and the chrome setting -- and they are
// different layouts, so a sweep wants both.
//
// WHY THE PRINTED LINES MATTER MORE THAN THE PNG. A grab shows that something
// rendered. It does not show which button Return activates, what family a
// label actually resolved to after the sheet landed, or whether a string that
// happens to fit at 13px fits at 20. Every bug this harness has found so far
// was in the text, not in the picture: a fingerprint drawn in a proportional
// face because the named family was absent, a primary button that was not the
// default one, a line that wrapped one size up. So each mode prints its own
// assertions and main() prints the ones every dialog shares.
//
// The greps that go with a sweep:
//
//   grep 'primary=true' out.txt | grep -v 'default=true'   # must be empty
//   grep -c 'onOneLine=NO' out.txt                          # must be 0
//   grep 'resolved' out.txt | grep -v Mono                  # mono, must be empty
//   grep -oE 'size=[0-9]+x[0-9]+' out.txt | sort -u         # one size
//
// Runs with QT_QPA_PLATFORM=offscreen. propagateSizeHints warnings from the
// offscreen plugin are noise; see the sandbox README.

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFontInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QStringList>
#include <QEventLoop>
#include <QKeyEvent>
#include <QWidget>

#include <cstdarg>
#include <memory>
#include <cstdio>
#include <vector>

#include "app/credentialeditordialog.h"
#include "app/credentialmanagerdialog.h"
#include "app/credentialpromptdialog.h"
#include "app/currenttokens.h"
#include "app/hostkeyerror.h"
#include "app/helpdialog.h"
#include "app/hostkeypromptdialog.h"
#include "app/omegasettings.h"
#include "app/settings.h"
#include "app/sessioneditordialog.h"
#include "app/settingsdialog.h"
#include "app/pasteconfirmdialog.h"
#include "app/quickconnectdialog.h"
#include "sessions/store.h"
#include "app/vaultunlockdialog.h"
#include "omegasshvault.h"
#include "theme/theme.h"
#include "theme/tokens.h"
#include "theme/tokenstylesheet.h"

using namespace omega::app;
using omegassh::AuthMethod;
using omegassh::CredentialInput;
using omegassh::CredentialMeta;
using omegassh::Vault;
using omegassh::VaultError;

namespace {

// Long enough that the vault does not reject it as weak. A short one comes
// back VaultError::WeakPassword (16) and reads as a vault bug for ten minutes.
const char *kMaster = "probe-master-passphrase-long-enough-to-pass";

// The parent. ModalFrame decides whether to draw a title strip by reading
// windowFlags() on parentWidget()->window(), so the only way to exercise both
// paths is to give it a real parent with the flag set or clear.
QWidget *makeParent(bool frameless) {
    auto *w = new QWidget;
    if (frameless) w->setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    w->resize(900, 600);
    return w;
}

// ---------------------------------------------------------------------------
// reporting
// ---------------------------------------------------------------------------

void line(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fputc('\n', stdout);
}

// The face and size a widget ACTUALLY got, which is the only useful question:
// the sheet names a stack, the stack may name a family that is not installed,
// and Qt falls through silently. QFontInfo answers with what was resolved,
// QFont with what was asked for.
void reportFont(const char *what, const QWidget *w) {
    const QFontInfo info(w->font());
    line("  %s: resolved=%s px=%d fixedPitch=%s", what,
         qPrintable(info.family()), info.pixelSize(),
         info.fixedPitch() ? "yes" : "no");
}

// Whether a string fits the width it was given.
//
// ONLY ASKED OF LABELS THAT MUST NOT WRAP. A notice is word-wrapped on
// purpose, so measuring it against one line reports NO for every notice in the
// tree and the §5 grep -- which says the count of onOneLine=NO must be zero --
// becomes noise that is always non-zero and therefore never read. The labels
// this is a real question for are the ones with wordWrap off: a field label, a
// chip, a readout, the footer hint. Those are the ones that get clipped or
// elided when the dialog is too narrow, which is the bug being looked for.
void reportOneLine(const char *what, const QLabel *label) {
    if (label->wordWrap()) return;
    if (label->text().isEmpty()) return;
    const int need = label->fontMetrics().horizontalAdvance(label->text());
    const int have = label->contentsRect().width();
    line("  %s \"%s\": onOneLine=%s need=%d have=%d", what,
         qPrintable(label->text()), need <= have ? "yes" : "NO", need, have);
}

// Every footer button, with the two properties that must agree. `primary` is
// what the sheet paints the accent with; `default` is what Return activates.
// A row with primary=true and default=false is the failure dialogbuttons.h
// and ModalFrame::addButton exist to prevent, and it is not visible in a grab
// because both buttons look the same shape.
void reportButtons(const QDialog *dialog) {
    const QList<QPushButton *> buttons = dialog->findChildren<QPushButton *>();
    for (const QPushButton *b : buttons) {
        line("  button \"%s\": primary=%s default=%s autoDefault=%s enabled=%s",
             qPrintable(b->text()),
             b->property("primary").toBool() ? "true" : "false",
             b->isDefault() ? "true" : "false",
             b->autoDefault() ? "true" : "false",
             b->isEnabled() ? "true" : "false");
    }
}

// Which role frames exist and are visible. A hidden notice and an absent one
// look identical in a PNG, and they are different bugs.
void reportRoleFrames(const QDialog *dialog) {
    const QList<QFrame *> frames = dialog->findChildren<QFrame *>();
    for (QFrame *f : frames) {
        // QLabel DERIVES FROM QFRAME, so an unfiltered findChildren<QFrame*>
        // returns every role LABEL as well -- which is the same inheritance
        // that made QFrame[role="notice"] match the label inside the notice
        // frame and draw a second border. Reporting them together here would
        // hide exactly the collision this pass renamed `noticebox` to prevent:
        // a container and a label sharing a role would read as one line.
        if (qobject_cast<QLabel *>(f)) continue;
        const QString role = f->property("role").toString();
        if (role.isEmpty()) continue;
        line("  frame role=%s visible=%s size=%dx%d", qPrintable(role),
             f->isVisible() ? "yes" : "no", f->width(), f->height());
    }
}

// The role LABELS, reported separately and one-line-checked. Kept apart from
// the frames for the reason in reportRoleFrames.
void reportRoleLabels(const QDialog *dialog) {
    for (const QLabel *l : dialog->findChildren<QLabel *>()) {
        const QString role = l->property("role").toString();
        if (role.isEmpty()) continue;
        line("  label role=%s visible=%s wrap=%s size=%dx%d", qPrintable(role),
             l->isVisible() ? "yes" : "no", l->wordWrap() ? "yes" : "no",
             l->width(), l->height());
        if (l->isVisible()) reportOneLine("   label", l);
    }
}

// Fields marked invalid, and fields not marked. The border is one pixel of one
// colour; asking the property is the reliable form.
void reportInvalid(const QDialog *dialog) {
    const QList<QLineEdit *> edits = dialog->findChildren<QLineEdit *>();
    for (const QLineEdit *e : edits) {
        if (!e->property("invalid").toBool()) continue;
        line("  invalid=true on QLineEdit placeholder=\"%s\" text=\"%s\"",
             qPrintable(e->placeholderText()), qPrintable(e->text()));
    }
    const QList<QComboBox *> combos = dialog->findChildren<QComboBox *>();
    for (const QComboBox *c : combos) {
        if (!c->property("invalid").toBool()) continue;
        line("  invalid=true on QComboBox current=\"%s\"",
             qPrintable(c->currentText()));
    }
}

// Which page a tabbed dialog is showing, and what the tabs are called. A grab
// shows one page; whether the dialog put the RIGHT page in front after a
// failed validation is not visible in it at all, because the notice renders
// identically either way.
void reportTabs(const QDialog *dialog) {
    for (const QTabWidget *t : dialog->findChildren<QTabWidget *>()) {
        QStringList names;
        for (int i = 0; i < t->count(); ++i) names << t->tabText(i);
        line("  tabs=[%s] showing=\"%s\"",
             qPrintable(names.join(QStringLiteral(" | "))),
             qPrintable(t->tabText(t->currentIndex())));
    }
}

// The segmented control: which segments exist and which is checked. `checked`
// is what the sheet paints with the accent, and a grab of an accented pill
// cannot say whether the STATE agrees with the page that is showing.
void reportSegments(const QDialog *dialog) {
    QStringList names;
    QString checked;
    for (const QPushButton *b : dialog->findChildren<QPushButton *>()) {
        if (!b->property("segment").toBool()) continue;
        names << b->text();
        if (b->isChecked()) checked = b->text();
    }
    if (names.isEmpty()) return;
    line("  segments=[%s] checked=\"%s\"",
         qPrintable(names.join(QStringLiteral(" | "))), qPrintable(checked));
}

void reportCombos(const QDialog *dialog) {
    const QList<QComboBox *> combos = dialog->findChildren<QComboBox *>();
    for (const QComboBox *c : combos) {
        QStringList items;
        for (int i = 0; i < c->count(); ++i) items << c->itemText(i);
        line("  combo current=\"%s\" enabled=%s items=[%s]",
             qPrintable(c->currentText()), c->isEnabled() ? "yes" : "no",
             qPrintable(items.join(QStringLiteral(" | "))));
    }
}

// The chips and the mono labels, which is where the font trap lives. A chip is
// mono at caption size and a readout is mono at body size; both must resolve
// to a fixed-pitch family or the sheet's stack fell through to the default.
void reportMonoLabels(const QDialog *dialog) {
    const QList<QLabel *> labels = dialog->findChildren<QLabel *>();
    for (const QLabel *l : labels) {
        const bool mono = l->property("mono").toBool();
        const QString chip = l->property("chip").toString();
        if (!mono && chip.isEmpty()) continue;
        const QFontInfo info(l->font());
        line("  %s \"%s\": resolved=%s px=%d fixedPitch=%s visible=%s",
             mono ? "mono" : "chip", qPrintable(l->text()),
             qPrintable(info.family()), info.pixelSize(),
             info.fixedPitch() ? "yes" : "no", l->isVisible() ? "yes" : "no");
        if (l->isVisible()) reportOneLine(mono ? "   mono" : "   chip", l);
    }
}

// ---------------------------------------------------------------------------
// vault fixtures
// ---------------------------------------------------------------------------

QString scratchVaultPath(const QString &tag) {
    return QDir::temp().filePath(QStringLiteral("omega-modal-probe-%1.json").arg(tag));
}

// A vault with three credentials in it, opened. Removed first: a probe that
// reuses yesterday's file is a probe whose fixture drifts.
Vault *makePopulatedVault(const QString &tag) {
    const QString path = scratchVaultPath(tag);
    QFile::remove(path);

    auto *vault = new Vault(path);
    VaultError e = vault->create(QString::fromLatin1(kMaster));
    if (e != VaultError::Ok) {
        line("  FIXTURE FAILED: create -> %s (%s)",
             qPrintable(Vault::errorName(e)), qPrintable(vault->lastError()));
        return vault;
    }

    CredentialInput a;
    a.name = QStringLiteral("lab-admin");
    a.username = QStringLiteral("admin");
    a.auth = AuthMethod::Password;
    a.password = QStringLiteral("not-a-real-password");
    a.description = QStringLiteral("Lab switches and the console server");
    a.priority = 10;
    a.tags = QStringList{QStringLiteral("lab"), QStringLiteral("access")};
    a.scope.domainSuffix = QStringLiteral("lab.local");
    a.scope.cidrs = QStringList{QStringLiteral("10.20.0.0/16"),
                                QStringLiteral("192.0.2.0/24")};
    a.scope.platforms = QStringList{QStringLiteral("ios"), QStringLiteral("nxos")};
    a.isDefault = true;
    vault->store(a);
    a.wipeSecrets();

    CredentialInput b;
    b.name = QStringLiteral("edge-key");
    b.username = QStringLiteral("netops");
    b.auth = AuthMethod::PublicKey;
    b.keyPath = QStringLiteral("~/.ssh/id_ed25519");
    b.keyPassphrase = QStringLiteral("also-not-real");
    b.description = QStringLiteral("Peering edge, key auth only");
    b.priority = 5;
    vault->store(b);
    b.wipeSecrets();

    // No material at all: agent auth carries none, which is the record that
    // makes the well's chip say "No material stored" and is otherwise never
    // rendered anywhere in a sweep.
    CredentialInput d;
    d.name = QStringLiteral("agent-only");
    d.username = QStringLiteral("netops");
    d.auth = AuthMethod::Agent;
    d.description = QStringLiteral("Forwarded agent, nothing stored here");
    vault->store(d);

    CredentialInput c;
    c.name = QStringLiteral("legacy-console");
    c.username = QStringLiteral("root");
    c.auth = AuthMethod::Password;
    c.password = QStringLiteral("still-not-real");
    c.disabled = true;
    vault->store(c);
    c.wipeSecrets();

    return vault;
}

// The metadata for one entry by name, as the credential manager would hand it
// to the editor: redacted, exactly as the vault returned it.
CredentialMeta metaByName(Vault *vault, const QString &name) {
    CredentialMeta m;
    const VaultError e = vault->meta(name, &m);
    if (e != VaultError::Ok) {
        line("  FIXTURE FAILED: meta(%s) -> %s", qPrintable(name),
             qPrintable(Vault::errorName(e)));
    }
    return m;
}

// ---------------------------------------------------------------------------
// modes
// ---------------------------------------------------------------------------

// Each builder returns a shown dialog. Anything a mode wants to say about
// itself it says here; the shared reporting runs afterwards in render().

QDialog *buildHostKey(QWidget *parent) {
    HostKeyInfo info;
    info.unknownHost = true;
    info.hostname = QStringLiteral("core-rtr-01.lab.local:22");
    info.keyType = QStringLiteral("ssh-ed25519");
    // A real SHA256 fingerprint, at its real length. A shortened one is the
    // difference between the width calculation being exercised and not.
    info.fingerprint =
        QStringLiteral("SHA256:uNiVwhcCiCiPYAG2SUJmqzUYs1RA3PPFBnhWLDpQXOo");
    info.knownHostsPath = QStringLiteral("/home/operator/.ssh/known_hosts");
    return new HostKeyPromptDialog(info, parent);
}

QDialog *buildPaste(QWidget *parent, int baud) {
    QString preview;
    for (int i = 1; i <= 40; ++i) {
        preview += QStringLiteral(
                       "interface GigabitEthernet0/0/%1\n"
                       " description uplink to spine-%1\n"
                       " ip address 10.20.%1.1 255.255.255.0\n")
                       .arg(i);
    }
    return new PasteConfirmDialog(preview, 120, preview.size(), baud, parent);
}

QDialog *buildCredentialEditor(QWidget *parent, const QString &mode,
                               Vault **vaultOut) {
    if (mode == QLatin1String("crededitadd")) {
        return new CredentialEditorDialog(CredentialMeta(), parent);
    }

    Vault *vault = makePopulatedVault(QStringLiteral("crededit"));
    *vaultOut = vault;

    if (mode == QLatin1String("crededitnone")) {
        return new CredentialEditorDialog(metaByName(vault,
                                                     QStringLiteral("agent-only")),
                                          parent);
    }
    if (mode == QLatin1String("crededitkey")) {
        return new CredentialEditorDialog(metaByName(vault,
                                                     QStringLiteral("edge-key")),
                                          parent);
    }
    return new CredentialEditorDialog(metaByName(vault,
                                                 QStringLiteral("lab-admin")),
                                      parent);
}

// The manager, in each of the four states its own refresh() distinguishes:
// no handle, no file yet, locked, and open with rows.
QDialog *buildCredentialManager(QWidget *parent, const QString &mode,
                                Vault **vaultOut) {
    if (mode == QLatin1String("credmgrnovault")) {
        // A path that cannot be opened at all -- the "no vault handle" branch,
        // which is otherwise only reachable by breaking a real vault.
        const QString path = scratchVaultPath(QStringLiteral("credmgrnovault"));
        QFile::remove(path);
        auto *vault = new Vault(path);
        *vaultOut = vault;
        return new CredentialManagerDialog(vault, parent);
    }

    Vault *vault = makePopulatedVault(QStringLiteral("credmgr"));
    *vaultOut = vault;

    if (mode == QLatin1String("credmgrlocked")) vault->lock();
    return new CredentialManagerDialog(vault, parent);
}

// Settings, in the two shapes its own enabled-state logic distinguishes:
// anti-idle off, and anti-idle on with a custom hex keystroke, which is the
// only state where the byte-count note is visible at all.
QDialog *buildSettings(QWidget *parent, const QString &mode,
                       omega::theme::ThemeEngine *themes) {
    AppSettings shared;
    OmegaSettings omega;
    if (mode.contains(QLatin1String("idle"))) {
        omega.anti_idle.enabled = true;
        omega.anti_idle.seconds = 120;
        omega.anti_idle.keystroke = AntiIdleKeystroke::Custom;
        omega.anti_idle.custom = QByteArray::fromHex("0d0a");
    }
    return new SettingsDialog(shared, omega, themes, parent);
}

// Help and About. Help is built directly rather than through HelpDialog::open,
// which parks a QPointer in a file-static and sets WA_DeleteOnClose -- a probe
// that went through it would leak a window per render and delete it out from
// under the grab.
QDialog *buildHelp(QWidget *parent, const QString &mode,
                   const omega::theme::Tokens &tokens) {
    if (mode == QLatin1String("about")) {
        // The accent, as MainWindow passes it. Anchors resolve their colour
        // from the stylesheet's palette rather than the widget's, and the
        // theme QSS says nothing about links, so without this the anchor comes
        // back Qt's default dark blue -- very nearly invisible on a dark
        // background.
        return new AboutDialog(
            QString::fromStdString(omega::theme::hex(tokens.accent)), parent);
    }
    auto *dialog = new HelpDialog(parent);
    if (mode == QLatin1String("helplicenses")) {
        dialog->showTopic(QStringLiteral("licenses"));
    }
    return dialog;
}

// The session editor. Ten forms across three tabs, and the only dialog whose
// fixture needs a store as well as a vault.
QDialog *buildSessionEditor(QWidget *parent, const QString &mode,
                            Vault **vaultOut,
                            omega::sessions::SessionStore **storeOut) {
    const QString dbPath =
        QDir::temp().filePath(QStringLiteral("omega-modal-probe-sessions.db"));
    QFile::remove(dbPath);
    // open() is a static factory returning unique_ptr; the constructor is
    // private. Released into a raw pointer because render() owns the lifetime
    // of every fixture uniformly and deletes them at the end.
    std::unique_ptr<omega::sessions::SessionStore> owned =
        omega::sessions::SessionStore::open(dbPath.toStdString());
    if (!owned) {
        line("  FIXTURE FAILED: could not open session store at %s",
             qPrintable(dbPath));
    }
    omega::sessions::SessionStore *store = owned.release();
    *storeOut = store;

    Vault *vault = makePopulatedVault(QStringLiteral("session"));
    *vaultOut = vault;

    omega::sessions::Session session;
    if (mode != QLatin1String("sessionnew")) {
        session.id = 1;
        session.name = "lab-core-1";
        session.description = "Core switch, lab rack 3";
        session.hostname = "core-rtr-01.lab.local";
        session.port = 22;
        session.credential_name = "lab-admin";
        session.transport = mode == QLatin1String("sessionserial")
                                ? omega::sessions::SessionTransport::Serial
                                : omega::sessions::SessionTransport::Ssh;
        session.serial_port = "/dev/ttyUSB0";
        session.serial_baud = 9600;
    }

    AppSettings shared;
    OmegaSettings omega;
    return new SessionEditorDialog(session, store, vault, shared, omega, parent);
}

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------

void render(const omega::theme::Theme &theme, const QString &mode,
            int uiFontPx, bool frameless, const QString &outDir,
            omega::theme::ThemeEngine *themes) {
    const omega::theme::Tokens tokens = omega::theme::tokensFromTheme(theme);
    setCurrentTokens(tokens);

    // Both levers, exactly as MainWindow::applyTheme sets them: the sheet
    // covers the widgets it names and the application font covers the rest.
    // A probe that set only one measures a font combination the application
    // never produces.
    QFont appFont = QApplication::font();
    appFont.setPixelSize(uiFontPx);
    QApplication::setFont(appFont);

    const QString qss = QString::fromStdString(
        omega::theme::generateTokenStylesheet(tokens, uiFontPx));

    QWidget *parent = makeParent(frameless);
    parent->setStyleSheet(qss);
    parent->show();

    Vault *vault = nullptr;
    omega::sessions::SessionStore *store = nullptr;
    QDialog *dialog = nullptr;

    if (mode.startsWith(QLatin1String("hostkey"))) {
        dialog = buildHostKey(parent);
    } else if (mode == QLatin1String("paste")) {
        dialog = buildPaste(parent, 0);
    } else if (mode == QLatin1String("paste9600")) {
        dialog = buildPaste(parent, 9600);
    } else if (mode == QLatin1String("cred")) {
        dialog = new CredentialPromptDialog(
            QStringLiteral("admin@core-rtr-01.lab.local:22"), nullptr, parent);
    } else if (mode == QLatin1String("credfull")) {
        vault = makePopulatedVault(QStringLiteral("credfull"));
        dialog = new CredentialPromptDialog(
            QStringLiteral("admin@core-rtr-01.lab.local:22"), vault, parent);
    } else if (mode == QLatin1String("credlocked")) {
        vault = makePopulatedVault(QStringLiteral("credlocked"));
        vault->lock();
        dialog = new CredentialPromptDialog(
            QStringLiteral("admin@core-rtr-01.lab.local:22"), vault, parent);
    } else if (mode.startsWith(QLatin1String("vault"))) {
        const QString path = scratchVaultPath(mode);
        if (mode == QLatin1String("vaultnew")) QFile::remove(path);
        vault = new Vault(path);
        if (mode != QLatin1String("vaultnew") && !vault->exists()) {
            vault->create(QString::fromLatin1(kMaster));
            vault->lock();
        }
        dialog = new VaultUnlockDialog(vault, parent);
    } else if (mode.startsWith(QLatin1String("help")) || mode == QLatin1String("about")) {
        dialog = buildHelp(parent, mode, tokens);
    } else if (mode.startsWith(QLatin1String("settings"))) {
        dialog = buildSettings(parent, mode, themes);
    } else if (mode.startsWith(QLatin1String("session"))) {
        dialog = buildSessionEditor(parent, mode, &vault, &store);
    } else if (mode.startsWith(QLatin1String("quick"))) {
        // With a populated, unlocked vault unless the mode says otherwise --
        // the credential picker is the only part of this dialog whose content
        // depends on anything outside it.
        if (mode != QLatin1String("quicknovault")) {
            vault = makePopulatedVault(QStringLiteral("quick"));
            if (mode == QLatin1String("quicklocked")) vault->lock();
        }
        dialog = new QuickConnectDialog(vault, parent);
    } else if (mode.startsWith(QLatin1String("credmgr"))) {
        dialog = buildCredentialManager(parent, mode, &vault);
    } else if (mode.startsWith(QLatin1String("crededit"))) {
        dialog = buildCredentialEditor(parent, mode, &vault);
    } else {
        line("unknown mode: %s", qPrintable(mode));
        delete parent;
        return;
    }

    dialog->show();
    QApplication::processEvents();

    // The interactive states, driven after the first show so the dialog has
    // been polished -- which is the whole point of the repolish() rule. A
    // property set before the first polish would look right here and wrong in
    // the application.
    if (mode == QLatin1String("vaultwrong")) {
        if (auto *pw = dialog->findChild<QLineEdit *>()) {
            pw->setText(QStringLiteral("wrong-master-password-entirely"));
        }
        if (auto *ok = dialog->findChildren<QPushButton *>().value(1)) {
            ok->click();
        }
        QApplication::processEvents();
    } else if (mode == QLatin1String("vaultstale")) {
        // KeyringStale cannot be produced in the sandbox -- there is no
        // keyring at all -- so the quiet-result path is driven directly. Named
        // so a sweep does not read it as a real keyring test.
        static_cast<VaultUnlockDialog *>(dialog)->setQuietResult(
            VaultError::KeyringStale);
        QApplication::processEvents();
    } else if (mode == QLatin1String("sessionterminal") ||
               mode == QLatin1String("sessionadvanced")) {
        // The inherit-bearing pages. Every override row on these two has to
        // keep "inherit" distinct from every value the field can take, and
        // that distinction is the thing a grab is for.
        const QString want = mode == QLatin1String("sessionterminal")
                                 ? QStringLiteral("Terminal")
                                 : QStringLiteral("Advanced");
        if (auto *tabs = dialog->findChild<QTabWidget *>()) {
            for (int i = 0; i < tabs->count(); ++i) {
                if (tabs->tabText(i) == want) {
                    tabs->setCurrentIndex(i);
                    break;
                }
            }
        }
        QApplication::processEvents();
    } else if (mode == QLatin1String("sessionproblem")) {
        // Save with the name cleared. Picked by placeholder and by the primary
        // property, never by position -- this is the deepest widget tree in
        // the application and findChild order is a layout detail.
        for (QLineEdit *edit : dialog->findChildren<QLineEdit *>()) {
            if (edit->placeholderText() == QLatin1String("lab-core-1")) {
                edit->clear();
                break;
            }
        }
        for (QPushButton *b : dialog->findChildren<QPushButton *>()) {
            if (b->property("primary").toBool()) {
                b->click();
                break;
            }
        }
        QApplication::processEvents();
    } else if (mode == QLatin1String("crededitproblem")) {
        // Save with the name cleared: the one hard validation failure, and the
        // one that marks a field.
        //
        // BOTH WIDGETS PICKED BY IDENTITY, NOT BY POSITION. This mode used
        // findChild<QLineEdit*>() and buttons.last(), and both were correct
        // right up until the editor was tabbed -- findChild searches every
        // direct child before recursing, so moving the name field one level
        // deeper handed back a different QLineEdit, the name stayed filled in,
        // validate() passed and the dialog ACCEPTED. The mode then reported no
        // invalid field and no notice, and every §5 grep passed, because a
        // check with nothing to check cannot fail. A probe that stops probing
        // and still says green is worse than no probe.
        for (QLineEdit *edit : dialog->findChildren<QLineEdit *>()) {
            if (edit->placeholderText() == QLatin1String("lab-admin")) {
                edit->clear();
                break;
            }
        }
        for (QPushButton *b : dialog->findChildren<QPushButton *>()) {
            if (b->property("primary").toBool()) {
                b->click();
                break;
            }
        }
        QApplication::processEvents();
    } else if (mode == QLatin1String("quicktelnet") ||
               mode == QLatin1String("quickserial")) {
        // Click the segment rather than setting an index: the point of the
        // control is that clicking it drives the page switch and the row
        // enabling, and a probe that bypassed the click would test neither.
        const QString want = mode == QLatin1String("quicktelnet")
                                 ? QStringLiteral("Telnet")
                                 : QStringLiteral("Serial");
        for (QPushButton *b : dialog->findChildren<QPushButton *>()) {
            if (b->property("segment").toBool() && b->text() == want) {
                b->click();
                break;
            }
        }
        QApplication::processEvents();
    } else if (mode.startsWith(QLatin1String("settingsidle"))) {
        // The Anti-idle page in front, since that is the one these modes
        // exist to look at, and a grab of the Appearance tab would show none
        // of it.
        if (auto *tabs = dialog->findChild<QTabWidget *>()) {
            for (int i = 0; i < tabs->count(); ++i) {
                if (tabs->tabText(i).contains(QLatin1String("idle"))) {
                    tabs->setCurrentIndex(i);
                    break;
                }
            }
        }
        if (mode == QLatin1String("settingsidlebad")) {
            // An odd number of hex digits. The validator permits it -- one
            // refusing an odd length would refuse every first digit typed --
            // so this is the state where anti-idle is configured, enabled and
            // sends nothing, which is the failure the notice exists for.
            for (QLineEdit *e : dialog->findChildren<QLineEdit *>()) {
                if (e->placeholderText().contains(QLatin1String("hex"))) {
                    e->setText(QStringLiteral("0d0"));
                    break;
                }
            }
        }
        QApplication::processEvents();
    } else if (mode == QLatin1String("crededitreplace")) {
        // The material section switched on, which is what shows the hazard
        // notice and re-enables the method combo.
        for (QCheckBox *cb : dialog->findChildren<QCheckBox *>()) {
            if (cb->text().contains(QLatin1String("Replace"))) {
                cb->setChecked(true);
                break;
            }
        }
        // And the page it is about put in front, so the grab shows the state
        // this mode exists to look at rather than whichever tab opened.
        if (auto *tabs = dialog->findChild<QTabWidget *>()) {
            for (int i = 0; i < tabs->count(); ++i) {
                if (tabs->tabText(i).contains(QLatin1String("Material"))) {
                    tabs->setCurrentIndex(i);
                    break;
                }
            }
        }
        QApplication::processEvents();
    } else if (mode == QLatin1String("crededitblankpw")) {
        // Replace ticked, password left empty, Save pressed once: the warning
        // that is not an error and that marks no field.
        for (QCheckBox *cb : dialog->findChildren<QCheckBox *>()) {
            if (cb->text().contains(QLatin1String("Replace"))) {
                cb->setChecked(true);
                break;
            }
        }
        QApplication::processEvents();
        // The primary button by property, for the reason spelled out in
        // crededitproblem above: findChildren order is a layout detail and
        // this mode's whole value depends on pressing Save specifically.
        for (QPushButton *b : dialog->findChildren<QPushButton *>()) {
            if (b->property("primary").toBool()) {
                b->click();
                break;
            }
        }
        QApplication::processEvents();
    }

    // THE ONE THING SETTINGS EXERCISES THAT NOTHING ELSE DOES. Previewing a
    // theme replaces the application stylesheet while this modal is open,
    // which delivers QEvent::StyleChange to it -- ModalFrame's handler for
    // that has existed since the frame landed and, until this mode, had never
    // run. Six dialogs already inherit that code path.
    //
    // Done in the ORDER MainWindow::applyTheme does it: setCurrentTokens
    // BEFORE the setStyleSheet loop. StyleChange is delivered synchronously,
    // so an open dialog re-reads the published tokens during the setStyleSheet
    // call -- publish after and the close button paints the OLD theme while
    // everything around it is new.
    //
    // The font size moves too, because that is the other half of what settings
    // applies live, and a restyle that also changes the metrics is where
    // fitToContent on an already-open modal either holds or does not.
    if (mode == QLatin1String("settingsrestyle")) {
        if (!dialog->grab().save(QStringLiteral("%1/%2-%3-%4-%5-before.png")
                                     .arg(outDir,
                                          QString::fromStdString(tokens.name),
                                          mode, QString::number(uiFontPx),
                                          frameless ? QStringLiteral("merged")
                                                    : QStringLiteral("native")))) {
            line("  GRAB FAILED: before");
        }
        const QSize was = dialog->size();

        // A theme that is not the one it opened under, chosen by name so the
        // difference is visible rather than incidental: light against dark.
        const omega::theme::Theme *other =
            themes ? themes->get("enterprise_light") : nullptr;
        if (!other) {
            line("  restyle SKIPPED: enterprise_light not loaded");
        } else {
            const omega::theme::Tokens t2 = omega::theme::tokensFromTheme(*other);
            const int px2 = uiFontPx + 5;

            setCurrentTokens(t2);
            QFont f2 = QApplication::font();
            f2.setPixelSize(px2);
            QApplication::setFont(f2);
            parent->setStyleSheet(QString::fromStdString(
                omega::theme::generateTokenStylesheet(t2, px2)));
            QApplication::processEvents();

            line("  restyle: %s@%dpx -> %s@%dpx  size %dx%d -> %dx%d",
                 qPrintable(QString::fromStdString(tokens.name)), uiFontPx,
                 qPrintable(QString::fromStdString(t2.name)), px2, was.width(),
                 was.height(), dialog->width(), dialog->height());
            reportFont("dialog after restyle", dialog);
        }
    }

    // --- the shared report ------------------------------------------------
    line("== theme=%s mode=%s uiFontPx=%d frame=%s ==",
         qPrintable(QString::fromStdString(tokens.name)), qPrintable(mode),
         uiFontPx, frameless ? "merged" : "native");
    line("  dialog: size=%dx%d title=\"%s\" frameless=%s",
         dialog->width(), dialog->height(), qPrintable(dialog->windowTitle()),
         dialog->windowFlags().testFlag(Qt::FramelessWindowHint) ? "yes" : "no");
    reportFont("dialog", dialog);
    // WHAT RETURN ACTUALLY DOES, observed rather than derived. Reading which
    // button carries default=true answers a different question: Qt falls
    // through to the first enabled autoDefault button when the default one is
    // disabled, and the exact fallback is version-dependent. Wiring every
    // button and pressing the key is the only form of this that cannot be
    // wrong about the Qt in front of it.
    {
        QStringList fired;
        const QList<QPushButton *> all = dialog->findChildren<QPushButton *>();
        for (QPushButton *b : all) {
            // EVERY REAL SLOT COMES OFF FIRST. Without this the probe hangs:
            // whatever Return lands on runs for real, and on the credential
            // manager that is a QInputDialog or a QMessageBox with nobody to
            // dismiss it. Disconnecting leaves Qt's target SELECTION untouched
            // -- which is the thing being measured -- and removes only the
            // consequence. The dialog is spent afterwards, so this stays the
            // last thing done before the grab.
            QObject::disconnect(b, &QPushButton::clicked, nullptr, nullptr);
            QObject::connect(b, &QPushButton::clicked, dialog,
                             [&fired, b] { fired << b->text(); });
        }
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(dialog, &press);
        // QDialog answers Return with animateClick(), which fires clicked()
        // from a ~100ms timer rather than immediately -- so a single
        // processEvents() races it and reports "(nothing)" for a button that
        // is about to be pressed. Pumped until it settles instead, with a
        // ceiling so a genuinely dead Return still reports as dead rather than
        // hanging the sweep.
        for (int spin = 0; spin < 40 && fired.isEmpty(); ++spin) {
            QApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        // When nothing fires, say WHY. Qt requires the default button to be
        // visible as well as enabled, and "(nothing)" on its own sent an hour
        // into the wrong question -- the answer turned out to be that the
        // dialog was not visible at all, because the step before had
        // accidentally accepted it.
        if (fired.isEmpty()) {
            for (const QPushButton *b : all) {
                line("  returnPressBlocked \"%s\": default=%d enabled=%d "
                     "visible=%d",
                     qPrintable(b->text()), b->isDefault(), b->isEnabled(),
                     b->isVisible());
            }
            line("  returnPressBlocked dialog: visible=%d focus=%s",
                 dialog->isVisible(),
                 dialog->focusWidget()
                     ? dialog->focusWidget()->metaObject()->className()
                     : "none");
        }
        line("  returnPressActivates=%s",
             fired.isEmpty() ? "(nothing)"
                             : qPrintable(fired.join(QStringLiteral(" + "))));
        for (QPushButton *b : all) QObject::disconnect(b, nullptr, dialog, nullptr);
    }

    reportButtons(dialog);
    reportRoleFrames(dialog);
    reportRoleLabels(dialog);
    reportMonoLabels(dialog);
    reportTabs(dialog);
    reportSegments(dialog);
    reportCombos(dialog);
    reportInvalid(dialog);

    // Any notice that is showing, with its text: a failure state is a string
    // and the string is the thing under review.
    for (const QFrame *f : dialog->findChildren<QFrame *>()) {
        if (f->property("role").toString() != QLatin1String("noticebox")) continue;
        if (!f->isVisible()) continue;
        if (const QLabel *l = f->findChild<QLabel *>()) {
            line("  notice: \"%s\"", qPrintable(l->text()));
        }
    }

    QDir().mkpath(outDir);
    const QString png =
        QStringLiteral("%1/%2-%3-%4-%5.png")
            .arg(outDir, QString::fromStdString(tokens.name), mode,
                 QString::number(uiFontPx),
                 frameless ? QStringLiteral("merged") : QStringLiteral("native"));
    if (!dialog->grab().save(png)) {
        line("  GRAB FAILED: %s", qPrintable(png));
    } else {
        line("  png=%s", qPrintable(png));
    }

    delete dialog;
    delete parent;
    delete vault;
    delete store;
}

}  // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 6) {
        std::fprintf(stderr,
                     "usage: modal_probe <themes-dir> <theme|all> <ui-font-px> "
                     "<merged|native> <outdir> [mode]\n"
                     "modes: hostkey paste paste9600 cred "
                     "credfull credlocked vaultopen vaultnew vaultwrong "
                     "vaultstale crededitadd crededit crededitkey crededitnone "
                     "credmgr credmgrlocked credmgrnovault "
                     "settings settingsidle settingsidlebad settingsrestyle "
                     "quick quicktelnet quickserial quicklocked quicknovault "
                     "session sessionnew sessionserial sessionproblem "
                     "sessionterminal sessionadvanced "
                     "help helplicenses about "
                     "crededitreplace crededitproblem crededitblankpw\n");
        return 2;
    }

    const QString themesDir = args.at(1);
    const QString which = args.at(2);
    const int uiFontPx = args.at(3).toInt();
    const bool frameless = args.at(4) == QLatin1String("merged");
    const QString outDir = args.at(5);
    const QString mode =
        args.size() > 6 ? args.at(6) : QStringLiteral("hostkey");

    omega::theme::ThemeEngine themes;
    themes.loadDirectory(themesDir.toStdString());

    std::vector<omega::theme::Theme> selected;
    if (which == QLatin1String("all")) {
        for (const std::string &name : themes.names()) {
            if (const omega::theme::Theme *t = themes.get(name)) {
                selected.push_back(*t);
            }
        }
    } else if (const omega::theme::Theme *t = themes.get(which.toStdString())) {
        selected.push_back(*t);
    } else {
        std::fprintf(stderr, "no such theme: %s\n", qPrintable(which));
        return 2;
    }

    for (const omega::theme::Theme &t : selected) {
        render(t, mode, uiFontPx, frameless, outDir, &themes);
    }
    return 0;
}