// examples/qt/theme_gallery.cpp
//
// Every control the stylesheet styles, rendered against a chosen theme, with a
// live terminal preview beside them.
//
// This is Phase 3's "look at it" artifact. generateStylesheet() is verified
// against the Python byte for byte by tests/compat/theme_differential.py, but
// that only proves the two agree -- it cannot tell you that a theme's border is
// invisible against its background, or that one of the 33 files has a
// foreground nobody can read. Those are seen, not asserted.
//
// It also exercises the two things the theme system has to get right before any
// real widget is written:
//
//   LIVE SWITCHING. The combo box changes theme with nothing restarting.
//
//   ONE applyTheme PATH. The walker below reaches the main window, every
//   terminal, every overlay AND the detached windows. nterm-qt's _apply_theme
//   walks its tab widget only -- it keeps a list of detached session windows
//   and never walks it, so a live theme switch leaves them on the old palette.
//   The Detach button here exists to make that failure visible if it is ever
//   reintroduced: open one, switch theme, and both windows must move.
//
// Usage:
//   theme_gallery [--themes <dir>] [--theme <name>]
//   theme_gallery [--themes <dir>] --shot <dir>    render every theme to PNG
//
// The --shot mode is how this runs with no display: it applies each theme,
// grabs the window and writes <dir>/<name>.png, which is reviewable in a batch
// rather than one screenshot at a time.

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenuBar>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QGridLayout>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVector>

#include <omegassh_theme_anytermqt.h>

#include "theme/stylesheet.h"
#include "theme/theme.h"

#include <cstdio>

using omega::theme::Theme;
using omega::theme::ThemeEngine;

namespace {

// A colour chart, written straight into the emulator. No pty, no shell: the
// widget takes bytes from anywhere, which is the whole reason a preview like
// this costs nothing.
QByteArray previewStream() {
    QByteArray b;
    b += "\x1b[2J\x1b[H";
    b += "omega@lab-core-01:~$ ls -la /etc/network\r\n";
    b += "\x1b[1;34mdrwxr-xr-x\x1b[0m  4 root root  4096 Aug 28 09:14 \x1b[1;34m.\x1b[0m\r\n";
    b += "-rw-r--r--  1 root root   612 Aug 28 09:14 interfaces\r\n\r\n";
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 8; ++col) {
            const int code = (row == 0 ? 30 : 90) + col;
            b += "\x1b[" + QByteArray::number(code) + ";1m  block  \x1b[0m";
        }
        b += "\r\n";
    }
    b += "\r\n\x1b[1mbold\x1b[0m  normal  \x1b[4munderline\x1b[0m  \x1b[7mreverse\x1b[0m\r\n";
    b += "omega@lab-core-01:~$ ";
    return b;
}

// A window that can be themed. The main window and every detached one are the
// same type, so the walker has one kind of thing to visit and cannot forget a
// second list.
class ThemedWindow : public QMainWindow {
public:
    explicit ThemedWindow(const QString &title, bool full, QWidget *parent = nullptr)
        : QMainWindow(parent) {
        setWindowTitle(title);
        build(full);
        // Read the widget's own selection alpha ONCE, before any theme lands.
        // Reading it after the first applyTheme would read back the alpha that
        // call wrote, and the translucency would be lost on the second switch.
        selectionAlpha_ = omega::theme::defaultSelectionAlpha(terminal_);
    }

    // Fed AFTER the window is laid out, not in the constructor. The widget
    // derives its grid from its size and its font, so a resize re-makes the
    // screen -- content written to a zero-sized terminal is gone by the time
    // anyone looks at it. Applying a theme changes the font, which resizes the
    // grid too, so this runs after every theme change rather than once.
    void fillPreview() {
        terminal_->feed(previewStream());
        // A selection, so the alpha workaround is visible rather than assumed:
        // an opaque selectionBackground would erase the glyphs under it.
        //
        // Anchored to viewTopLine(), not to line 1. Selection coordinates are
        // absolute on anytermqt's line axis, and a widget that has been resized
        // has already pushed its initial blank screen into history -- so line 1
        // is somewhere in the scrollback and the highlight lands where nobody
        // can see it. This is the same conversion a real tab will need.
        const int top = terminal_->viewTopLine();
        terminal_->setSelection(top + 1, 0, top + 1, 46);
    }

    void applyTheme(const Theme &theme) {
        setStyleSheet(QString::fromStdString(omega::theme::generateStylesheet(theme)));
        omega::theme::applyTheme(terminal_, theme, selectionAlpha_);
        terminal_->refresh();
        overlay_->setStyleSheet(omega::theme::overlayStylesheet(theme));
    }

