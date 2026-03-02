#include "ConnectionTreeView.h"
#include "ConnectionTreeModel.h"
#include "TreeItem.h"
#include "app/Application.h"
#include "core/connectiondb/ConnectionDatabase.h"
#include <QMenu>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QHeaderView>
#include <QMimeData>
#include <QIODevice>
#include <QTimer>
#include <QPainter>
#include <QStyledItemDelegate>

// Delegate that draws a highlight flash on the active flash row
class FlashDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    ConnectionTreeView *treeView() const { return static_cast<ConnectionTreeView *>(parent()); }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        auto *tv = treeView();
        if (tv->m_flashStep > 0 && tv->m_flashIndex.isValid() && index == tv->m_flashIndex) {
            int alpha = tv->m_flashStep * 18;  // 8 steps × 18 = 144 max alpha
            painter->fillRect(option.rect, QColor(100, 180, 255, alpha));
        }
    }
};

ConnectionTreeView::ConnectionTreeView(QWidget *parent)
    : QTreeView(parent)
{
    setHeaderHidden(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::InternalMove);
    setAnimated(true);
    setExpandsOnDoubleClick(false);
    setItemDelegate(new FlashDelegate(this));

    m_flashTimer = new QTimer(this);
    m_flashTimer->setInterval(30);
    connect(m_flashTimer, &QTimer::timeout, this, [this]() {
        if (--m_flashStep <= 0) {
            m_flashStep = 0;
            m_flashTimer->stop();
        }
        if (m_flashIndex.isValid())
            update(m_flashIndex);
    });
}

void ConnectionTreeView::setModel(QAbstractItemModel *model)
{
    QTreeView::setModel(model);
    m_model = qobject_cast<ConnectionTreeModel *>(model);
}

void ConnectionTreeView::mouseDoubleClickEvent(QMouseEvent *event)
{
    QModelIndex index = indexAt(event->pos());
    if (index.isValid() && m_model) {
        auto *item = m_model->itemFromIndex(index);
        if (item && item->nodeType() == TreeNodeType::Connection) {
            flashRow(index);
            emit connectRequested(index);
            return;
        }
        if (item && item->nodeType() == TreeNodeType::Folder) {
            setExpanded(index, !isExpanded(index));
            return;
        }
    }
    QTreeView::mouseDoubleClickEvent(event);
}

void ConnectionTreeView::contextMenuEvent(QContextMenuEvent *event)
{
    QModelIndex index = indexAt(event->pos());
    QMenu menu(this);

    bool admin = Application::instance()->isAdmin();

    if (index.isValid() && m_model) {
        auto *item = m_model->itemFromIndex(index);

        if (item->nodeType() == TreeNodeType::Connection) {
            auto state = item->connectionState();
            if (state == ConnectionState::Connected) {
                menu.addAction(QStringLiteral("Disconnect"), [this, index]() {
                    emit disconnectRequested(index);
                });
                menu.addAction(QStringLiteral("Reconnect"), [this, index]() {
                    emit reconnectRequested(index);
                });
            } else {
                menu.addAction(QStringLiteral("Connect"), [this, index]() {
                    emit connectRequested(index);
                });
            }
            menu.addSeparator();
        }

        if (item->nodeType() == TreeNodeType::Folder) {
            int serverCount = countConnectionsUnder(item);
            if (serverCount > 0) {
                menu.addAction(QStringLiteral("Connect to All (%1 servers)").arg(serverCount),
                               [this, index]() {
                    emit connectAllRequested(index);
                });
                int connectedCount = countConnectedUnder(item);
                if (connectedCount > 0) {
                    menu.addAction(QStringLiteral("Disconnect All (%1 connected)").arg(connectedCount),
                                   [this, index]() {
                        emit disconnectAllRequested(index);
                    });
                }
                menu.addSeparator();
            }
            if (admin) {
                menu.addAction(QStringLiteral("Add Folder..."), [this, index]() {
                    emit addFolderRequested(index);
                });
                menu.addAction(QStringLiteral("Add Connection..."), [this, index]() {
                    emit addConnectionRequested(index);
                });
                menu.addAction(QStringLiteral("Sort by Name"), [this, index]() {
                    emit sortByNameRequested(index);
                });
                menu.addSeparator();
            }
        }

        menu.addAction(QStringLiteral("Edit..."), [this, index]() {
            emit editRequested(index);
        });
        if (admin) {
            menu.addAction(QStringLiteral("Delete"), [this, index]() {
                emit deleteRequested(index);
            });
        }
    } else if (admin) {
        // Right-click on empty space (admin only)
        menu.addAction(QStringLiteral("Add Folder..."), [this]() {
            emit addFolderRequested(QModelIndex());
        });
        menu.addAction(QStringLiteral("Add Connection..."), [this]() {
            emit addConnectionRequested(QModelIndex());
        });
        menu.addAction(QStringLiteral("Sort by Name"), [this]() {
            emit sortByNameRequested(QModelIndex());
        });
    }

    if (!menu.isEmpty())
        menu.exec(event->globalPos());
}

