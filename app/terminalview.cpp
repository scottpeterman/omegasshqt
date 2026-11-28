// app/terminalview.cpp

#include "app/terminalview.h"

#include <QEvent>
#include <QKeyEvent>

namespace omega::app {

namespace {

// The same test handleSelectionKey makes, and no stricter. See the header:
// narrowing it leaves a gap the unconfirmed paste comes back through.
bool isCtrlShift(const QKeyEvent *event, int key) {
    const Qt::KeyboardModifiers mods = event->modifiers();
    return event->key() == key && mods.testFlag(Qt::ControlModifier) &&
           mods.testFlag(Qt::ShiftModifier);
}

}  // namespace

TerminalView::TerminalView(QWidget *parent) : qtpyte::TerminalWidget(parent) {}

int TerminalView::zoomStep(const QKeyEvent *event) {
    const Qt::KeyboardModifiers mods =
        event->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier);
    if (mods != Qt::ControlModifier) return 0;
    switch (event->key()) {
        case Qt::Key_Equal:
        case Qt::Key_Plus:
            return 1;
        case Qt::Key_Minus:
            return -1;
        default:
            return 0;
    }
}

bool TerminalView::focusNextPrevChild(bool next) {
    Q_UNUSED(next);
    // Never. Tab belongs to the far end here, not to the layout.
    return false;
}

bool TerminalView::event(QEvent *event) {
    if (event->type() == QEvent::ShortcutOverride) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (isCtrlShift(key, Qt::Key_C) || isCtrlShift(key, Qt::Key_V) ||
            zoomStep(key) != 0) {
            event->accept();
            return true;
        }
    }
    return qtpyte::TerminalWidget::event(event);
}

void TerminalView::keyPressEvent(QKeyEvent *event) {
    if (isCtrlShift(event, Qt::Key_V)) {
        event->accept();
        Q_EMIT pasteRequested();
        return;
    }

    // Always consumed. With a selection the widget's copySelection() runs;
    // without one, handleSelectionKey declines and the keymap would encode the
    // chord as Ctrl+C -- 0x03, an interrupt sent to the far end by a gesture
    // that meant "copy". A miss-timed copy must not abort a running command.
    if (isCtrlShift(event, Qt::Key_C)) {
        event->accept();
        if (hasSelection()) copySelection();
        return;
    }

    if (const int steps = zoomStep(event)) {
        event->accept();
        Q_EMIT zoomRequested(steps);
        return;
    }

    qtpyte::TerminalWidget::keyPressEvent(event);
}

}  // namespace omega::app
