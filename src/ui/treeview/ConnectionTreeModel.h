#pragma once

#include <QAbstractItemModel>
#include <QHash>
#include <QMimeData>
#include "TreeItem.h"

struct FolderDefaults;

class ConnectionTreeModel : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit ConnectionTreeModel(QObject *parent = nullptr);
    ~ConnectionTreeModel() override;

    // QAbstractItemModel interface
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    Qt::DropActions supportedDropActions() const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action,
                      int row, int column, const QModelIndex &parent) override;

    // Tree building
    void clear();
    void addFolder(const ConnectionFolder &folder, const QModelIndex &parent = {});
    void addConnection(const ConnectionEntry &entry, const QModelIndex &parent = {});
    void removeItem(const QModelIndex &index);

    TreeItem *itemFromIndex(const QModelIndex &index) const;
    QModelIndex indexFromItem(TreeItem *item) const;

    // Search
    TreeItem *findItemById(TreeNodeType type, qint64 id) const;
    bool isDescendantOf(TreeItem *item, TreeItem *potentialAncestor) const;

    // Reload entire tree from database
    void loadFromDatabase();

    // Update connection state
    void setConnectionState(const QModelIndex &index, ConnectionState state);

signals:
    void connectionDoubleClicked(const ConnectionEntry &entry);

private:
    TreeItem *m_rootItem = nullptr;

    // Cached user DB overlays (loaded in loadFromDatabase)
    QHash<qint64, qint64> m_credentialAssignments;     // connection_id -> credential_id
    QHash<qint64, FolderDefaults> m_folderDefaults;     // folder_id -> defaults

    void buildTreeFromMaps(TreeItem *parentItem, qint64 parentFolderId,
                           const QHash<qint64, QVector<ConnectionFolder>> &foldersByParent,
                           const QHash<qint64, QVector<ConnectionEntry>> &connsByFolder);
};
