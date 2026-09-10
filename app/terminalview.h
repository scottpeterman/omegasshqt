// app/terminalview.h
//
// The anytermqt terminal widget with two of Qt's and one of anytermqt's own
// key interceptions taken off, both for the same reason: in this application
// those keys belong somewhere other than where the widget sends them.
//
// --- Tab -------------------------------------------------------------------
//
// QWidget::event() intercepts Key_Tab and Key_Backtab and hands them to
// focusNextPrevChild() before keyPressEvent() is ever called. In a window with
// any other focusable widget in it -- the session tree, a filter box, the tab
// bar -- Tab therefore moves focus out of the terminal and the far end
// receives nothing at all. The widget looks like it stopped responding; what
// actually happened is that it stopped being focused.
//
// The emulator's keymap already does the right thing with both keys once they
// arrive: Tab sends HT, Shift+Tab sends CSI Z. Nothing needs adding, only the
// interception needs removing.
//
// This is the same fix nterm-qt carries as _NativeTerminal in
// nterm/terminal/widget.py, for the same reason and at the same layer: in the
// application that hosts the widget, not in anytermqt. A terminal is the one
// widget where Tab belongs to the far end rather than to the layout, but
// anytermqt is a general-purpose widget, and a host that wants Tab to navigate
// -- a form with a small terminal in one corner -- is entitled to it.
//
// Ctrl+Tab is unaffected: QWidget::event() only consults the focus chain when
// neither Control nor Alt is held, so tab switching keeps working.
//
// --- Ctrl+Shift+V ----------------------------------------------------------
//
// TerminalWidget::handleSelectionKey (anytermqt qtpyte/src/selection.cpp)
// answers Ctrl+Shift+V by reading the clipboard and calling its own paste()
// straight away. For a general-purpose widget that is the correct default and
// the only thing it could do. In Omega it is wrong, and quietly so: it skips
// TerminalTab::paste() entirely, and with it the multi-line threshold, the
// confirmation dialog, and the baud pacing. Thirty lines of configuration go
// to a console port at full rate with nothing asked -- which is the exact
// event the confirmation exists to prevent, reachable by the shortcut most
// likely to be used for it.
//
// The context menu's Paste went through TerminalTab; the keyboard did not.
// Only the menu path was ever tested, so the gap was invisible.
//
// So the keystroke is caught here and turned into a request the tab answers.
// The widget's own paste() is untouched and still does the sending -- this
// changes who decides, not how the bytes go out.
//
// THE CONDITION MUST BE A SUPERSET of handleSelectionKey's, not a tightening
// of it. That function asks only whether Control and Shift are both held; it
// does not care what else is. Testing for "Control and Shift and nothing
// else" here would be more precise and would leave Alt+Ctrl+Shift+V falling
// through to the widget, pasting unconfirmed -- the same bug through a
// narrower gap. Matching the condition exactly is what closes it.

#ifndef OMEGA_APP_TERMINALVIEW_H
#define OMEGA_APP_TERMINALVIEW_H

#include <qtpyte/terminalwidget.h>

namespace omega::app {

class TerminalView : public qtpyte::TerminalWidget {
    Q_OBJECT

public:
    explicit TerminalView(QWidget *parent = nullptr);

Q_SIGNALS:
    // Ctrl+Shift+V was pressed. The host is expected to route this to whatever
    // its own paste path is; nothing has been read from the clipboard or sent
    // to the far end yet.
    void pasteRequested();

protected:
    // Refusing the move is what makes QWidget::event() fall through to
    // keyPressEvent(), which is where the keymap lives.
    bool focusNextPrevChild(bool next) override;

    // Where Ctrl+Shift+V is taken before the base class can act on it.
    void keyPressEvent(QKeyEvent *event) override;
};

}  // namespace omega::app

#endif  // OMEGA_APP_TERMINALVIEW_H