    qtpyte::TerminalWidget *terminal() const { return terminal_; }
    QComboBox *themePicker() const { return picker_; }
    QPushButton *detachButton() const { return detach_; }

private:
    void build(bool full) {
        auto *splitter = new QSplitter(Qt::Horizontal, this);

        // --- left: tree ----------------------------------------------------
        auto *tree = new QTreeWidget;
        tree->setHeaderLabels({QStringLiteral("Sessions")});
        for (const char *folder : {"lab-core", "lab-edge"}) {
            auto *top = new QTreeWidgetItem(tree, {QString::fromLatin1(folder)});
            for (int i = 1; i <= 3; ++i) {
                new QTreeWidgetItem(top, {QStringLiteral("%1-sw%2")
                                              .arg(QString::fromLatin1(folder))
                                              .arg(i, 2, 10, QLatin1Char('0'))});
            }
            top->setExpanded(true);
        }
        tree->setCurrentItem(tree->topLevelItem(0)->child(1));
        splitter->addWidget(tree);

        // --- right: tabs ---------------------------------------------------
        auto *tabs = new QTabWidget;
        tabs->setTabsClosable(true);

        // Terminal tab. The terminal and the overlay share one grid cell,
        // which is how terminal_window.cpp stacks them. The overlay is added
        // with AlignCenter here rather than filling the cell, so a gallery
        // shot shows both the overlay chrome and the palette underneath it --
        // a real connection overlay covers the terminal, and covering it is
        // the one thing this example must not do.
        auto *termPage = new QWidget;
        auto *stack = new QGridLayout(termPage);
        stack->setContentsMargins(0, 0, 0, 0);
        terminal_ = new qtpyte::TerminalWidget;
        stack->addWidget(terminal_, 0, 0);
        overlay_ = new QLabel(QStringLiteral("Connecting..."));
        overlay_->setAlignment(Qt::AlignCenter);
        overlay_->setAttribute(Qt::WA_TransparentForMouseEvents);
        stack->addWidget(overlay_, 0, 0, Qt::AlignCenter);
        tabs->addTab(termPage, QStringLiteral("lab-core-sw01"));
        tabs->addTab(new QWidget, QStringLiteral("lab-edge-sw02"));
        tabs->addTab(new QWidget, QStringLiteral("lab-edge-sw03"));
        splitter->addWidget(tabs);
        splitter->setSizes({220, 780});

        // The controls sit BELOW the tree and tabs rather than in a tab of
        // their own, so one screenshot carries every styled class. A gallery
        // that needs two shots to show a theme is a gallery nobody compares.
        if (full) {
            auto *vertical = new QSplitter(Qt::Vertical, this);
            vertical->addWidget(splitter);
            vertical->addWidget(buildControls());
            vertical->setSizes({420, 220});
            setCentralWidget(vertical);
        } else {
            setCentralWidget(splitter);
        }

        // --- menus, for QMenuBar and QMenu ---------------------------------
        //
        // addAction() is spelled the long way on purpose. The convenient
        // overloads that take a shortcut and a functor together disagree about
        // argument order across the versions this has to build on: Qt 6.2
        // takes (text, slot, shortcut) and 6.4 takes (text, shortcut, slot),
        // with the 6.2 spelling deprecated in 6.4. Either one compiles on one
        // machine and fails on another. Creating the action and setting its
        // shortcut has meant the same thing since Qt 4.
        auto *file = menuBar()->addMenu(QStringLiteral("&File"));
        auto *quickConnect = file->addAction(QStringLiteral("Quick Connect"));
        quickConnect->setShortcut(QKeySequence(QStringLiteral("Ctrl+N")));
        file->addSeparator();
        auto *closeTab = file->addAction(QStringLiteral("Close Tab"));
        closeTab->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
        menuBar()->addMenu(QStringLiteral("&View"));
        menuBar()->addMenu(QStringLiteral("&Help"));

        resize(1040, 720);
    }

