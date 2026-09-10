// app/helpdialog.h
//
// In-application help: a window of topics, and the About text.
//
// EXTENDING IT IS ONE ENTRY. helpTopics() returns a list; a new topic is a new
// element in it, and the tab appears. Nothing else has to be touched -- not the
// menu, not the dialog, not a resource file. That is the whole reason the
// content is a table rather than a hand-built page per tab.
//
// MODELESS, NOT MODAL. Help about the session dialog is help you want ON SCREEN
// while the session dialog is open, and a modal one cannot be. It is a child of
// the main window so the theme walk reaches it and it closes with the
// application; open() raises the existing instance rather than stacking a
// second copy.
//
// The topic bodies are the one place in the shell that has to be kept honest by
// hand: they describe fields that live in sessioneditordialog.cpp and files that
// are named in settings.cpp, omegasettings.cpp and mainwindow.cpp, and nothing
// makes them follow along when those change. Everything they claim was read out
// of those files rather than remembered.
//
// NO COLOURS IN THE HTML. The bodies use structure only -- headings, lists,
// <code> -- so the theme's palette decides how they look. A body that set its
// own foreground would be black on black the first time somebody opened it
// under enterprise_dark.

#ifndef OMEGA_APP_HELPDIALOG_H
#define OMEGA_APP_HELPDIALOG_H

#include <QDialog>
#include <QString>
#include <QVector>

class QTabWidget;

namespace omega::app {

// One tab. id is what a menu item or a future context-sensitive Help button
// names to open the window on this page; it is not shown anywhere.
struct HelpTopic {
    QString id;
    QString title;
    QString html;
};

// The topics, in tab order.
const QVector<HelpTopic> &helpTopics();

// The About box body. Here rather than in mainwindow.cpp so the version, the
// licence and the repository are stated once.
//
// linkColor is the ONE colour that has to be passed in rather than left to the
// palette, and it is the exception that proves the rule at the top of this
// file. Qt renders an anchor in QPalette::Link, but a widget carrying a
// stylesheet resolves its palette from that stylesheet and ignores one set on
// the widget -- and the theme QSS says nothing about links, so the anchor falls
// back to Qt's default dark blue and is very nearly invisible on a dark
// background. Empty leaves the anchor unstyled.
QString aboutHtml(const QString &linkColor = QString());

// The About window: the project banner over the text above.
//
// A dialog rather than QMessageBox::about, because the banner is 640px wide and
// a message box will not lay one out sensibly -- it sizes itself around the
// text and then puts the pixmap where its icon goes. The image is a compiled-in
// resource (see app/omega.qrc), so it cannot go missing in a bundle.
class AboutDialog : public QDialog {
    Q_OBJECT

public:
    // linkColor is the theme's accent; see the note on aboutHtml().
    explicit AboutDialog(const QString &linkColor, QWidget *parent = nullptr);
};

// Registers app/omega.qrc.
//
// MUST BE CALLED BEFORE ANY :/omega/ PATH IS USED. omega_shell is a STATIC
// library, and a resource in one is not self-registering: the object rcc
// generates is referenced by nothing, so the linker drops it and every
// QPixmap(":/omega/...") silently comes back null. The failure is a blank label
// rather than an error, which is what makes it worth a named function instead
// of a comment.
//
// Safe to call more than once.
void initOmegaResources();

// The version this build reports, from the CMake project version -- never a
// literal in the source. A version that has to be edited in two places is a
// version that will disagree with itself.
QString omegaVersion();

class HelpDialog : public QDialog {
    Q_OBJECT

public:
    explicit HelpDialog(QWidget *parent = nullptr);

    // Opens the window on the named topic, creating it if it is not already
    // up and raising it if it is. An id that matches nothing opens the first
    // topic rather than an empty window.
    static void open(QWidget *parent, const QString &topicId = QString());

    void showTopic(const QString &topicId);

private:
    QTabWidget *tabs_ = nullptr;
};

}  // namespace omega::app

#endif  // OMEGA_APP_HELPDIALOG_H
