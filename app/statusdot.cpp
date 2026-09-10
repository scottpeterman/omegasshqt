// app/statusdot.cpp

#include "app/statusdot.h"

#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QPixmap>

namespace omega::app {
namespace {

QHash<int, QIcon> &cache() {
    static QHash<int, QIcon> c;
    return c;
}

}  // namespace

QIcon linkDotIcon(Link link, const theme::Tokens &tokens) {
    const int key = static_cast<int>(link);
    const auto it = cache().constFind(key);
    if (it != cache().constEnd()) return it.value();

    const qreal dpr = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
    QPixmap pm(QSize(10, 10) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(dotColor(link, tokens));
    p.drawEllipse(QRectF(1.5, 1.5, 7, 7));
    p.end();

    const QIcon icon(pm);
    cache().insert(key, icon);
    return icon;
}

void clearDotCache() { cache().clear(); }

}  // namespace omega::app