// -- Drag-and-drop (connections only) -----------------------------------

void ConnectionTreeView::dropEvent(QDropEvent *event)
{
    if (!m_model || !Application::instance()->isAdmin()) {
        event->ignore();
        return;
    }

    const QMimeData *mimeData = event->mimeData();
    if (!mimeData->hasFormat(QStringLiteral("application/x-remotedesktop-treeitem"))) {
        event->ignore();
        return;
    }

    QByteArray encoded = mimeData->data(QStringLiteral("application/x-remotedesktop-treeitem"));
    QDataStream stream(&encoded, QIODevice::ReadOnly);
    if (stream.atEnd()) {
        event->ignore();
        return;
    }

    int nodeTypeInt;
    qint64 itemId;
    stream >> nodeTypeInt >> itemId;
    auto nodeType = static_cast<TreeNodeType>(nodeTypeInt);

    // Only connections can be dragged
    if (nodeType != TreeNodeType::Connection) {
        event->ignore();
        return;
    }

    auto *db = Application::instance()->database();
    if (!db) {
        event->ignore();
        return;
    }

    TreeItem *sourceItem = m_model->findItemById(nodeType, itemId);
    if (!sourceItem || !sourceItem->parentItem()) {
        event->ignore();
        return;
    }

    QModelIndex dropIndex = indexAt(event->position().toPoint());
    DropIndicatorPosition indicator = dropIndicatorPosition();

    TreeItem *targetParent = nullptr;
    int insertRow = -1;

    if (!dropIndex.isValid()) {
        targetParent = m_model->itemFromIndex(QModelIndex());
        insertRow = targetParent->childCount();
    } else {
        TreeItem *dropItem = m_model->itemFromIndex(dropIndex);
        switch (indicator) {
        case QAbstractItemView::OnItem:
            if (dropItem->nodeType() == TreeNodeType::Folder) {
                targetParent = dropItem;
                insertRow = targetParent->childCount();
            } else {
                targetParent = dropItem->parentItem();
                if (!targetParent)
                    targetParent = m_model->itemFromIndex(QModelIndex());
                insertRow = dropItem->row();
            }
            break;
        case QAbstractItemView::AboveItem:
            targetParent = dropItem->parentItem();
            if (!targetParent)
                targetParent = m_model->itemFromIndex(QModelIndex());
            insertRow = dropItem->row();
            break;
        case QAbstractItemView::BelowItem:
            targetParent = dropItem->parentItem();
            if (!targetParent)
                targetParent = m_model->itemFromIndex(QModelIndex());
            insertRow = dropItem->row() + 1;
            break;
        default:
            targetParent = m_model->itemFromIndex(QModelIndex());
            insertRow = targetParent->childCount();
            break;
        }
    }

    if (!targetParent)
        targetParent = m_model->itemFromIndex(QModelIndex());

    TreeItem *sourceParent = sourceItem->parentItem();
    TreeItem *rootItem = m_model->itemFromIndex(QModelIndex());

    qint64 sourceFolderId = (sourceParent != rootItem &&
                             sourceParent->nodeType() == TreeNodeType::Folder)
                                ? sourceParent->folder().id : -1;
    qint64 targetFolderId = (targetParent != rootItem &&
                             targetParent->nodeType() == TreeNodeType::Folder)
                                ? targetParent->folder().id : -1;

    bool reparenting = (sourceFolderId != targetFolderId);

    if (reparenting)
        db->moveConnection(itemId, targetFolderId);

    // Build new child ordering for the target parent
    QVector<TreeItem *> children;
    for (int i = 0; i < targetParent->childCount(); ++i)
        children.append(targetParent->child(i));

    if (!reparenting) {
        int sourceRow = children.indexOf(sourceItem);
        if (sourceRow >= 0) {
            children.removeAt(sourceRow);
            if (sourceRow < insertRow)
                insertRow--;
        }
    }

    insertRow = qBound(0, insertRow, children.size());
    children.insert(insertRow, sourceItem);

    // Persist connection ordering
    QVector<qint64> connectionIds;
    for (auto *child : children) {
        if (child->nodeType() == TreeNodeType::Connection)
            connectionIds.append(child->itemId());
    }
    if (!connectionIds.isEmpty())
        db->reorderConnections(targetFolderId, connectionIds);

    // Close sort_order gaps in source parent if reparented
    if (reparenting) {
        QVector<qint64> srcConnectionIds;
        for (int i = 0; i < sourceParent->childCount(); ++i) {
            auto *child = sourceParent->child(i);
            if (child != sourceItem && child->nodeType() == TreeNodeType::Connection)
                srcConnectionIds.append(child->itemId());
        }
        if (!srcConnectionIds.isEmpty())
            db->reorderConnections(sourceFolderId, srcConnectionIds);
    }

    auto expandedIds = saveExpandedFolderIds();
    m_model->loadFromDatabase();
    restoreExpandedFolderIds(expandedIds);

    event->accept();
}

