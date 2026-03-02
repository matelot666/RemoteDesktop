#pragma once

#include <QTreeView>
#include <QSet>

class QTimer;
class ConnectionTreeModel;
class TreeItem;

class ConnectionTreeView : public QTreeView {
    Q_OBJECT
public:
    explicit ConnectionTreeView(QWidget *parent = nullptr);

    void setModel(QAbstractItemModel *model) override;

signals:
    void connectRequested(const QModelIndex &index);
    void connectAllRequested(const QModelIndex &folderIndex);
    void disconnectAllRequested(const QModelIndex &folderIndex);
    void disconnectRequested(const QModelIndex &index);
    void reconnectRequested(const QModelIndex &index);
    void editRequested(const QModelIndex &index);
    void deleteRequested(const QModelIndex &index);
    void addFolderRequested(const QModelIndex &parentIndex);
    void addConnectionRequested(const QModelIndex &parentIndex);
    void sortByNameRequested(const QModelIndex &parentIndex);

protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void moveItemInOrder(const QModelIndex &index, int direction);
    void indentFolder(const QModelIndex &index);
    void outdentFolder(const QModelIndex &index);
    int countConnectionsUnder(TreeItem *item) const;
    int countConnectedUnder(TreeItem *item) const;
    void flashRow(const QModelIndex &index);

    void collectExpandedIds(const QModelIndex &parent, QSet<qint64> &ids) const;
    void restoreExpandedIds(const QModelIndex &parent, const QSet<qint64> &ids);

public:
    QSet<qint64> saveExpandedFolderIds() const;
    void restoreExpandedFolderIds(const QSet<qint64> &ids);

    ConnectionTreeModel *m_model = nullptr;
    QPersistentModelIndex m_flashIndex;
    int m_flashStep = 0;
    QTimer *m_flashTimer = nullptr;
};
