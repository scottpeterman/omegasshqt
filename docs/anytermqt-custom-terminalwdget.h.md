// include/qtpyte/terminalwidget.h
#pragma once

#include <QAbstractScrollArea>
#include <QByteArray>
#include <QColor>
#include <QElapsedTimer>
#include <QPoint>
#include <QFont>
#include <QString>

#include <memory>
#include <string>
#include <vector>

#include "pyte/screen.h"
#include "pyte/stream.h"
#include "qtpyte/keymap.h"
#include "qtpyte/palette.h"
#include "qtpyte/selection.h"

class QTimer;
class QMouseEvent;

namespace qtpyte {

// A terminal widget: bytes on one side, a pyte::Screen in the middle, a grid
// of glyphs on the other.
//
// The widget owns no pty, no shell, no SSH client and no serial port. It owns
// a screen. Bytes arrive through feed() and leave through dataReady(), and
// where they come from or go is the caller's business -- a local pseudo-
// terminal (see qtpyte::PtySession), an SSH channel, a socket, or a recorded
// stream in a test. Anything that can be pushed into a slot can drive it.
//
// A QAbstractScrollArea rather than a plain QWidget, so scrollback gets a real
// scrollbar and wheel handling for free instead of a hand-rolled imitation.
// Painting happens on viewport().
//
// The division of labour is the same one the emulator draws: pyte owns what
// the screen contains, this owns how it looks and what the keyboard sends.
// Nothing here parses an escape sequence, and nothing in pyte knows about Qt.
class TerminalWidget : public QAbstractScrollArea {
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget *parent = nullptr);
    ~TerminalWidget() override;

    // Send text as if typed: encoded and emitted through dataReady().
    // Pasted text goes through paste() so newline conversion and bracketed
    // paste are applied.
    void send(const QString &text);
    void paste(const QString &text);

    // The font decides the cell size and so the grid: setting it re-derives
    // rows and columns and tells the child.
    void setTerminalFont(const QFont &font);
    QFont terminalFont() const { return font_; }

    Palette &palette() { return palette_; }
    const Palette &palette() const { return palette_; }
    // Re-read the palette after changing it.
    void refresh();

    // The same palette by value, for callers that cannot hold a C++
    // reference into this object -- a language binding, principally.
    //
    // palette() hands out a reference and expects the caller to mutate it in
    // place and call refresh(). That contract does not survive a binding: a
    // generated wrapper copies a value-type on return, so the mutation lands
    // on a temporary, nothing changes, and nothing reports an error. These
    // two take a copy in and out instead, which is the same operation
    // expressed in a way that cannot silently do nothing.
    //
    // Named for the terminal rather than spelled setPalette, because
    // QWidget::setPalette already exists and takes a QPalette. Overloading
    // across that would be resolved by argument type in C++ and by whichever
    // wrapper the generator emitted first in Python. Same reason
    // setTerminalFont is not called setFont.
    // --- selection ---------------------------------------------------------
    //
    // Copying is never automatic. Selecting text puts nothing on the
    // clipboard until copySelection() is called, whether by the Ctrl+Shift+C
    // handler or by a host application's own menu item: a terminal that
    // rewrites the clipboard on every drag is a terminal that loses whatever
    // was there.
    // Where the viewport currently sits on the absolute line axis.
    //
    // Absolute lines are numbered from the oldest row the store still holds,
    // not from the top of the screen, so line 0 stops meaning "the first row
    // I wrote" as soon as anything scrolls off. A host holding its own
    // selection coordinates needs these two to convert; so does anything
    // testing one.
    int viewTopLine() const;
    int firstLine() const;
    int endLine() const;

    // The text of one absolute line, history or live screen alike, whatever
    // the viewport is pointed at. Empty for a line that has been trimmed.
    //
    // Note what firstLine() means before using it as a starting point: it is
    // the oldest row the store still holds, and a widget that was resized
    // after construction has pushed its initial blank screen into history
    // ahead of any output. The first line of output is not line zero and is
    // not firstLine() either.
    QString lineAt(int line) const;

    bool hasSelection() const;
    QString selectedText() const;
    void setSelection(int startLine, int startCol, int endLine, int endCol);
    void selectAll();
    void clearSelection();
    void copySelection();

    // The highlight, painted over the text rather than under it, so it works
    // whatever colours an application chose. Alpha is expected.
    void setSelectionColor(const QColor &color);
    QColor selectionColor() const { return selection_color_; }

    Palette terminalPalette() const { return palette_; }
    void setTerminalPalette(const Palette &p) {
        palette_ = p;
        refresh();
    }

