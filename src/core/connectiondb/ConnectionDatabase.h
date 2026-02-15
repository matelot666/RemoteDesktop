#pragma once

#include <QSqlDatabase>
#include <QVector>
#include "core/ConnectionEntry.h"
#include "core/Folder.h"

class ConnectionDatabase {
public:
    ConnectionDatabase();
    ~ConnectionDatabase();

    bool open(const QString &path);
    void close();

    // Folders
    qint64 insertFolder(const ConnectionFolder &folder);
    bool updateFolder(const ConnectionFolder &folder);
    bool deleteFolder(qint64 id);
    ConnectionFolder folderById(qint64 id) const;
    QVector<ConnectionFolder> foldersInParent(qint64 parentId) const;

    // Connections
    qint64 insertConnection(const ConnectionEntry &entry);
    bool updateConnection(const ConnectionEntry &entry);
    bool deleteConnection(qint64 id);
    ConnectionEntry connectionById(qint64 id) const;
    QVector<ConnectionEntry> connectionsInFolder(qint64 folderId) const;
    QVector<ConnectionEntry> allConnections() const;

    // Metadata (shared DB still has a metadata table for future use)
    QByteArray metadataValue(const QString &key) const;
    bool setMetadataValue(const QString &key, const QByteArray &value);

    // All folders (for export)
    QVector<ConnectionFolder> allFolders() const;

    // Move and reorder
    bool moveConnection(qint64 connectionId, qint64 newFolderId);
    bool moveFolder(qint64 folderId, qint64 newParentId);
    bool reorderConnections(qint64 folderId, const QVector<qint64> &idsInOrder);
    bool reorderFolders(qint64 parentId, const QVector<qint64> &idsInOrder);
    bool sortChildrenByName(qint64 parentFolderId);

    // Sort order
    int nextSortOrder(const QString &table, const QString &parentColumn, qint64 parentId) const;

private:
    bool createTables();
    void migrate();
    QSqlDatabase m_db;
    QString m_connectionName;
};
