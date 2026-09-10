// app/helpdialog.cpp

#include "app/helpdialog.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "app/dialogbuttons.h"

#ifndef OMEGA_VERSION
// Only reachable in a build that did not define it -- a probe target linking
// the shell without app/CMakeLists.txt's compile definition. Says so rather
// than reporting a number it made up.
#define OMEGA_VERSION "unknown"
#endif

// Global namespace on purpose: Q_INIT_RESOURCE expands to a call to a function
// rcc declares at global scope, so invoking it from inside a namespace does not
// compile.
static void omegaInitResourcesImpl() { Q_INIT_RESOURCE(omega); }

namespace omega::app {

void initOmegaResources() { omegaInitResourcesImpl(); }

namespace {

// One window at a time. A modeless dialog opened from a menu item is opened
// from that menu item repeatedly, and without this there is a stack of them.
QPointer<HelpDialog> g_open;

// A footer strip with one button in it, matching what ModalFrame builds for
// the modals -- role="bar" so the sheet paints it, the button right-aligned and
// carrying `primary`.
//
// A PLAIN QPushButton, NOT A QDialogButtonBox, and that is the whole point of
// touching these two windows. A standard button carries the STYLE'S icon:
// QDialogButtonBox::Close renders SP_DialogCloseButton beside the text, and
// that icon comes from the platform icon theme, which the token stylesheet does
// not reach. Under enterprise_dark it painted a near-black cross on a
// near-black button -- not invisible enough to be missed, just a smudge to the
// left of the word. Every dialog converted in this pass escaped it by going
// through ModalFrame::addButton; these two were the last place it survived.
//
// Return still works because setReturnActivates is called explicitly. It was
// not called before, and got away with it: one button in the box meant Qt's
// own autoDefault picked the only candidate. That is true until somebody adds
// a second button, at which point the answer becomes construction order --
// which is exactly how the credential manager ended up with a Return key that
// did nothing.
QPushButton *addFooterButton(QVBoxLayout *layout, QWidget *parent,
                             const QString &text) {
    auto *bar = new QFrame(parent);
    bar->setProperty("role", "bar");
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(16, 10, 16, 10);
    row->addStretch(1);

    auto *button = new QPushButton(text, bar);
    button->setProperty("primary", true);
    row->addWidget(button);

    layout->addWidget(bar);
    setReturnActivates(QList<QPushButton *>{button}, button);
    return button;
}

// --- topic bodies ----------------------------------------------------------
//
// Written against the code rather than from memory: the session fields below
// are the rows built in sessioneditordialog.cpp, and the file list is the set
// of paths named in settings.cpp, omegasettings.cpp, mainwindow.cpp and
// omegasshsession.h. When a row is added to that form, this is the other place
// to change.

QString sessionOptionsHtml() {
    return QStringLiteral(R"(
<h2>Session options</h2>

<p>The session editor has three pages. Anything on the Terminal and Advanced
pages can be left to <b>inherit</b>, which means the session takes whatever the
global setting currently holds -- the inherit row shows that value beside it, so
the form answers "what will this actually do" without a trip to Settings.</p>

<p>Inherit is a real state, not an empty field. A blank text box inherits, a
number box one below its minimum inherits, and a flag is a three-way choice
rather than a checkbox.</p>

<h3>Connection</h3>
<ul>
<li><b>Name</b> -- what the session is called in the tree and on the tab.</li>
<li><b>Description</b> -- optional, shown in the tree.</li>
<li><b>Folder</b> -- where it sits in the tree, or top level.</li>
<li><b>Transport</b> -- SSH, Telnet or Serial. This switches the Target box
between a network host and a serial line.</li>
</ul>

<p>For SSH and Telnet:</p>
<ul>
<li><b>Host</b> and <b>port</b> -- the target. Port defaults to 22.</li>
<li><b>Credential</b> -- a vault entry by <i>name</i>. The name is what is
saved; the secret itself is read during the dial and never comes back into the
application. The <code>(default)</code> row leaves the session naming none, in
which case Settings decides what it dials with -- the vault's default
credential, a prompt each time, or your SSH agent. A locked vault says so in
the picker rather than showing an empty list.</li>
<li><b>Username</b> -- overrides the username on the credential. Explicit
fields win over a reference, so a credential can supply the password while this
supplies the user.</li>
</ul>

<p>For Serial:</p>
<ul>
<li><b>Port</b> -- editable, because a saved session names a port that may not
be plugged in right now.</li>
<li><b>Baud</b> -- the line rate. The mode is fixed at 8 data bits, no parity,
1 stop bit.</li>
</ul>

<h3>Terminal</h3>
<ul>
<li><b>Terminal type</b> -- declared to the far end: the pty request on SSH,
the answer to a TTYPE subnegotiation on Telnet. Serial has nowhere to declare
it.</li>
<li><b>Scrollback lines</b> -- rows of history the emulator keeps. Lowering it
trims immediately, including on tabs that are already open.</li>
<li><b>Confirm above</b> -- lines in a paste before the confirmation dialog
appears.</li>
<li><b>Rate</b> -- paces a paste at the rate the far end is actually set to,
in baud. Unpaced sends the whole block at once, which is what a host with flow
control wants; a console port or terminal server usually does not have any.
Left to inherit, the transport decides: a serial line takes its own baud,
Telnet assumes 9600, SSH is unpaced.</li>
</ul>

<h4>Anti-idle</h4>
<p>A keystroke sent after a period of silence, so the device's own exec-timeout
does not close a session you are still in. It counts terminal input, not
packets, which is why TCP keepalive does not help here.</p>
<ul>
<li><b>Enabled</b>, and <b>After</b> -- seconds of silence before it fires.</li>
<li><b>Keystroke</b> -- Backspace is the convention and is a no-op at an idle
prompt, but it deletes a character if you left a line half typed. Space +
Backspace is safer for that. NUL and a custom hex sequence are also
available.</li>
</ul>
<p>It is suppressed while a full-screen application is up and while a paced
paste is running -- a key at a pager prompt moves the pager on, and a byte in
the middle of a paste corrupts a line.</p>

<h3>Advanced</h3>
<ul>
<li><b>Host key</b> -- per session, because a lab bench and a production edge
do not want the same policy. <i>Strict</i> accepts only what is already in
known_hosts. <i>Trust on first use</i> pins the key the first time and refuses
a change after that. <i>Insecure</i> does not verify at all. A key that does
not match a pinned one always fails, under every policy.</li>
<li><b>Legacy algorithms</b> -- appends the old KEX, cipher and MAC set that
aging gear still requires. Harmless against a modern server.</li>
<li><b>Jump host</b> -- host, username and credential for a bastion. Leave the
host empty to connect directly; the other two are ignored without it.</li>
<li><b>CR becomes CR LF</b> (Telnet) -- expands a lone CR on write, which is
what RFC 854 makes the telnet newline. Turn it off only for a device that
echoes a doubled newline.</li>
</ul>
)");
}

QString filesOnDiskHtml() {
    return QStringLiteral(R"(
<h2>What is kept on disk</h2>

<p>Everything lives in <code>~/.omega</code>. This directory is Omega's alone.
Earlier versions kept these files in <code>~/.nterm</code> and shared them with
nterm-qt; that is no longer the case, and the old directory is left untouched
rather than migrated. A tree, a vault or settings you want carried across come
over by export and import.</p>

<h3>~/.omega/config.json</h3>
<p>The main settings file. Theme name and font size; the paste confirmation
threshold and scrollback lines; the default terminal type, keepalive interval
and auto-reconnect flag; window size, position and maximized state; the session
tree width; and the recent-profile list.</p>
<p>It keeps nterm-qt's schema, so a file copied over by hand is still read --
but nothing else writes it now, and a key it does not recognise is no longer
dropped by another application saving over the top.</p>

<h3>~/.omega/omega.json</h3>
<p>Settings that only this application has -- currently the anti-idle
configuration. A separate file from config.json, which keeps the shared schema
it was born with. Both files are written together when you accept the settings
dialog.</p>

<h3>~/.omega/sessions.db</h3>
<p>The saved sessions and their folders, as SQLite. Two tables: folders, and
sessions with their per-session overrides as columns. Import and export move
sessions in and out of this file in TerminalTelemetry's sessions.yaml format;
the file itself is not the interchange format.</p>

<h3>~/.omega/vault.json</h3>
<p>Credentials. A JSON envelope holding the Argon2id parameters, a per-file
salt, a per-write nonce, and an AES-256-GCM sealed blob -- so names, usernames
and secrets are all inside the encrypted part.</p>
<p>The master password is never written to disk. The key is derived from it and
held in memory only while the vault is unlocked. Locking the vault, or closing
the application, discards it.</p>
<p>This is not nterm-qt's <code>vault.db</code>. They are unrelated formats and
deliberately have different names, so neither application opens the other's file
and reports it corrupt.</p>

<h3>~/.omega/logs/</h3>
<p>Where session captures land when a log is asked for without a specific path.
Captures are plain text with escape sequences stripped. Recording your own
keystrokes is off by default: on Telnet and on a serial console the device's
login prompt is ordinary session data, so the password answering it would be
written to a plaintext file.</p>

<h3>Not in this folder</h3>
<ul>
<li><b>Host keys</b> -- <code>~/.ssh/known_hosts</code>, the same file the
system ssh client uses, unless a session names another path.</li>
<li><b>The vault master password</b>, if you asked to remember it -- your OS
keyring, under the service name <code>PathfinderSSH</code>. Removing that entry
is what "Forget keyring entry" does; it does not touch the vault itself.</li>
</ul>
)");
}

}  // namespace

QString omegaVersion() { return QStringLiteral(OMEGA_VERSION); }

// The licence page.
//
// LGPLv3 SECTION 4 IS WHY THIS EXISTS, not tidiness. Conveying a combined work
// that uses Qt requires prominent notice that the library is used and that its
// use is covered by the LGPL, a copy of the GNU GPL and the LGPL alongside the
// work, and -- for a work that displays copyright notices while running, which
// the About box does -- the library's copyright among them with a pointer to
// those copies. The BSD and MIT modules in the Go archive separately require
// their copyright notices and warranty disclaimers to travel with the binary.
//
// Both obligations are discharged by shipping files: `licenses/LGPL-3.0.txt`,
// `licenses/GPL-3.0.txt` and the generated `THIRD-PARTY-NOTICES.md`. This page
// is the "prominent notice" half and the thing that tells a reader where those
// files are. It deliberately does NOT reproduce the licence texts -- a
// QTextBrowser holding 674 lines of GPLv3 is not more compliant than a
// sentence saying which file to open, and it is a great deal less readable.
//
// THE COMPONENT LIST IS DUPLICATED, and knowingly. THIRD-PARTY-NOTICES.md is
// generated from the module cache and is authoritative; this is a summary a
// user can read without leaving the application. Regenerate that file and
// check this list when a dependency changes -- the same standing obligation
// the topic bodies above already carry for the session form.
QString licensesHtml() {
    // A NOTICE, NOT A PAGE OF HELP. The other topics in this dialog explain
    // how something works and are written accordingly; this one exists to
    // discharge LGPLv3 section 4 and the BSD/MIT notice requirements, and the
    // register is different. It states what is used, under what terms, and
    // where the texts are. It does not narrate how the notices file is
    // produced, and it does not tell the reader which menu item to click --
    // that is a tutorial voice in a document that is meant to be read once and
    // checked, and it dilutes the sentence the licence actually requires.
    return QObject::tr(R"(
<h2>Licences</h2>

<p>Omega is free software under the <b>GNU General Public License, version
3</b>. Full text: <code>LICENSE</code>.</p>

<p>This program uses the <b>Qt Toolkit</b> under the <b>GNU Lesser General
Public License, version 3</b>. Qt is copyright The Qt Company Ltd and other
contributors. Qt is dynamically linked and unmodified, and may be replaced
with another build of the same version. Full text:
<code>licenses/LGPL-3.0.txt</code> and <code>licenses/GPL-3.0.txt</code>.</p>

<h3>Components</h3>
<table cellpadding="3">
<tr><td><b>Qt 6</b></td><td>LGPL-3.0</td><td>dynamically linked</td></tr>
<tr><td><b>anytermqt</b> (qtpyte, pyte)</td><td>GPL-3.0</td>
    <td>terminal widget and emulation core</td></tr>
<tr><td><b>utf8proc</b></td><td>MIT, Unicode data licence</td>
    <td>UTF-8 decoding, character widths</td></tr>
<tr><td><b>Go runtime and standard library</b></td><td>BSD-3-Clause</td>
    <td>statically linked</td></tr>
<tr><td><b>golang.org/x/crypto</b></td><td>BSD-3-Clause</td><td>SSH</td></tr>
<tr><td><b>golang.org/x/sys</b></td><td>BSD-3-Clause</td><td></td></tr>
<tr><td><b>go.bug.st/serial</b></td><td>BSD-3-Clause</td><td>serial</td></tr>
<tr><td><b>github.com/google/uuid</b></td><td>BSD-3-Clause</td><td></td></tr>
<tr><td><b>github.com/godbus/dbus</b></td><td>BSD-2-Clause</td>
    <td>keyring, Linux</td></tr>
<tr><td><b>github.com/zalando/go-keyring</b></td><td>MIT</td><td></td></tr>
<tr><td><b>github.com/creack/goselect</b></td><td>MIT</td><td></td></tr>
<tr><td><b>github.com/danieljoos/wincred</b></td><td>MIT</td>
    <td>keyring, Windows</td></tr>
<tr><td><b>SQLite</b></td><td>public domain</td><td></td></tr>
</table>

<p>Copyright notices and warranty disclaimers for every component above:
<code>THIRD-PARTY-NOTICES.md</code>.</p>

)");
}

const QVector<HelpTopic> &helpTopics() {
    static const QVector<HelpTopic> topics = {
        {QStringLiteral("sessions"), QObject::tr("Session Options"),
         sessionOptionsHtml()},
        {QStringLiteral("files"), QObject::tr("Files on Disk"),
         filesOnDiskHtml()},
        {QStringLiteral("licenses"), QObject::tr("Licences"),
         licensesHtml()},
    };
    return topics;
}

QString aboutHtml(const QString &linkColor) {
    // Inline on the anchor, because that is the only place it survives: see
    // the note on this function in the header.
    const QString style =
        linkColor.isEmpty() ? QString()
                            : QStringLiteral(" style=\"color:%1\"").arg(linkColor);

    // %2 IS THE ANCHOR STYLE, AND IT HAS TO BE USED. This body carried only
    // %1 while still being passed two arguments, so QString::arg substituted
    // the version and dropped `style` on the floor -- silently, because arg()
    // does not complain about an argument with no placeholder to fill. The
    // result was a linkColor threaded all the way from MainWindow through
    // AboutDialog into this function, computed, and discarded, under a header
    // comment explaining at length why it was indispensable. Nothing rendered
    // wrong; there was simply no link.
    //
    // The repository is the one from go.mod's module path -- the authoritative
    // statement of where this code lives -- rather than a URL typed from
    // memory. NO LICENCE LINE: the header says the licence is stated here and
    // there is no LICENSE file in the tree and no HOMEPAGE in CMakeLists, so
    // there is nothing to state that would not be invented.
    return QObject::tr(R"(
<p><b>Version %1</b></p>
<p>A terminal for network gear: SSH, telnet and serial in one window, with a
session tree and an encrypted credential vault.</p>
<p>Built on a Go transport core reached through a C ABI, and the anytermqt
terminal widget.</p>
<p>Free software under the GNU General Public License v3. This program uses
the Qt Toolkit under the GNU Lesser General Public License v3; Qt is
copyright The Qt Company Ltd and other contributors. See
<b>Help &gt; Omega Help &gt; Licences</b>, and the licence files shipped
alongside the application.</p>
<p><a href="https://github.com/scottpeterman/omegassh"%2>github.com/scottpeterman/omegassh</a></p>

)")
        .arg(omegaVersion(), style);
}

AboutDialog::AboutDialog(const QString &linkColor, QWidget *parent)
    : QDialog(parent) {
    initOmegaResources();

    setWindowTitle(tr("About Omega"));
    setModal(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Edge to edge at the top, with no margin around it: the banner has its own
    // border built into the artwork, and a margin puts the window background in
    // a frame around a frame.
    auto *art = new QLabel(this);
    const QPixmap banner(QStringLiteral(":/omega/banner.png"));
    art->setPixmap(banner);
    art->setAlignment(Qt::AlignCenter);
    layout->addWidget(art);

    auto *body = new QWidget(this);
    body->setProperty("bare", true);
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(20, 16, 20, 12);

    auto *text = new QLabel(aboutHtml(linkColor), body);
    text->setWordWrap(true);
    text->setOpenExternalLinks(true);
    // The banner sets the width; without this the label would ask for more and
    // the window would end up wider than the image sitting in it.
    text->setMaximumWidth(banner.width() - 40);
    bodyLayout->addWidget(text);

    layout->addWidget(body);

    // NOT WRAPPED IN ModalFrame, and this is a decision rather than an
    // oversight. ModalFrame puts a title strip above a body with margins, and
    // this window's banner is edge to edge on purpose -- the artwork carries
    // its own border, so an inset would frame a frame. The frame also sizes a
    // dialog to its content, and this one is deliberately pinned to the
    // banner's width. What it wanted from the pass was the footer and the
    // button role, which is what it gets.
    //
    // "Close" rather than "OK": there is nothing here to agree to.
    QPushButton *close = addFooterButton(layout, this, tr("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    setFixedWidth(banner.width());
}

HelpDialog::HelpDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Omega Help"));

    // Not Qt::Dialog's default modality, and said explicitly rather than left
    // to the constructor: this window's whole point is being readable while
    // the dialog it describes is open.
    setModal(false);

    // Big enough that the lists below do not reflow every other line. Resizable
    // rather than fixed -- the bodies are long, and a help window somebody
    // cannot make taller is a help window they scroll instead of reading.
    resize(720, 620);

    auto *layout = new QVBoxLayout(this);
    // Edge to edge, so the footer bar below spans the window the way
    // ModalFrame's does. The tab widget supplies its own page margins.
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);
    for (const HelpTopic &topic : helpTopics()) {
        auto *view = new QTextBrowser(tabs_);
        view->setOpenExternalLinks(true);
        // No stylesheet and no explicit palette: the theme walk sets both on
        // the window, and the bodies carry no colours of their own.
        view->setHtml(topic.html);
        // The id travels with the page, so showTopic() does not depend on the
        // tab order matching the list order.
        view->setProperty("helpTopicId", topic.id);
        tabs_->addTab(view, topic.title);
    }
    layout->addWidget(tabs_, 1);

    // NOT WRAPPED IN ModalFrame EITHER, for a different reason: this window is
    // modeless and deliberately resizable, and ModalFrame is built for the
    // opposite. fitToContent sizes a dialog to its content, and the frameless
    // path drops the window manager's resize edges -- see §8 of the pass
    // notes, where that is called out as deliberate because "a modal sizes
    // itself". A help window that cannot be made taller is one people scroll
    // instead of reading, which is the exact thing resize(720, 620) and the
    // comment above it exist to prevent.
    QPushButton *close = addFooterButton(layout, this, tr("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::close);
}

void HelpDialog::showTopic(const QString &topicId) {
    if (topicId.isEmpty()) {
        return;
    }
    for (int i = 0; i < tabs_->count(); ++i) {
        if (tabs_->widget(i)->property("helpTopicId").toString() == topicId) {
            tabs_->setCurrentIndex(i);
            return;
        }
    }
    // An id that matches nothing leaves the first tab showing. A help window
    // that opened blank because a menu item named a topic that has since been
    // renamed would be worse than one showing the wrong page.
}

void HelpDialog::open(QWidget *parent, const QString &topicId) {
    if (g_open.isNull()) {
        g_open = new HelpDialog(parent);
        // Parented for the theme walk and for lifetime, but deleted on close
        // so the next open builds fresh pages -- which is what will make a
        // topic that grows a live value (a path, a version) correct rather
        // than whatever it was when the window was first opened.
        g_open->setAttribute(Qt::WA_DeleteOnClose);
    }
    g_open->showTopic(topicId);
    g_open->show();
    g_open->raise();
    g_open->activateWindow();
}

}  // namespace omega::app