    int columns() const { return cols_; }
    int terminalRows() const { return rows_; }
    // The grid pitch in pixels. A host sizing a window to a whole number of
    // cells needs these, and so does anything checking that text lands on the
    // grid rather than near it.
    int cellWidth() const { return cell_w_; }
    int cellHeight() const { return cell_h_; }
    int scrollbackSize() const { return screen_.history_size(); }
    void setScrollbackSize(int lines) { screen_.set_max_scrollback(lines); }

    // The limit itself, as opposed to how much is currently held. A setter
    // with no getter cannot be asserted by a host, which is the whole reason
    // this is here.
    int scrollbackLimit() const { return screen_.max_scrollback(); }

    // True while a full-screen application has switched to the alternate
    // screen -- vi, htop, a pager. A host that synthesises input on a timer
    // (anti-idle, keepalive-by-keystroke) has to know: bytes that are a no-op
    // at a shell prompt are not one in an editor, where a backspace deletes a
    // character and any key at a pager prompt advances or aborts the output.
    bool alternateScreen() const { return screen_.alternate_screen(); }

    // Everything on screen plus retained history, as text. This is what a
    // "save session" action wants; it is not a substitute for selection.
    QString sessionText() const;
    QString screenText() const;

public Q_SLOTS:
    // Push received bytes into the emulator and repaint. This is the whole
    // input surface: whatever is producing bytes calls this, from the thread
    // the widget lives on.
    void feed(const QByteArray &data);

Q_SIGNALS:
    // Bytes to send upstream: keystrokes, pasted text, and the replies the
    // emulator owes to Device Status Report and friends. Some applications
    // sit waiting for those answers, so a caller that drops this signal will
    // see them hang rather than fail.
    void dataReady(const QByteArray &data);

    // The grid changed shape, because the widget was resized or the font
    // changed. Whatever is on the other end of the bytes usually needs to be
    // told -- an application that does not repaint after a window resize is
    // normally missing this, not missing a redraw.
    void resized(int cols, int rows);

    // The child asked for a bell.
    void bell();

    // A selection appeared or went away -- for enabling a Copy menu item.
    void selectionChanged(bool hasSelection);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void changeEvent(QEvent *event) override;

private:
    void applyFontMetrics();
    void updateGeometryForSize();
    void syncScrollBar();
    void handleMode(int mode, bool set, bool private_mode);
    void paintRow(QPainter &painter, int y);
    void drawRun(QPainter &painter, const QString &text, int x, int y, int span);
    static bool isPlainAscii(const QString &text);
    void paintCursor(QPainter &painter);
    QRect cellRect(int x, int y, int span = 1) const;

    // Selection. Defined in selection.cpp.
    void paintSelection(QPainter &painter, int first, int last);
    bool handleSelectionKey(QKeyEvent *event);
    void pointToCell(const QPoint &pos, int &line, int &col) const;
    void extendFocusTo(const QPoint &pos);
    void updateAutoScroll(const QPoint &pos);
    void stopAutoScroll();
    void selectWordAt(int line, int col);
    void selectLineAt(int line);
    void notifySelectionChanged();

    // Whether the application on the far end has asked for mouse events.
    // Always false today -- the core does not implement DECSET 1000/1002/1006
    // yet. It exists so the mouse handlers ask the question now: when
    // reporting does land, an application gets the drag and Shift forces
    // selection, which is xterm's convention and what every full-screen
    // application expects.
    bool mouseReportingActive() const;

    pyte::Screen screen_;
    pyte::Stream stream_;

    Palette palette_;
    QFont font_;
    int cell_w_ = 8;
    int cell_h_ = 16;
    int baseline_ = 12;
    // False when the chosen font's glyphs do not all advance by the same
    // amount -- a proportional font, or a "monospace" one with exceptions.
    // Runs cannot be batched then; each cell is drawn at its own position.
    bool uniform_advance_ = true;
    int cols_ = 80;
    int rows_ = 24;

    Selection selection_;
    QColor selection_color_{80, 120, 200, 110};
    QTimer *autoscroll_ = nullptr;
    int autoscroll_dir_ = 0;
    QPoint last_drag_pos_;
    // Click run length for word and line selection. Qt delivers a double
    // click as its own event but has nothing for a triple, so the count is
    // kept here and reset by the interval.
    int click_count_ = 0;
    QElapsedTimer click_timer_;

    KeyModes key_modes_;
    bool cursor_visible_ = true;   // DECTCEM (?25)
    bool bracketed_paste_ = false; // ?2004
};

}  // namespace qtpyte