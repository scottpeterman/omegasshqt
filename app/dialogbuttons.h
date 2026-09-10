// app/dialogbuttons.h
//
// One place that decides what Return does in a dialog.
//
// Qt has two separate mechanisms for it and they disagree. QDialog's own
// keyPressEvent looks for the button whose isDefault() is set and clicks that
// one -- but it only ever sees the key if the focused widget lets it through.
// QPushButton::keyPressEvent gets there first, and a button with autoDefault
// set clicks ITSELF on Return regardless of which button is the default. In a
// QDialogButtonBox every button has autoDefault on by default, so Return with
// the focus on Cancel is Cancel, whatever the accept button says about itself.
//
// The second trap is quieter. QPushButton::setDefault() registers the button
// with its dialog by walking window() -- and a QDialogButtonBox constructed
// with no parent IS its own window until the layout adopts it. Calling
// setDefault() on a button in a box that has not been added to a layout yet
// therefore sets the flag and registers nothing, which leaves the answer to
// QDialogButtonBox's show-time fallback and the platform style. That is the
// shape of a bug that behaves on one OS and not another.
//
// So: add the box to the layout FIRST, then call this. It turns autoDefault
// off on everything, then turns it and default on for the one button Return
// should mean -- which makes the answer the same on all three platforms and
// independent of where the focus happens to be sitting.
//
// pasteconfirmdialog.cpp and hostkeypromptdialog.cpp use it the other way
// round, naming the SAFE button rather than the accept one. That is the point
// of passing the button rather than assuming AcceptRole.
//
// THE LOOSE-BUTTON OVERLOAD is for ModalFrame, whose footer is a QHBoxLayout
// rather than a QDialogButtonBox -- the redesign's footer carries a hint
// label at one end and the buttons at the other, which is not a shape
// QDialogButtonBox lays out. Nothing about the trap above changes: a bare
// QPushButton in a layout still constructs with autoDefault ON, so it still
// answers Return itself. Dropping the box does not drop the problem, and
// dropping this call along with the box is how it would come back.
//
// PRIMARY AND DEFAULT ARE THE SAME BUTTON, always. The accent fill says "this
// is what Return does and what this dialog recommends"; it does not say "this
// is the affirmative". Where those differ -- the host key prompt, where Enter
// must not accept an unverified key, and the paste confirmation, where Enter
// must not paste forty lines into a console port -- the accent goes on the
// SAFE button and the affirmative one is an ordinary button beside it. The
// failure this rules out is a dialog whose loudest control is not the one the
// keyboard activates. ModalFrame::addButton enforces it by doing both in one
// call; anything hand-rolling a footer has to do it here.

#ifndef OMEGA_APP_DIALOGBUTTONS_H
#define OMEGA_APP_DIALOGBUTTONS_H

#include <QList>

class QAbstractButton;
class QDialogButtonBox;
class QPushButton;

namespace omega::app {

// Makes `activated` the button Return presses, and stops every other button
// in the box from answering Return on its own. Call after the box has been
// added to the dialog's layout; a null `activated` clears autoDefault on
// everything and sets no default, which is a dialog Return does nothing in.
void setReturnActivates(QDialogButtonBox *buttons, QAbstractButton *activated);

// The same, over a set of buttons that are not in a QDialogButtonBox. Every
// button in `buttons` is cleared, `activated` is then set -- so `activated`
// may be a member of the list and usually is. Call after the buttons are in
// the dialog's layout, for the window()-walk reason above.
//
// Re-call it whenever the set changes. A button added after the last call
// comes in with autoDefault on and will answer Return itself.
void setReturnActivates(const QList<QPushButton *> &buttons,
                        QAbstractButton *activated);

}  // namespace omega::app

#endif  // OMEGA_APP_DIALOGBUTTONS_H