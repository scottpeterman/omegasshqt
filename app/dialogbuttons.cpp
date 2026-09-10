// app/dialogbuttons.cpp

#include "app/dialogbuttons.h"

#include <QDialogButtonBox>
#include <QPushButton>

namespace omega::app {
namespace {

// Cleared on EVERY button first, the chosen one included, so the order the
// caller happens to hold them in cannot matter.
void clearAll(const QList<QPushButton *> &buttons) {
    for (QPushButton *button : buttons) {
        if (!button) continue;
        button->setAutoDefault(false);
        button->setDefault(false);
    }
}

void activate(QAbstractButton *activated) {
    if (auto *pb = qobject_cast<QPushButton *>(activated)) {
        pb->setAutoDefault(true);
        pb->setDefault(true);
    }
}

}  // namespace

void setReturnActivates(QDialogButtonBox *buttons, QAbstractButton *activated) {
    if (!buttons) return;

    QList<QPushButton *> pushButtons;
    const QList<QAbstractButton *> all = buttons->buttons();
    pushButtons.reserve(all.size());
    for (QAbstractButton *b : all) {
        if (auto *pb = qobject_cast<QPushButton *>(b)) pushButtons.append(pb);
    }

    clearAll(pushButtons);
    activate(activated);
}

void setReturnActivates(const QList<QPushButton *> &buttons,
                        QAbstractButton *activated) {
    clearAll(buttons);
    activate(activated);
}

}  // namespace omega::app