// -- Shift+Arrow reordering (folders and connections) -------------------

void ConnectionTreeView::keyPressEvent(QKeyEvent *event)
{
    // Mask out KeypadModifier -- macOS reports it for arrow keys
    auto mods = event->modifiers() & ~Qt::KeypadModifier;
    if (mods == Qt::ShiftModifier && Application::instance()->isAdmin()) {
        if (event->key() == Qt::Key_Up) {
            moveItemInOrder(currentIndex(), -1);
            return;
        }
        if (event->key() == Qt::Key_Down) {
            moveItemInOrder(currentIndex(), +1);
            return;
        }
        if (event->key() == Qt::Key_Right) {
            indentFolder(currentIndex());
            return;
        }
        if (event->key() == Qt::Key_Left) {
            outdentFolder(currentIndex());
            return;
        }
    }
    QTreeView::keyPressEvent(event);
}

void ConnectionTreeView::moveItemInOrder(const QModelIndex &index, int direction)
{
    if (!index.isValid() || !m_model)
        return;

    auto *db = Application::instance()->database();
    if (!db)
        return;

    auto *item = m_model->itemFromIndex(index);
    if (!item || !item->parentItem())
        return;

    TreeItem *parentItem = item->parentItem();
    TreeItem *rootItem = m_model->itemFromIndex(QModelIndex());
    qint64 parentFolderId = (parentItem != rootItem &&
                             parentItem->nodeType() == TreeNodeType::Folder)
                                ? parentItem->folder().id : -1;

    // Collect sibling IDs of the same type in current order
    QVector<qint64> siblingIds;
    for (int i = 0; i < parentItem->childCount(); ++i) {
        auto *child = parentItem->child(i);
        if (child->nodeType() == item->nodeType())
            siblingIds.append(child->itemId());
    }

    int pos = siblingIds.indexOf(item->itemId());
    if (pos < 0)
        return;

    int newPos = pos + direction;
    if (newPos < 0 || newPos >= siblingIds.size())
        return; // already at the edge

    // Swap
    siblingIds.swapItemsAt(pos, newPos);

    // Persist
    if (item->nodeType() == TreeNodeType::Folder)
        db->reorderFolders(parentFolderId, siblingIds);
    else
        db->reorderConnections(parentFolderId, siblingIds);

    // Reload preserving expansion and reselect the moved item
    auto expandedIds = saveExpandedFolderIds();
    m_model->loadFromDatabase();
    restoreExpandedFolderIds(expandedIds);

    // Reselect the moved item
    TreeItem *movedItem = m_model->findItemById(item->nodeType(), item->itemId());
    if (movedItem) {
        QModelIndex newIndex = m_model->indexFromItem(movedItem);
        setCurrentIndex(newIndex);
    }
}

