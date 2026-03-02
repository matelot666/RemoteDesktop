#include "ConnectionTreeModel.h"
#include "app/Application.h"
#include "core/connectiondb/ConnectionDatabase.h"
#include "core/userdb/UserDatabase.h"
#include <QIcon>
#include <QIODevice>

ConnectionTreeModel::ConnectionTreeModel(QObject *parent)
    : QAbstractItemModel(parent)
{
    ConnectionFolder rootFolder;
    rootFolder.name = QStringLiteral("Root");
    m_rootItem = new TreeItem(rootFolder);
}

ConnectionTreeModel::~ConnectionTreeModel()
{
    delete m_rootItem;
}

QModelIndex ConnectionTreeModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent))
        return {};

    TreeItem *parentItem = parent.isValid()
        ? static_cast<TreeItem *>(parent.internalPointer())
        : m_rootItem;

    TreeItem *childItem = parentItem->child(row);
    if (childItem)
        return createIndex(row, column, childItem);
    return {};
}

QModelIndex ConnectionTreeModel::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return {};

    auto *childItem = static_cast<TreeItem *>(child.internalPointer());
    TreeItem *parentItem = childItem->parentItem();

    if (parentItem == m_rootItem || !parentItem)
        return {};

    return createIndex(parentItem->row(), 0, parentItem);
}

int ConnectionTreeModel::rowCount(const QModelIndex &parent) const
{
    TreeItem *parentItem = parent.isValid()
        ? static_cast<TreeItem *>(parent.internalPointer())
        : m_rootItem;
    return parentItem->childCount();
}

int ConnectionTreeModel::columnCount(const QModelIndex &) const
{
    return 1;
}

QVariant ConnectionTreeModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};

    auto *item = static_cast<TreeItem *>(index.internalPointer());

    if (role == Qt::DisplayRole) {
        return item->name();
    }

    if (role == Qt::DecorationRole) {
        if (item->nodeType() == TreeNodeType::Folder) {
            return QIcon(QStringLiteral(":/icons/folder.svg"));
        }
        auto state = item->connectionState();
        auto proto = item->connection().protocol;
        QString prefix = proto == Protocol::RDP ? QStringLiteral("rdp") : QStringLiteral("ssh");
        switch (state) {
        case ConnectionState::Connected:
            return QIcon(QStringLiteral(":/icons/%1-connected.svg").arg(prefix));
        case ConnectionState::Connecting:
            return QIcon(QStringLiteral(":/icons/%1-connecting.svg").arg(prefix));
        case ConnectionState::Error:
            return QIcon(QStringLiteral(":/icons/%1-error.svg").arg(prefix));
        default:
            return QIcon(QStringLiteral(":/icons/%1-disconnected.svg").arg(prefix));
        }
    }

    return {};
}

Qt::ItemFlags ConnectionTreeModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags defaultFlags = QAbstractItemModel::flags(index);

    bool admin = Application::instance()->isAdmin();

    if (!index.isValid())
        return defaultFlags | (admin ? Qt::ItemIsDropEnabled : Qt::ItemFlags{}); // root accepts drops (admin only)

    auto *item = static_cast<TreeItem *>(index.internalPointer());
    if (item->nodeType() == TreeNodeType::Folder)
        return defaultFlags | (admin ? Qt::ItemIsDropEnabled : Qt::ItemFlags{}); // folders accept drops (admin only)
    // Connections can be dragged (admin only) but don't accept drops
    return defaultFlags | (admin ? Qt::ItemIsDragEnabled : Qt::ItemFlags{});
}

Qt::DropActions ConnectionTreeModel::supportedDropActions() const
{
    return Qt::MoveAction;
}

QStringList ConnectionTreeModel::mimeTypes() const
{
    return {QStringLiteral("application/x-remotedesktop-treeitem")};
}

QMimeData *ConnectionTreeModel::mimeData(const QModelIndexList &indexes) const
{
    auto *mimeData = new QMimeData;
    QByteArray encoded;
    QDataStream stream(&encoded, QIODevice::WriteOnly);
    for (const auto &idx : indexes) {
        if (idx.isValid()) {
            auto *item = itemFromIndex(idx);
            stream << static_cast<int>(item->nodeType()) << item->itemId();
        }
    }
    mimeData->setData(QStringLiteral("application/x-remotedesktop-treeitem"), encoded);
    return mimeData;
}

bool ConnectionTreeModel::dropMimeData(const QMimeData *data, Qt::DropAction action,
                                       int row, int column, const QModelIndex &parent)
{
    Q_UNUSED(data)
    Q_UNUSED(action)
    Q_UNUSED(row)
    Q_UNUSED(column)
    Q_UNUSED(parent)
    // Drop handling is done in ConnectionTreeView::dropEvent()
    return false;
}

void ConnectionTreeModel::clear()
{
    beginResetModel();
    m_rootItem->removeAllChildren();
    endResetModel();
}