    QWidget *buildControls() {
        auto *page = new QWidget;
        auto *outer = new QVBoxLayout(page);

        auto *box = new QGroupBox(QStringLiteral("Connection"));
        auto *form = new QVBoxLayout(box);

        auto *row1 = new QHBoxLayout;
        row1->addWidget(new QLabel(QStringLiteral("Host")));
        auto *host = new QLineEdit(QStringLiteral("lab-core-sw01"));
        row1->addWidget(host);
        row1->addWidget(new QLabel(QStringLiteral("Port")));
        auto *port = new QSpinBox;
        port->setRange(1, 65535);
        port->setValue(22);
        row1->addWidget(port);
        form->addLayout(row1);

        auto *row2 = new QHBoxLayout;
        row2->addWidget(new QLabel(QStringLiteral("Transport")));
        auto *transport = new QComboBox;
        transport->addItems({QStringLiteral("SSH"), QStringLiteral("Telnet"),
                             QStringLiteral("Serial")});
        row2->addWidget(transport);
        auto *disabled = new QLineEdit(QStringLiteral("disabled field"));
        disabled->setEnabled(false);
        row2->addWidget(disabled);
        form->addLayout(row2);

        auto *row3 = new QHBoxLayout;
        row3->addWidget(new QCheckBox(QStringLiteral("Log session")));
        auto *checked = new QCheckBox(QStringLiteral("Legacy algorithms"));
        checked->setChecked(true);
        row3->addWidget(checked);
        row3->addStretch();
        form->addLayout(row3);
        outer->addWidget(box);

        auto *buttons = new QHBoxLayout;
        picker_ = new QComboBox;
        buttons->addWidget(new QLabel(QStringLiteral("Theme")));
        buttons->addWidget(picker_);
        buttons->addStretch();
        detach_ = new QPushButton(QStringLiteral("Detach"));
        buttons->addWidget(detach_);
        auto *cancel = new QPushButton(QStringLiteral("Cancel"));
        cancel->setEnabled(false);
        buttons->addWidget(cancel);
        auto *connectBtn = new QPushButton(QStringLiteral("Connect"));
        connectBtn->setDefault(true);
        connectBtn->setToolTip(QStringLiteral("Dial the selected session"));
        buttons->addWidget(connectBtn);
        outer->addLayout(buttons);
        outer->addStretch();
        return page;
    }

    qtpyte::TerminalWidget *terminal_ = nullptr;
    QLabel *overlay_ = nullptr;
    QComboBox *picker_ = nullptr;
    QPushButton *detach_ = nullptr;
    int selectionAlpha_ = 110;
};

QString argValue(const QStringList &args, const QString &flag,
                 const QString &fallback = {}) {
    const int at = args.indexOf(flag);
    return (at >= 0 && at + 1 < args.size()) ? args.at(at + 1) : fallback;
}

}  // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const QStringList args = app.arguments();

    const QString dir = argValue(args, QStringLiteral("--themes"),
                                 QStringLiteral(OMEGA_THEME_DIR_DEFAULT));
    ThemeEngine engine;
    const int loaded = engine.loadDirectory(dir.toStdString());
    for (const std::string &w : engine.warnings()) {
        std::fprintf(stderr, "warning: %s\n", w.c_str());
    }
    if (loaded == 0) {
        std::fprintf(stderr, "no themes loaded from %s\n", qPrintable(dir));
        return 1;
    }
    std::fprintf(stderr, "loaded %d themes from %s\n", loaded, qPrintable(dir));

    auto *window = new ThemedWindow(QStringLiteral("Omega - theme gallery"), true);

    // The one applyTheme path. Every window that exists gets the theme,
    // detached ones included -- one list, walked in one place.
    QVector<ThemedWindow *> windows{window};
    auto applyAll = [&windows, &engine](const QString &name) {
        const Theme *t = engine.get(name.toStdString());
        if (!t) return;
        for (ThemedWindow *w : windows) {
            w->applyTheme(*t);
            w->fillPreview();
        }
    };

    for (const std::string &name : engine.names()) {
        window->themePicker()->addItem(QString::fromStdString(name));
    }

    const QString shotDir = argValue(args, QStringLiteral("--shot"));
    if (!shotDir.isEmpty()) {
        QDir().mkpath(shotDir);
        window->show();
        // One event loop pass before the first grab, so the layout has settled
        // and the terminal has a cell size. Grabbing straight after show()
        // produces an image of a window that has not been laid out.
        QTimer::singleShot(0, [&] {
            int written = 0;
            for (const std::string &name : engine.names()) {
                applyAll(QString::fromStdString(name));
                window->themePicker()->setCurrentText(QString::fromStdString(name));
                QApplication::processEvents();
                const QString path =
                    shotDir + QLatin1Char('/') + QString::fromStdString(name) + QStringLiteral(".png");
                if (window->grab().save(path)) ++written;
            }
            std::fprintf(stderr, "wrote %d screenshots to %s\n", written,
                         qPrintable(shotDir));
            QApplication::quit();
        });
        return app.exec();
    }

    QObject::connect(window->themePicker(), &QComboBox::currentTextChanged,
                     [&applyAll](const QString &name) { applyAll(name); });

    QObject::connect(window->detachButton(), &QPushButton::clicked, [&] {
        auto *detached = new ThemedWindow(QStringLiteral("Omega - detached"), false);
        windows.append(detached);
        // A detached window joins the walker's list before it is themed, so it
        // is never a window the next switch does not know about.
        applyAll(window->themePicker()->currentText());
        detached->resize(700, 420);
        detached->show();
    });

    const QString start = argValue(args, QStringLiteral("--theme"),
                                   QStringLiteral("default"));
    window->themePicker()->setCurrentText(start);
    applyAll(start);
    window->show();
    return app.exec();
}