// -- Shift+Right/Left: indent/outdent folders ---------------------------

void ConnectionTreeView::indentFolder(const QModelIndex &index)
{
    if (!index.isValid() || !m_model)
        return;

    auto *item = m_model->itemFromIndex(index);
    if (!item || item->nodeType() != TreeNodeType::Folder || !item->parentItem())
        return;

    auto *db = Application::instance()->database();
    if (!db)
        return;

    TreeItem *parentItem = item->parentItem();
    TreeItem *rootItem = m_model->itemFromIndex(QModelIndex());
    qint64 parentFolderId = (parentItem != rootItem &&
                             parentItem->nodeType() == TreeNodeType::Folder)
                                ? parentItem->folder().id : -1;

    // Find the previous sibling folder to nest into
    TreeItem *targetFolder = nullptr;
    for (int i = 0; i < parentItem->childCount(); ++i) {
        auto *child = parentItem->child(i);
        if (child == item)
            break;
        if (child->nodeType() == TreeNodeType::Folder)
            targetFolder = child;
    }

    if (!targetFolder)
        return; // no sibling folder above to nest into

    qint64 targetFolderId = targetFolder->folder().id;

    // Move in DB
    db->moveFolder(item->folder().id, targetFolderId);

    // Reorder source parent to close gaps
    QVector<qint64> srcFolderIds;
    for (int i = 0; i < parentItem->childCount(); ++i) {
        auto *child = parentItem->child(i);
        if (child->nodeType() == TreeNodeType::Folder && child != item)
            srcFolderIds.append(child->itemId());
    }
    if (!srcFolderIds.isEmpty())
        db->reorderFolders(parentFolderId, srcFolderIds);

    // Reload, expand the target folder so the moved item is visible, reselect
    auto expandedIds = saveExpandedFolderIds();
    expandedIds.insert(targetFolderId);
    m_model->loadFromDatabase();
    restoreExpandedFolderIds(expandedIds);

    TreeItem *movedItem = m_model->findItemById(TreeNodeType::Folder, item->folder().id);
    if (movedItem)
        setCurrentIndex(m_model->indexFromItem(movedItem));
}

void ConnectionTreeView::outdentFolder(const QModelIndex &index)
{
    if (!index.isValid() || !m_model)
        return;

    auto *item = m_model->itemFromIndex(index);
    if (!item || item->nodeType() != TreeNodeType::Folder || !item->parentItem())
        return;

    auto *db = Application::instance()->database();
    if (!db)
        return;

    TreeItem *parentItem = item->parentItem();
    TreeItem *rootItem = m_model->itemFromIndex(QModelIndex());

    // Can only outdent if currently inside a subfolder (not at root)
    if (parentItem == rootItem)
        return;
    if (parentItem->nodeType() != TreeNodeType::Folder)
        return;

    TreeItem *grandparent = parentItem->parentItem();
    if (!grandparent)
        return;

    qint64 oldParentId = parentItem->folder().id;
    qint64 newParentId = (grandparent != rootItem &&
                          grandparent->nodeType() == TreeNodeType::Folder)
                             ? grandparent->folder().id : -1;

    // Move in DB -- places it at the end of the new parent
    db->moveFolder(item->folder().id, newParentId);

    // Reorder old parent to close gaps
    QVector<qint64> oldFolderIds;
    for (int i = 0; i < parentItem->childCount(); ++i) {
        auto *child = parentItem->child(i);
        if (child->nodeType() == TreeNodeType::Folder && child != item)
            oldFolderIds.append(child->itemId());
    }
    if (!oldFolderIds.isEmpty())
        db->reorderFolders(oldParentId, oldFolderIds);

    // Place it right after its old parent in the new parent's folder ordering
    QVector<qint64> newFolderIds;
    for (int i = 0; i < grandparent->childCount(); ++i) {
        auto *child = grandparent->child(i);
        if (child->nodeType() == TreeNodeType::Folder) {
            newFolderIds.append(child->itemId());
            if (child == parentItem)
                newFolderIds.append(item->folder().id);
        }
    }
    // If the moved folder ended up appended by moveFolder, remove the duplicate
    if (!newFolderIds.isEmpty() && newFolderIds.count(item->folder().id) > 1)
        newFolderIds.removeFirst(); // remove the one added by moveFolder at the end
    db->reorderFolders(newParentId, newFolderIds);

    // Reload, reselect
    auto expandedIds = saveExpandedFolderIds();
    m_model->loadFromDatabase();
    restoreExpandedFolderIds(expandedIds);

    TreeItem *movedItem = m_model->findItemById(TreeNodeType::Folder, item->folder().id);
    if (movedItem)
        setCurrentIndex(m_model->indexFromItem(movedItem));
}

