// app/livesessions.cpp

#include "app/livesessions.h"

namespace omega::app {

LiveSessionRegistry::LiveSessionRegistry(QObject *parent) : QObject(parent) {}

int LiveSessionRegistry::rank(Link link) {
    switch (link) {
        case Link::Live:           return 6;
        case Link::NeedsInput:     return 5;
        case Link::Authenticating: return 4;
        case Link::Connecting:     return 3;
        case Link::Resolving:      return 3;
        case Link::Reconnecting:   return 2;
        case Link::Dead:           return 1;
        case Link::Idle:           break;
    }
    return 0;
}

// The row's answer, from however many tabs are open on it.
Link LiveSessionRegistry::resolve(const QHash<const QObject *, Link> &perTab) {
    Link best = Link::Idle;
    int bestRank = -1;
    for (auto it = perTab.constBegin(); it != perTab.constEnd(); ++it) {
        const int r = rank(it.value());
        if (r > bestRank) {
            bestRank = r;
            best = it.value();
        }
    }
    return best;
}

void LiveSessionRegistry::attach(qint64 sessionId, const QObject *tab) {
    if (sessionId <= 0) return;  // quick connect: no stored row to annotate
    Entry &e = entries_[sessionId];
    e.perTab.insert(tab, Link::Connecting);
    e.link = resolve(e.perTab);
    emit changed(sessionId);
}

void LiveSessionRegistry::detach(qint64 sessionId, const QObject *tab) {
    if (sessionId <= 0) return;
    const auto it = entries_.find(sessionId);
    if (it == entries_.end()) return;
    it->perTab.remove(tab);
    if (!it->perTab.isEmpty()) {
        // Another tab is still open on this session, so what is left behind is
        // the surviving tab's state and not the departing one's.
        it->link = resolve(it->perTab);
        emit changed(sessionId);
        return;
    }
    entries_.erase(it);
    // Emitted AFTER the erase, so the model reads the absence rather than the
    // entry that is about to go. The row falls back to its stored subtitle,
    // which is what makes a closed tab leave no residue in the tree.
    emit changed(sessionId);
}

void LiveSessionRegistry::setLink(qint64 sessionId, const QObject *tab,
                                  Link link) {
    if (sessionId <= 0) return;
    const auto it = entries_.find(sessionId);
    if (it == entries_.end()) return;  // no tab: nothing to report about
    it->perTab.insert(tab, link);
    const Link resolved = resolve(it->perTab);
    if (it->link == resolved) return;
    it->link = resolved;
    emit changed(sessionId);
}

void LiveSessionRegistry::setRtt(qint64 sessionId, int ms) {
    if (sessionId <= 0) return;
    const auto it = entries_.find(sessionId);
    if (it == entries_.end()) return;
    const int clamped = ms < 0 ? -1 : ms;
    if (it->rttMs == clamped) return;
    it->rttMs = clamped;
    emit changed(sessionId);
}

void LiveSessionRegistry::setDetail(qint64 sessionId, const QString &detail) {
    if (sessionId <= 0) return;
    const auto it = entries_.find(sessionId);
    if (it == entries_.end()) return;
    if (it->detail == detail) return;
    it->detail = detail;
    emit changed(sessionId);
}

}  // namespace omega::app
