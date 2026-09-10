// app/sessiontreepane.h
//
// The left pane: a filter box, the tree, and the buttons under it.
//
// A port of manager/tree.py's SessionTreeWidget onto a QTreeView and the model
// next door. The behaviour is the same one -- double-click connects, the
// context menu offers the same nine actions, folders remember whether they
// were open -- and the parts that differ are the parts that had to, because
// the view is no longer the place the tree lives.
//
// The one behavioural rule with no counterpart in nterm-qt: dragging is
// switched off while the filter is non-empty. There it is a latent hazard --
// _persist_tree_state walks the visible tree, a filtered tree is missing rows,
// and a drop under a filter therefore rewrites positions from a partial
// picture. The model here computes a drop from the model rather than the view,
// so the hazard is gone, but a drag under a filter still means something the
// user did not intend: the row above the drop point is not the row that will
// be above it once the filter clears. Refusing is honest. Restoring the drag
// mode is bound to the filter's own state so the two cannot drift.

#ifndef OMEGA_APP_SESSIONTREEPANE_H
#define OMEGA_APP_SESSIONTREEPANE_H

#include <QWidget>

#include <optional>

#include <omegasshvault.h>

#include "app/omegasettings.h"
#include "app/settings.h"
#include "sessions/store.h"
#include "theme/theme.h"
#include "theme/tokens.h"

class QLineEdit;
class QModelIndex;
class QPoint;
class QPushButton;
class QTimer;
class QTreeView;

namespace omega::app {

class LiveSessionRegistry;
class SessionDelegate;

class SessionFilterProxy;
class SessionTreeModel;

class SessionTreePane : public QWidget {
    Q_OBJECT

public:
    // How a connect request wants to be opened. Window is Phase 5's; the tree
    // emits it now so the menu is real, and the shell says which phase owns it.
    enum ConnectMode { InTab, InWindow };
    Q_ENUM(ConnectMode)

    // store must outlive the pane and is not owned. vault may be null; it is
    // read only for the editor's credential picker, which needs names.
    //
    // settings and omega are passed through to the session editor and read
    // nowhere else in the pane. The editor labels its inherit rows with what
    // the globals currently hold, so it needs to see them; it never writes
    // either, and both must outlive the pane for the same reason store does.
    // themes is passed through to the session editor and read nowhere else
    // here, exactly as settings and omega are: the editor's theme row needs
    // the installed names to offer. Null is allowed; see the editor.
    SessionTreePane(sessions::SessionStore *store, omegassh::Vault *vault,
                    const AppSettings &settings, const OmegaSettings &omega,
                    const theme::ThemeEngine *themes = nullptr,
                    QWidget *parent = nullptr);

    // Reloads from the store. The shell calls this after anything outside the
    // pane changes the database.
    void refresh();

    // The delegate reads LinkRole and RttRole through the model; this is what
    // gives the model something to read.
    void setLiveRegistry(LiveSessionRegistry *registry);

    // The delegate paints from tokens rather than from a stylesheet, so a
    // theme change has to reach it. MainWindow::applyTheme calls this on the
    // same walk that themes everything else.
    void setTokens(const theme::Tokens &tokens);

    // Same journey as setTokens and for the same reason: the delegate paints
    // its own text, so omega.json's ui_font_size cannot reach it through the
    // stylesheet. Called on the same walk.
    void setBaseFontSize(int px);

    // The selected session, or empty when the selection is a folder or
    // nothing. The connect buttons key off this.
    std::optional<sessions::Session> selectedSession() const;

signals:
    // The session has already had its connect recorded when this fires: a
    // connect that the shell then refuses to open is still a connect the user
    // asked for, and nterm-qt records it at the same point.
    void connectRequested(const omega::sessions::Session &session,
                          omega::app::SessionTreePane::ConnectMode mode);

    // The pane's own button. The shell owns the dialog.
    void quickConnectRequested();

private:
    void buildUi();
    void restoreExpansion();
    void selectSession(qint64 id);

    void onFilterChanged();
    void applyFilter();
    void onSelectionChanged();
    void onDoubleClicked(const QModelIndex &index);
    void onExpanded(const QModelIndex &index);
    void onCollapsed(const QModelIndex &index);
    void showContextMenu(const QPoint &pos);

    void connectSelected(ConnectMode mode);
    void emitConnect(const sessions::Session &session, ConnectMode mode);

    void addSession(std::optional<qint64> folderId);
    void editSession(const sessions::Session &session);
    void duplicateSession(const sessions::Session &session);
    void deleteSession(const sessions::Session &session);
    void addFolder(std::optional<qint64> parentId);
    void renameFolder(const sessions::Folder &folder);
    void deleteFolder(const sessions::Folder &folder);

    // Maps a proxy index to the source model's. Every handler here is handed a
    // proxy index by the view and every model call wants a source one; going
    // through one named function is how that stops being a per-handler
    // opportunity to forget.
    QModelIndex toSource(const QModelIndex &proxyIndex) const;

    sessions::SessionStore *store_ = nullptr;
    omegassh::Vault *vault_ = nullptr;
    const AppSettings &settings_;
    const OmegaSettings &omega_;
    const theme::ThemeEngine *themes_ = nullptr;

    SessionTreeModel *model_ = nullptr;
    SessionFilterProxy *proxy_ = nullptr;

    QLineEdit *filter_ = nullptr;
    QTimer *filterTimer_ = nullptr;
    QTreeView *tree_ = nullptr;
    SessionDelegate *delegate_ = nullptr;
    QPushButton *connectTabButton_ = nullptr;
    QPushButton *connectWindowButton_ = nullptr;
    QPushButton *addButton_ = nullptr;
};

}  // namespace omega::app

#endif  // OMEGA_APP_SESSIONTREEPANE_H
