// app/sessiontreepane.cpp

#include "app/sessiontreepane.h"

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QPushButton>
#include <QTimer>
#include <QTreeView>

#include "app/sessiondelegate.h"
#include <QVBoxLayout>

#include <functional>

#include "app/sessioneditordialog.h"
#include "app/sessionfilterproxy.h"
#include "app/sessiontreemodel.h"

namespace omega::app {
namespace {

// A tree with no rows is a featureless panel: the actions that would put
// something in it live in a context menu and a 30px button, and neither
// announces itself. nterm-qt has the same gap and gets away with it because a
// user arriving there already had a populated sessions.db. Omega can be the
// first thing to open a fresh one, so the empty case has to say what to do.
//
// A painted message rather than a placeholder widget swapped into the layout:
// the view keeps its geometry, its drop handling and its scrollbars, and there
// is no second thing that has to be shown and hidden in step with the model.
class EmptyStateTreeView : public QTreeView {
public:
    using QTreeView::QTreeView;

    void setEmptyText(const QString &text) { emptyText_ = text; }

protected:
    void paintEvent(QPaintEvent *event) override {
        QTreeView::paintEvent(event);
        if (!model() || model()->rowCount({}) > 0 || emptyText_.isEmpty())
            return;

        QPainter painter(viewport());
        // The palette's disabled text, so the hint reads as a hint in every
        // theme rather than as a row someone could click.
        painter.setPen(palette().color(QPalette::Disabled, QPalette::Text));
        QRect box = viewport()->rect().adjusted(16, 16, -16, -16);
        painter.drawText(box, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                         emptyText_);
    }

private:
    QString emptyText_;
};

// Long enough that typing a hostname is one filter pass rather than twelve,
// short enough not to feel laggy. The same number tree.py uses.
constexpr int kFilterDebounceMs = 150;

QString toQ(const std::string &s) { return QString::fromStdString(s); }

}  // namespace

SessionTreePane::SessionTreePane(sessions::SessionStore *store,
                                 omegassh::Vault *vault,
                                 const AppSettings &settings,
                                 const OmegaSettings &omega,
                                 const theme::ThemeEngine *themes,
                                 QWidget *parent)
    : QWidget(parent), store_(store), vault_(vault), settings_(settings),
      omega_(omega), themes_(themes) {
    buildUi();
    restoreExpansion();
    onSelectionChanged();
}

void SessionTreePane::buildUi() {
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(4);

    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(4);

    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Filter sessions..."));
    filter_->setClearButtonEnabled(true);
    toolbar->addWidget(filter_, 1);

    auto *quick = new QPushButton(tr("Quick Connect"));
    connect(quick, &QPushButton::clicked, this,
            &SessionTreePane::quickConnectRequested);
    toolbar->addWidget(quick);
    outer->addLayout(toolbar);

    model_ = new SessionTreeModel(store_, this);
    proxy_ = new SessionFilterProxy(this);
    proxy_->setSourceModel(model_);

    auto *tree = new EmptyStateTreeView;
    tree_ = tree;
    tree_->setModel(proxy_);
    tree_->setHeaderHidden(true);
    // The delegate draws the chevron itself, so the view must not also draw
    // one and must not indent for it -- see sessiondelegate.h. A two-line row
    // indented by the view's own margin loses the width the address needs.
    tree_->setRootIsDecorated(false);
    tree_->setIndentation(0);
    delegate_ = new SessionDelegate(tree_, this);
    tree_->setItemDelegate(delegate_);
    // Rows have to repaint on hover for the delegate's State_MouseOver branch;
    // without this the hover pill never appears.
    tree_->setMouseTracking(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setDragDropMode(QAbstractItemView::InternalMove);
    tree_->setDragEnabled(true);
    tree_->setAcceptDrops(true);
    tree_->setDropIndicatorShown(true);
    tree_->setDefaultDropAction(Qt::MoveAction);
    tree_->setAnimated(true);
    tree_->setExpandsOnDoubleClick(false);  // double-click connects instead
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setEmptyText(tr("No sessions yet.\n\nRight-click here, or use + below,\n"
                          "to add a session or a folder."));
    outer->addWidget(tree_, 1);

    filterTimer_ = new QTimer(this);
    filterTimer_->setSingleShot(true);
    connect(filterTimer_, &QTimer::timeout, this, &SessionTreePane::applyFilter);
    connect(filter_, &QLineEdit::textChanged, this,
            &SessionTreePane::onFilterChanged);

    connect(tree_, &QTreeView::doubleClicked, this,
            &SessionTreePane::onDoubleClicked);
    // With no expander to click, a folder toggles on a single click. Sessions
    // are untouched: a single click still only selects, and connecting is
    // still the double click it always was.
    connect(tree_, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        if (model_->kindOf(toSource(index)) != SessionTreeModel::FolderItem)
            return;
        tree_->setExpanded(index, !tree_->isExpanded(index));
    });
    connect(tree_, &QTreeView::customContextMenuRequested, this,
            &SessionTreePane::showContextMenu);
    connect(tree_, &QTreeView::expanded, this, &SessionTreePane::onExpanded);
    connect(tree_, &QTreeView::collapsed, this, &SessionTreePane::onCollapsed);
    connect(tree_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this](const QItemSelection &, const QItemSelection &) {
                onSelectionChanged();
            });

