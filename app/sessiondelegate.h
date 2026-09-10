// app/sessiondelegate.h
//
// The one place in the shell that paints instead of styling.
//
// A tree row here is two lines of different weight and family, a state dot, and
// an optional chip -- none of which a QSS rule can express, because a stylesheet
// styles a row and this row has internal structure. Everything else in the
// window is still a selector.
//
// KIND IS NOT WHAT THE PREVIEW HARNESS SAID. That harness declared
// `enum Kind { Session = 0, Group = 1 }` and its header claimed the roles
// matched SessionTreeModel's "in its order". The roles do; the enum does not --
// the model is `enum Kind { FolderItem, SessionItem }`, so a folder is 0 and a
// session is 1, exactly inverted. Dropping the harness's delegate in unchanged
// paints every session as a group header and every folder as a session row.
// This one asks the model's enum by name and never compares against an integer.
//
// THE DELEGATE DRAWS THE BRANCH INDICATOR, so the view runs at indentation 0
// with setRootIsDecorated(false). That is not a style choice: a two-line row
// indented by the view's own margin loses the width the detail line needs, and
// the spec's rail is already narrow enough that the address elides. Since the
// view then draws no expander, the pane toggles folders on single click.
//
// GEOMETRY COMES FROM FONT METRICS, not from the spec's literal 44px. The
// literal is what 13px plus 10.5px mono plus padding happens to come to at
// 100%; deriving it means a 200% display or a larger UI font moves the whole
// row together instead of clipping the second line.

#ifndef OMEGA_APP_SESSIONDELEGATE_H
#define OMEGA_APP_SESSIONDELEGATE_H

#include <QStyledItemDelegate>

#include "theme/tokens.h"

class QTreeView;
class QVariantAnimation;

namespace omega::app {

class SessionDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    // The view is needed for two things: the pulse has a viewport to
    // invalidate, and the folder chevron has to ask isExpanded(), which lives
    // on QTreeView rather than on QAbstractItemView -- a delegate that draws
    // its own branch indicator is tree-specific and there is no view-agnostic
    // way to ask.
    explicit SessionDelegate(QTreeView *view, QObject *parent = nullptr);

    void setTokens(const theme::Tokens &tokens);

    // Body text size in pixels, the same number omega.json's ui_font_size
    // carries and the same one generateTokenStylesheet() is given. This
    // delegate paints its own text, so it is the one thing in the window that
    // a stylesheet font-size cannot reach -- without this the tree stays at
    // the shipped 13px while every other widget follows the setting.
    void setBaseFontSize(int px);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;

private:
    void paintFolder(QPainter *painter, const QStyleOptionViewItem &option,
                     const QModelIndex &index) const;
    void paintChip(QPainter *painter, const QRect &rect, const QString &text,
                   const QColor &fg) const;

    QFont uiFont(int px, int weight) const;
    QFont monoFont(qreal px) const;

    int namePx() const;
    int folderPx() const;
    qreal detailPx() const;

    QTreeView *view_ = nullptr;

    // ONE animation for the whole view, not one per row. A timer per visible
    // row is how a tree of two hundred sessions spends a core on a dot.
    QVariantAnimation *pulse_ = nullptr;
    qreal pulseValue_ = 1.0;

    // 13 is what the sizes here were written as literals, so an application
    // that never calls setBaseFontSize gets the row it always had.
    int baseFontPx_ = 13;

    theme::Tokens tokens_ = theme::specTokens();
};

}  // namespace omega::app

#endif  // OMEGA_APP_SESSIONDELEGATE_H
