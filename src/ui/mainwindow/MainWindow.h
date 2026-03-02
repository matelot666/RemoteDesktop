#pragma once

#include <QMainWindow>
#include <QSplitter>
#include <QLabel>
#include <QLineEdit>
#include <QDateTime>

class QTimer;
class ConnectionTreeView;
class ConnectionTreeModel;
class SessionTabWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    ConnectionTreeModel *treeModel() const { return m_treeModel; }
    ConnectionTreeView *treeView() const { return m_treeView; }
    SessionTabWidget *tabWidget() const { return m_tabWidget; }

    void updateStatusBar();
    void refreshTree();

private:
    void setupUi();
    void setupMenuBar();
    void setupShortcuts();
    void toggleLeftPanel();
    void quickConnect();
    void checkSharedDbChanged();
    void filterTree(const QString &text);
    bool filterTreeRecursive(const QModelIndex &parent, const QString &text);
    void clearTreeFilter(const QModelIndex &parent);

    static qint64 s_nextQuickConnectId;

    QSplitter *m_splitter = nullptr;
    QWidget *m_leftPanel = nullptr;
    QLineEdit *m_searchBox = nullptr;
    ConnectionTreeView *m_treeView = nullptr;
    ConnectionTreeModel *m_treeModel = nullptr;
    SessionTabWidget *m_tabWidget = nullptr;
    QLabel *m_dbPathLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    bool m_leftPanelVisible = true;
    QTimer *m_dbPollTimer = nullptr;
    QDateTime m_lastDbModified;
};
