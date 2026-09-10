// app/currenttokens.h
//
// The token set the application is currently painted with.
//
// WHY THIS EXISTS AND WHY IT IS A GLOBAL. MainWindow::applyTheme already
// derives one Tokens per theme change and hands it to every painted widget it
// can reach: the chrome bar, the delegate, the size grip, the tab closers.
// That walk works because the window owns all of them. It does not reach a
// modal dialog, which is constructed later, by a tab, and destroyed when it
// closes -- adding it to windows_ would mean the window holding a pointer to
// something whose lifetime is a stack frame in TerminalTab.
//
// The alternative is threading Tokens through ten dialog constructors and
// their call sites, for the sake of the two or three widgets in a dialog that
// paint rather than being styled. That is a lot of signature churn to carry a
// value that is, in fact, process-wide: there is one theme, and every window
// is painted with it.
//
// So: MainWindow::applyTheme publishes, and anything that paints reads. The
// value is only ever written from applyTheme, on the GUI thread, which is the
// only thread that may touch a QWidget anyway.
//
// Before the first theme lands this is specTokens(), so a widget constructed
// early paints something sensible rather than black on black -- the same
// default WindowButton and ChromeBar already carry.
//
// NOT A SUBSTITUTE FOR setTokens(). A widget that lives as long as the window
// should still be handed its tokens on the applyTheme walk, because that walk
// is also what repaints it. This is for the short-lived case, where the widget
// did not exist when the theme was applied -- and even there, a dialog that
// can outlive a theme change (SettingsDialog previews live) re-reads on
// QEvent::StyleChange. See ModalFrame::eventFilter.

#ifndef OMEGA_APP_CURRENTTOKENS_H
#define OMEGA_APP_CURRENTTOKENS_H

#include "theme/tokens.h"

namespace omega::app {

// The live set. Valid before any theme is applied; see the file comment.
const theme::Tokens &currentTokens();

// Called from MainWindow::applyTheme, and nowhere else.
void setCurrentTokens(const theme::Tokens &tokens);

}  // namespace omega::app

#endif  // OMEGA_APP_CURRENTTOKENS_H