// app/sessionribbon.cpp

#include "app/sessionribbon.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QStyle>

namespace omega::app {
namespace {

// A 1px vertical rule between groups. A QFrame with role="divider", so the
// generated sheet colours it and a theme change reaches it for free.
QFrame *divider(QWidget *parent) {
    auto *d = new QFrame(parent);
    d->setProperty("role", "divider");
    d->setFixedSize(1, 14);
    return d;
}

QLabel *monoLabel(QWidget *parent, const char *tone = nullptr) {
    auto *l = new QLabel(parent);
    l->setProperty("mono", true);
    if (tone) l->setProperty("tone", QString::fromLatin1(tone));
    return l;
}

QString transportWord(omegassh::Transport t) {
    switch (t) {
        case omegassh::Transport::Telnet: return QStringLiteral("telnet");
        case omegassh::Transport::Serial: return QStringLiteral("serial");
        case omegassh::Transport::Ssh:    break;
    }
    return QStringLiteral("ssh");
}

}  // namespace

SessionRibbon::SessionRibbon(QWidget *parent) : QFrame(parent) {
    setProperty("role", "ribbon");
    setFixedHeight(38);

    auto *lay = new QHBoxLayout(this);
    // The left inset clears the dot this widget paints itself.
    lay->setContentsMargins(30, 0, 12, 0);
    lay->setSpacing(10);

    name_ = new QLabel(this);
    name_->setProperty("role", "ribbonname");
    lay->addWidget(name_);

    lay->addWidget(divider(this));

    target_ = monoLabel(this, "ink");
    lay->addWidget(target_);

    lay->addWidget(divider(this));

    credential_ = monoLabel(this);
    lay->addWidget(credential_);

    transport_ = new QLabel(this);
    transport_->setProperty("chip", "true");
    lay->addWidget(transport_);

    lay->addStretch();

    state_ = monoLabel(this, "state");
    lay->addWidget(state_);

    reconnect_ = new QPushButton(tr("Reconnect"), this);
    reconnect_->setVisible(false);
    connect(reconnect_, &QPushButton::clicked, this,
            &SessionRibbon::reconnectRequested);
    lay->addWidget(reconnect_);
}

void SessionRibbon::setSession(const QString &displayName,
                               const omegassh::Config &config) {
    name_->setText(displayName);

    if (config.transport == omegassh::Transport::Serial) {
        // A serial line has no host and no port; showing "…:22" for one would
        // be a lie in the field most likely to be read.
        target_->setText(config.serialPort);
    } else {
        target_->setText(QStringLiteral("%1:%2").arg(config.host).arg(config.port));
    }

    // The credential as dialled. Not the negotiated key type -- nothing
    // publishes one; see the header.
    QString who = config.username;
    if (who.isEmpty() && !config.credential.isEmpty()) who = config.credential;
    credential_->setText(who);
    credential_->setVisible(!who.isEmpty());

    // Only when it is worth saying. Every other session in the window is ssh,
    // and a chip that is always there stops being read.
    const bool isSsh = config.transport == omegassh::Transport::Ssh;
    transport_->setText(transportWord(config.transport));
    transport_->setVisible(!isSsh);

    relayout();
}

void SessionRibbon::setLink(Link link) {
    if (link_ == link) return;
    link_ = link;
    state_->setText(linkWord(link));

    // Three tones, not two. The first cut used the state colour for
    // everything that was not NeedsInput, which painted "disconnected" in the
    // same colour as "live" -- a dead session announcing itself in the colour
    // reserved for a working one. Idle and Dead are muted; they are the
    // absence of a connection, not a kind of one.
    const char *tone = "state";
    if (linkIsWarn(link)) {
        tone = "warn";
    } else if (link == Link::Idle || link == Link::Dead) {
        tone = "muted";
    }
    state_->setProperty("tone", QString::fromLatin1(tone));
    state_->style()->unpolish(state_);
    state_->style()->polish(state_);
    update();  // the dot
}

void SessionRibbon::setReconnectVisible(bool visible) {
    reconnect_->setVisible(visible);
}

void SessionRibbon::setTokens(const theme::Tokens &tokens) {
    tokens_ = tokens;
    update();
}

void SessionRibbon::relayout() {
    // Hiding a label leaves its divider behind. Walking the layout is cheaper
    // than holding a pointer to each rule and remembering which pairs with
    // which.
    QLayout *lay = layout();
    QWidget *previousVisible = nullptr;
    for (int i = 0; i < lay->count(); ++i) {
        QWidget *w = lay->itemAt(i)->widget();
        if (!w) continue;
        if (w->property("role").toString() == QLatin1String("divider")) {
            w->setVisible(previousVisible != nullptr);
            if (previousVisible) previousVisible = nullptr;
            continue;
        }
        if (w->isVisibleTo(this)) previousVisible = w;
    }
}

void SessionRibbon::paintEvent(QPaintEvent *event) {
    QFrame::paintEvent(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(dotColor(link_, tokens_));
    p.drawEllipse(QRect(13, height() / 2 - 3, 7, 7));
}

}  // namespace omega::app