int ConnectionTreeView::countConnectionsUnder(TreeItem *item) const
{
    int count = 0;
    for (int i = 0; i < item->childCount(); ++i) {
        auto *child = item->child(i);
        if (child->nodeType() == TreeNodeType::Connection)
            ++count;
        else if (child->nodeType() == TreeNodeType::Folder)
            count += countConnectionsUnder(child);
    }
    return count;
}

int ConnectionTreeView::countConnectedUnder(TreeItem *item) const
{
    int count = 0;
    for (int i = 0; i < item->childCount(); ++i) {
        auto *child = item->child(i);
        if (child->nodeType() == TreeNodeType::Connection) {
            if (child->connectionState() == ConnectionState::Connected ||
                child->connectionState() == ConnectionState::Connecting)
                ++count;
        } else if (child->nodeType() == TreeNodeType::Folder) {
            count += countConnectedUnder(child);
        }
    }
    return count;
}

// -- Double-click flash animation ---------------------------------------

void ConnectionTreeView::flashRow(const QModelIndex &index)
{
    m_flashIndex = QPersistentModelIndex(index);
    m_flashStep = 8;  // ~240ms fade-out (8 × 30ms)
    m_flashTimer->start();
    update(index);
}

// -- Expansion state save/restore ---------------------------------------

QSet<qint64> ConnectionTreeView::saveExpandedFolderIds() const
{
    QSet<qint64> ids;
    collectExpandedIds(QModelIndex(), ids);
    return ids;
}

void ConnectionTreeView::restoreExpandedFolderIds(const QSet<qint64> &ids)
{
    restoreExpandedIds(QModelIndex(), ids);
}

void ConnectionTreeView::collectExpandedIds(const QModelIndex &parent, QSet<qint64> &ids) const
{
    if (!m_model)
        return;
    int rows = m_model->rowCount(parent);
    for (int i = 0; i < rows; ++i) {
        QModelIndex idx = m_model->index(i, 0, parent);
        auto *item = m_model->itemFromIndex(idx);
        if (item && item->nodeType() == TreeNodeType::Folder) {
            if (isExpanded(idx))
                ids.insert(item->folder().id);
            collectExpandedIds(idx, ids);
        }
    }
}

void ConnectionTreeView::restoreExpandedIds(const QModelIndex &parent, const QSet<qint64> &ids)
{
    if (!m_model)
        return;
    int rows = m_model->rowCount(parent);
    for (int i = 0; i < rows; ++i) {
        QModelIndex idx = m_model->index(i, 0, parent);
        auto *item = m_model->itemFromIndex(idx);
        if (item && item->nodeType() == TreeNodeType::Folder) {
            if (ids.contains(item->folder().id))
                setExpanded(idx, true);
            restoreExpandedIds(idx, ids);
        }
    }
}