void ConnectionTreeModel::addFolder(const ConnectionFolder &folder, const QModelIndex &parent)
{
    TreeItem *parentItem = parent.isValid()
        ? static_cast<TreeItem *>(parent.internalPointer())
        : m_rootItem;

    int row = parentItem->childCount();
    beginInsertRows(parent, row, row);
    parentItem->appendChild(new TreeItem(folder));
    endInsertRows();
}

void ConnectionTreeModel::addConnection(const ConnectionEntry &entry, const QModelIndex &parent)
{
    TreeItem *parentItem = parent.isValid()
        ? static_cast<TreeItem *>(parent.internalPointer())
        : m_rootItem;

    int row = parentItem->childCount();
    beginInsertRows(parent, row, row);
    parentItem->appendChild(new TreeItem(entry));
    endInsertRows();
}

void ConnectionTreeModel::removeItem(const QModelIndex &index)
{
    if (!index.isValid())
        return;

    auto *item = static_cast<TreeItem *>(index.internalPointer());
    TreeItem *parent = item->parentItem();
    if (!parent)
        return;

    QModelIndex parentIndex = (parent == m_rootItem) ? QModelIndex() : indexFromItem(parent);
    int row = item->row();
    beginRemoveRows(parentIndex, row, row);
    parent->removeChild(row);
    endRemoveRows();
}

TreeItem *ConnectionTreeModel::itemFromIndex(const QModelIndex &index) const
{
    if (!index.isValid())
        return m_rootItem;
    return static_cast<TreeItem *>(index.internalPointer());
}

QModelIndex ConnectionTreeModel::indexFromItem(TreeItem *item) const
{
    if (!item || item == m_rootItem)
        return {};
    return createIndex(item->row(), 0, item);
}

TreeItem *ConnectionTreeModel::findItemById(TreeNodeType type, qint64 id) const
{
    // Recursive search from root
    std::function<TreeItem *(TreeItem *)> search = [&](TreeItem *node) -> TreeItem * {
        if (node != m_rootItem && node->nodeType() == type && node->itemId() == id)
            return node;
        for (int i = 0; i < node->childCount(); ++i) {
            if (TreeItem *found = search(node->child(i)))
                return found;
        }
        return nullptr;
    };
    return search(m_rootItem);
}

bool ConnectionTreeModel::isDescendantOf(TreeItem *item, TreeItem *potentialAncestor) const
{
    TreeItem *current = item;
    while (current) {
        if (current == potentialAncestor)
            return true;
        current = current->parentItem();
    }
    return false;
}

void ConnectionTreeModel::loadFromDatabase()
{
    auto *db = Application::instance()->database();
    if (!db)
        return;

    // Pre-load user DB overlays
    auto *userDb = Application::instance()->userDatabase();
    if (userDb) {
        m_credentialAssignments = userDb->allCredentialAssignments();
        m_folderDefaults = userDb->allFolderDefaults();
    } else {
        m_credentialAssignments.clear();
        m_folderDefaults.clear();
    }

    // Fetch ALL folders and connections in 2 queries (instead of 2 per folder)
    auto allFolders = db->allFolders();
    auto allConns = db->allConnections();

    // Group by parent/folder and apply user DB overlays
    QHash<qint64, QVector<ConnectionFolder>> foldersByParent;
    for (auto &f : allFolders) {
        if (m_folderDefaults.contains(f.id)) {
            const auto &fd = m_folderDefaults[f.id];
            f.defaultRdpCredentialId = fd.defaultRdpCredentialId;
            f.defaultSshCredentialId = fd.defaultSshCredentialId;
        }
        foldersByParent[f.parentId].append(f);
    }

    QHash<qint64, QVector<ConnectionEntry>> connsByFolder;
    for (auto &c : allConns) {
        if (m_credentialAssignments.contains(c.id))
            c.credentialId = m_credentialAssignments[c.id];
        connsByFolder[c.folderId].append(c);
    }

    beginResetModel();
    m_rootItem->removeAllChildren();
    buildTreeFromMaps(m_rootItem, -1, foldersByParent, connsByFolder);
    endResetModel();
}

void ConnectionTreeModel::buildTreeFromMaps(
    TreeItem *parentItem, qint64 parentFolderId,
    const QHash<qint64, QVector<ConnectionFolder>> &foldersByParent,
    const QHash<qint64, QVector<ConnectionEntry>> &connsByFolder)
{
    auto folderIt = foldersByParent.find(parentFolderId);
    if (folderIt != foldersByParent.end()) {
        for (const auto &folder : *folderIt) {
            auto *folderItem = new TreeItem(folder);
            parentItem->appendChild(folderItem);
            buildTreeFromMaps(folderItem, folder.id, foldersByParent, connsByFolder);
        }
    }

    auto connIt = connsByFolder.find(parentFolderId);
    if (connIt != connsByFolder.end()) {
        for (const auto &conn : *connIt) {
            parentItem->appendChild(new TreeItem(conn));
        }
    }
}

void ConnectionTreeModel::setConnectionState(const QModelIndex &index, ConnectionState state)
{
    if (!index.isValid())
        return;
    auto *item = static_cast<TreeItem *>(index.internalPointer());
    item->setConnectionState(state);
    emit dataChanged(index, index, {Qt::DecorationRole});
}
