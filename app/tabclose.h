// app/tabclose.h
//
// What a multi-tab close is about to take, and the words for asking about it.
//
// Separate from MainWindow, and QtCore only, for the same reason antiidle is:
// the decision -- does this close need a question, and what does the question
// say -- is a pure function of the set, so a probe can check every branch of it
// with no display, no tab widget and no modal to answer. What is left in the
// window is the QMessageBox itself.
//
// WHY ONLY MULTI-TAB CLOSES ASK. Closing one tab has always been one click for
// one session, from the tab bar's own close button and from Ctrl+W, and a
// confirmation on that would be a confirmation on the common case. The risk
// being guarded is different in kind: Close Other Tabs and Close All take
// sessions that are not the one being looked at, so the count is not obvious
// from the gesture. Those routes ask; single close stays as it was.

#ifndef OMEGA_APP_TABCLOSE_H
#define OMEGA_APP_TABCLOSE_H

#include <QString>
#include <QStringList>

namespace omega::app {

// The tabs a close is about to take. liveTitles holds the ones with a session
// still running, which is the only thing that turns a close into a question --
// a set of five tabs whose sessions have all ended is five pieces of
// scrollback, and taking those without asking is what the close button is for.
struct TabCloseSet {
    int total = 0;
    QStringList liveTitles;

    bool needsConfirmation() const { return !liveTitles.isEmpty(); }
};

// "Close 3 tabs?" -- the question itself, with no detail in it.
QString tabCloseQuestion(const TabCloseSet &set);

// The live sessions, named. Empty when there are none, which is the case the
// caller answers by not asking at all.
//
// The list is capped: a person closing thirty tabs needs to know it is thirty
// and roughly which, not to read all of them in a message box that has grown
// taller than the window behind it.
QString tabCloseDetail(const TabCloseSet &set);

// How many names tabCloseDetail lists before it summarises the rest.
constexpr int kTabCloseMaxListed = 8;

}  // namespace omega::app

#endif  // OMEGA_APP_TABCLOSE_H
