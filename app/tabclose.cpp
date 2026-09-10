// app/tabclose.cpp

#include "app/tabclose.h"

#include <QCoreApplication>
#include <QObject>

namespace omega::app {
namespace {

// ASCII, deliberately. The same reason the spinner in terminaltab.cpp builds
// its braille from code points: a bullet written literally puts a non-ASCII
// byte in the source, and MSVC reads that in the machine's ANSI codepage
// unless /utf-8 is on.
const QString kBullet = QStringLiteral("  - ");

}  // namespace

QString tabCloseQuestion(const TabCloseSet &set) {
    if (set.total <= 1) {
        return QObject::tr("Close this tab?");
    }
    // %1 rather than Qt's %n plural form: "(s)" only resolves through an
    // installed translator, and with none the literal parenthesis renders --
    // the same note pasteconfirm carries.
    return QObject::tr("Close %1 tabs?").arg(set.total);
}

QString tabCloseDetail(const TabCloseSet &set) {
    if (set.liveTitles.isEmpty()) {
        return QString();
    }

    QStringList shown;
    for (const QString &title : set.liveTitles) {
        if (shown.size() == kTabCloseMaxListed) {
            break;
        }
        shown << kBullet + title;
    }

    const int hidden = static_cast<int>(set.liveTitles.size()) - shown.size();
    if (hidden > 0) {
        shown << kBullet + QObject::tr("and %1 more").arg(hidden);
    }

    const QString list = shown.join(QLatin1Char('\n'));

    if (set.liveTitles.size() == 1) {
        return QObject::tr("One session is still connected and will be "
                           "disconnected:\n\n%1")
            .arg(list);
    }
    return QObject::tr("%1 sessions are still connected and will be "
                       "disconnected:\n\n%2")
        .arg(static_cast<int>(set.liveTitles.size()))
        .arg(list);
}

}  // namespace omega::app
