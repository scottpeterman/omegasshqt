// app/statusdot.h
//
// The state dot as a QIcon, for the tab bar.
//
// QTabBar takes an icon per tab and nothing else; there is no delegate and no
// paint hook short of subclassing it. So the dot is rendered once per state
// into a pixmap and handed to every tab that needs it, rather than painted per
// tab on every linkChanged -- eight tabs cycling through four states during a
// batch connect is otherwise thirty-two rasterisations of a seven-pixel
// circle.
//
// The cache is keyed by state alone and therefore has to be dropped when the
// theme changes, because the colour comes from the tokens. clearDotCache() is
// called from MainWindow::applyTheme on the same walk that themes everything
// else; forgetting it leaves the previous theme's dots on the tabs, which is
// the sort of thing that survives review because the tabs still look fine on
// their own.
//
// Device pixel ratio is baked in at render time. A pixmap made at 1x and shown
// on a 200% display is a blurred circle, and a dot is small enough that the
// blur is most of it.

#ifndef OMEGA_APP_STATUSDOT_H
#define OMEGA_APP_STATUSDOT_H

#include <QIcon>

#include "app/linkstate.h"
#include "theme/tokens.h"

namespace omega::app {

QIcon linkDotIcon(Link link, const theme::Tokens &tokens);
void clearDotCache();

}  // namespace omega::app

#endif  // OMEGA_APP_STATUSDOT_H
