#pragma once

#include <QVector>
#include <QString>
#include "core/Enums.h"
#include "core/ConnectionEntry.h"
#include "core/Folder.h"

class TreeItem {
public:
    // Folder node
    explicit TreeItem(const ConnectionFolder &folder, TreeItem *parent = nullptr);
    // Connection node
    explicit TreeItem(const ConnectionEntry &entry, TreeItem *parent = nullptr);
    ~TreeItem();

    TreeItem *child(int row) const;
    int childCount() const;
    int row() const;
    TreeItem *parentItem() const;

    void appendChild(TreeItem *child);
    void insertChild(int row, TreeItem *child);
    void removeChild(int row);
    TreeItem *takeChild(int row); // remove without deleting
    void removeAllChildren();

    TreeNodeType nodeType() const { return m_nodeType; }
    QString name() const;
    qint64 itemId() const;
    ConnectionState connectionState() const { return m_connectionState; }
    void setConnectionState(ConnectionState state) { m_connectionState = state; }

    ConnectionFolder &folder() { return m_folder; }
    const ConnectionFolder &folder() const { return m_folder; }
    ConnectionEntry &connection() { return m_connection; }
    const ConnectionEntry &connection() const { return m_connection; }

private:
    TreeNodeType m_nodeType;
    ConnectionFolder m_folder;
    ConnectionEntry m_connection;
    ConnectionState m_connectionState = ConnectionState::Disconnected;
    QVector<TreeItem *> m_children;
    TreeItem *m_parent = nullptr;
};