    // A reset is how every mutation lands, including a drop. Expansion comes
    // from folders.expanded rather than from anything the view kept, which is
    // what makes the reset survivable.
    connect(model_, &QAbstractItemModel::modelReset, this,
            [this] { restoreExpansion(); onSelectionChanged(); });

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(4);

    connectTabButton_ = new QPushButton(tr("Connect"));
    connectTabButton_->setToolTip(tr("Connect in a new tab"));
    connectTabButton_->setEnabled(false);
    connect(connectTabButton_, &QPushButton::clicked, this,
            [this] { connectSelected(InTab); });
    buttons->addWidget(connectTabButton_);

    connectWindowButton_ = new QPushButton(tr("New"));
    connectWindowButton_->setToolTip(tr("Connect in a separate window"));
    connectWindowButton_->setEnabled(false);
    connect(connectWindowButton_, &QPushButton::clicked, this,
            [this] { connectSelected(InWindow); });
    buttons->addWidget(connectWindowButton_);

    buttons->addStretch();

    addButton_ = new QPushButton(QStringLiteral("+"));
    addButton_->setToolTip(tr("Add a session or folder"));
    // The theme's QPushButton rule carries "min-width: 60px" and "padding:
    // 6px 16px", which together put a 94px floor under every button and quietly
    // beat setFixedWidth. Three of those in this row is a 290px floor under the
    // whole pane -- wider than the 260 default tree width, so the splitter
    // silently refuses the width the settings asked for.
    //
    // Overridden on the widget rather than in theme/stylesheet.cpp: that
    // generator is a byte-for-byte port with a differential test against
    // nterm-qt's, and adding a rule there would be a divergence in the one
    // place that must not have one. A widget-level sheet overrides only the
    // properties it names, so the colours still cascade from the theme.
    // The width comes from the sheet rather than from setFixedWidth for the
    // same reason: a fixed width loses to the theme, and having both would mean
    // two numbers that can disagree.
    addButton_->setStyleSheet(
        QStringLiteral("QPushButton { min-width: 20px; padding: 6px 4px; }"));
    connect(addButton_, &QPushButton::clicked, this, [this] {
        QMenu menu(this);
        menu.addAction(tr("New Session..."),
                       [this] { addSession(std::nullopt); });
        menu.addAction(tr("New Folder..."), [this] { addFolder(std::nullopt); });
        menu.exec(addButton_->mapToGlobal(addButton_->rect().bottomLeft()));
    });
    buttons->addWidget(addButton_);

