// app/livesessions.h
//
// What the tabs know, keyed by what the tree shows.
//
// THE PROBLEM THIS SOLVES. SessionTreeModel is backed by SessionStore, which is
// a SQLite file and knows nothing about connections. Live state lives in
// TerminalTab and OmegaSshSession. Neither can reasonably reach the other: a
// model that walked the tab widget would be a model that depends on a window,
// and a tab that wrote into the store would be writing per-run state into a
// file two applications share. So the state sits here, the tabs write it, the
// model reads it, and the only coupling either side has is to this class.
//
// KEYED BY SESSION ID, not by tab. Two tabs can be open on one stored session
// -- opening the same device twice is ordinary -- and the tree has one row for
// it either way. The row shows the LIVELIEST of them, which is the answer that
// makes "is this device connected" true rather than "is this particular tab
// connected". A quick-connect tab has no stored session and no id, so it never
// reaches here at all; nothing in the tree corresponds to it.
//
// The registry never outlives the window and holds no ownership of anything.
// Entries are removed when the last tab on an id closes, which is what makes
// a row go back to its stored subtitle rather than keeping "disconnected"
// forever.

#ifndef OMEGA_APP_LIVESESSIONS_H
#define OMEGA_APP_LIVESESSIONS_H

#include <QHash>
#include <QObject>
#include <QString>

#include "app/linkstate.h"

namespace omega::app {

class LiveSessionRegistry : public QObject {
    Q_OBJECT

public:
    explicit LiveSessionRegistry(QObject *parent = nullptr);

    struct Entry {
        Link link = Link::Idle;

        // Milliseconds, or -1 for "not known". NOTHING SETS THIS YET.
        // sshcore/dial.go arms TCP keepalive and no application-level ping, so
        // there is no round trip to measure; getting one means a
        // keepalive@openssh.com global request in sshcore and an event out
        // through capi. The delegate reserves no width when it is -1, so the
        // rows are correct rather than empty-looking in the meantime.
        int rttMs = -1;

        // The subtitle, rendered by whoever knows why -- "key passphrase
        // needed" and "lab-admin@10.20.4.11:22" are different kinds of thing
        // and the delegate must not be the one deciding which it is looking
        // at. Empty means "no live opinion", and the model falls back to the
        // stored session's address.
        QString detail;

        // One link per open tab, keyed by the tab itself. The Entry::link
        // above is the resolved answer; this is what it is resolved from.
        //
        // Per tab rather than one value, because two tabs on one device
        // genuinely disagree: one can be Live while the other is Dead, and
        // last-writer-wins would make the row report whichever transitioned
        // most recently. See rank().
        QHash<const QObject *, Link> perTab;
    };

    // Called when a tab opens on a stored session, and when it closes. The tab
    // is the key, not a count, so two tabs on one id can hold different states.
    void attach(qint64 sessionId, const QObject *tab);
    void detach(qint64 sessionId, const QObject *tab);

    void setLink(qint64 sessionId, const QObject *tab, Link link);
    void setRtt(qint64 sessionId, int ms);
    void setDetail(qint64 sessionId, const QString &detail);

    bool has(qint64 sessionId) const { return entries_.contains(sessionId); }
    Entry entry(qint64 sessionId) const {
        return entries_.value(sessionId, Entry{});
    }

signals:
    // One row changed. The model turns this into a dataChanged for that row
    // only -- a full reset on every state transition would collapse the tree
    // under the user four times per connect.
    void changed(qint64 sessionId);

private:
    // Ranks the states so two tabs on one id resolve to the liveliest. Live
    // beats NeedsInput beats the in-flight ones beats Dead: the question a
    // tree row answers is "can I reach this device", and one working session
    // answers yes however the other one is doing.
    static int rank(Link link);
    static Link resolve(const QHash<const QObject *, Link> &perTab);

    QHash<qint64, Entry> entries_;
};

}  // namespace omega::app

#endif  // OMEGA_APP_LIVESESSIONS_H
