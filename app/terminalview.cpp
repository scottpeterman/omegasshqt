// app/terminalview.cpp

#include "app/terminalview.h"

#include <QKeyEvent>

namespace omega::app {

TerminalView::TerminalView(QWidget *parent) : qtpyte::TerminalWidget(parent) {}

bool TerminalView::focusNextPrevChild(bool next) {
    Q_UNUSED(next);
    // Never. Tab belongs to the far end here, not to the layout.
    return false;
}

void TerminalView::keyPressEvent(QKeyEvent *event) {
    // Deliberately the same test handleSelectionKey makes, and no stricter.
    // See the header: narrowing it leaves a gap the unconfirmed paste comes
    // back through.
    const Qt::KeyboardModifiers mods = event->modifiers();
    if (event->key() == Qt::Key_V && mods.testFlag(Qt::ControlModifier) &&
        mods.testFlag(Qt::ShiftModifier)) {
        event->accept();
        Q_EMIT pasteRequested();
        return;
    }

    qtpyte::TerminalWidget::keyPressEvent(event);
}

}  // namespace omega::app