    outer->addLayout(buttons);
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

void SessionTreePane::refresh() { model_->reload(); }

void SessionTreePane::setLiveRegistry(LiveSessionRegistry *registry) {
    model_->setLiveRegistry(registry);
}

void SessionTreePane::setTokens(const theme::Tokens &tokens) {
    if (delegate_) delegate_->setTokens(tokens);
}

void SessionTreePane::setBaseFontSize(int px) {
    if (delegate_) delegate_->setBaseFontSize(px);
}

QModelIndex SessionTreePane::toSource(const QModelIndex &proxyIndex) const {
    return proxy_->mapToSource(proxyIndex);
}

void SessionTreePane::restoreExpansion() {
    // Walking the proxy, setting each folder from the model's remembered flag.
    // setExpanded on the view re-enters onExpanded, which writes the same value
    // back; the model's setExpanded returns early when nothing changed, so the
    // round trip costs a comparison rather than a write per folder.
    std::function<void(const QModelIndex &)> walk =
        [&](const QModelIndex &parent) {
            const int rows = proxy_->rowCount(parent);
            for (int i = 0; i < rows; ++i) {
                const QModelIndex idx = proxy_->index(i, 0, parent);
                const QModelIndex src = toSource(idx);
                if (model_->kindOf(src) != SessionTreeModel::FolderItem)
                    continue;
                tree_->setExpanded(idx, model_->isExpanded(src));
                walk(idx);
            }
        };
    walk({});
}

void SessionTreePane::selectSession(qint64 id) {
    const QModelIndex src = model_->indexForSession(id);
    if (!src.isValid()) return;
    const QModelIndex idx = proxy_->mapFromSource(src);
    if (!idx.isValid()) return;

    tree_->setCurrentIndex(idx);
    tree_->scrollTo(idx);
}

std::optional<sessions::Session> SessionTreePane::selectedSession() const {
    const QModelIndex idx = tree_->currentIndex();
    if (!idx.isValid()) return std::nullopt;
    if (!tree_->selectionModel()->isSelected(idx)) return std::nullopt;
    return model_->sessionAt(proxy_->mapToSource(idx));
}

// ---------------------------------------------------------------------------
// filtering
// ---------------------------------------------------------------------------

void SessionTreePane::onFilterChanged() { filterTimer_->start(kFilterDebounceMs); }

void SessionTreePane::applyFilter() {
    proxy_->setFilterText(filter_->text());

    // The drag rule from the header comment, bound to the filter's own state
    // so there is one place that decides it.
    const bool filtering = proxy_->isFiltering();
    tree_->setDragDropMode(filtering ? QAbstractItemView::NoDragDrop
                                     : QAbstractItemView::InternalMove);
    tree_->setDragEnabled(!filtering);
    tree_->setAcceptDrops(!filtering);

    if (filtering) {
        // Everything that survived the filter, opened. A hit three folders
        // down that the user then has to go and find is not a search result.
        tree_->expandAll();
    } else {
        restoreExpansion();
    }
}

// ---------------------------------------------------------------------------
// selection and activation
// ---------------------------------------------------------------------------

void SessionTreePane::onSelectionChanged() {
    const bool haveSession = selectedSession().has_value();
    connectTabButton_->setEnabled(haveSession);
    connectWindowButton_->setEnabled(haveSession);
}

void SessionTreePane::onDoubleClicked(const QModelIndex &index) {
    const QModelIndex src = toSource(index);
    if (model_->kindOf(src) == SessionTreeModel::FolderItem) {
        tree_->setExpanded(index, !tree_->isExpanded(index));
        return;
    }
    if (auto session = model_->sessionAt(src)) emitConnect(*session, InTab);
}

void SessionTreePane::onExpanded(const QModelIndex &index) {
    // Not while filtering: expandAll() opened these, the user did not, and
    // recording it would leave the tree fully open once the filter clears.
    if (proxy_->isFiltering()) return;
    model_->setExpanded(toSource(index), true);
}

void SessionTreePane::onCollapsed(const QModelIndex &index) {
    if (proxy_->isFiltering()) return;
    model_->setExpanded(toSource(index), false);
}

void SessionTreePane::connectSelected(ConnectMode mode) {
    if (auto session = selectedSession()) emitConnect(*session, mode);
}

void SessionTreePane::emitConnect(const sessions::Session &session,
                                  ConnectMode mode) {
    if (session.id.has_value()) model_->recordConnect(*session.id);
    emit connectRequested(session, mode);
}

// ---------------------------------------------------------------------------
// context menu
// ---------------------------------------------------------------------------

void SessionTreePane::showContextMenu(const QPoint &pos) {
    const QModelIndex idx = tree_->indexAt(pos);
    QMenu menu(this);

    if (!idx.isValid()) {
        menu.addAction(tr("New Session..."), [this] { addSession(std::nullopt); });
        menu.addAction(tr("New Folder..."), [this] { addFolder(std::nullopt); });
        menu.exec(tree_->viewport()->mapToGlobal(pos));
        return;
    }

    const QModelIndex src = toSource(idx);

    if (model_->kindOf(src) == SessionTreeModel::SessionItem) {
        auto session = model_->sessionAt(src);
        if (!session) return;
        const sessions::Session s = *session;

        menu.addAction(tr("Connect in Tab"), [this, s] { emitConnect(s, InTab); });
        menu.addAction(tr("Connect in Window"),
                       [this, s] { emitConnect(s, InWindow); });
        menu.addSeparator();
        menu.addAction(tr("Edit..."), [this, s] { editSession(s); });
        menu.addAction(tr("Duplicate"), [this, s] { duplicateSession(s); });
        menu.addSeparator();
        menu.addAction(tr("Delete"), [this, s] { deleteSession(s); });
    } else {
        auto folder = model_->folderAt(src);
        if (!folder || !folder->id.has_value()) return;
        const sessions::Folder f = *folder;
        const qint64 id = *f.id;

        menu.addAction(tr("New Session Here..."),
                       [this, id] { addSession(id); });
        menu.addAction(tr("New Subfolder..."), [this, id] { addFolder(id); });
        menu.addSeparator();
        menu.addAction(tr("Rename..."), [this, f] { renameFolder(f); });
        menu.addSeparator();
        menu.addAction(tr("Delete Folder"), [this, f] { deleteFolder(f); });
    }

    menu.exec(tree_->viewport()->mapToGlobal(pos));
}

// ---------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------

void SessionTreePane::addSession(std::optional<qint64> folderId) {
    sessions::Session blank;
    if (folderId.has_value()) blank.folder_id = *folderId;

    SessionEditorDialog dialog(blank, store_, vault_, settings_, omega_,
                               themes_, this);
    if (dialog.exec() != QDialog::Accepted) return;

    if (auto id = model_->addSession(dialog.session())) {
        selectSession(*id);
    } else {
        QMessageBox::warning(this, tr("Add Session"), model_->lastError());
    }
}

void SessionTreePane::editSession(const sessions::Session &session) {
    SessionEditorDialog dialog(session, store_, vault_, settings_, omega_,
                               themes_, this);
    if (dialog.exec() != QDialog::Accepted) return;

    const sessions::Session updated = dialog.session();
    if (!model_->updateSession(updated)) {
        QMessageBox::warning(this, tr("Edit Session"), model_->lastError());
        return;
    }
    if (updated.id.has_value()) selectSession(*updated.id);
}

void SessionTreePane::duplicateSession(const sessions::Session &session) {
    if (!session.id.has_value()) return;
    if (auto id = model_->duplicateSession(*session.id)) selectSession(*id);
}

void SessionTreePane::deleteSession(const sessions::Session &session) {
    if (!session.id.has_value()) return;

    const auto answer = QMessageBox::question(
        this, tr("Delete Session"),
        tr("Delete session \"%1\"?").arg(toQ(session.name)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    model_->deleteSession(*session.id);
}

void SessionTreePane::addFolder(std::optional<qint64> parentId) {
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, tr("New Folder"), tr("Folder name:"),
                              QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    model_->addFolder(name.trimmed(), parentId);
}

void SessionTreePane::renameFolder(const sessions::Folder &folder) {
    if (!folder.id.has_value()) return;

    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Rename Folder"), tr("Folder name:"), QLineEdit::Normal,
        toQ(folder.name), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    model_->renameFolder(*folder.id, name.trimmed());
}

void SessionTreePane::deleteFolder(const sessions::Folder &folder) {
    if (!folder.id.has_value()) return;

    // The wording states what actually happens, which is not what the DDL's
    // ON DELETE CASCADE says would: the store reparents subfolders and
    // sessions to root rather than deleting them, because foreign keys are off
    // on a file nterm-qt also opens. A confirmation that promised a cascade
    // would be describing a different program.
    const auto answer = QMessageBox::question(
        this, tr("Delete Folder"),
        tr("Delete folder \"%1\"?\n\nSessions and subfolders inside it move to "
           "the top level.")
            .arg(toQ(folder.name)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    model_->deleteFolder(*folder.id);
}

}  // namespace omega::app
