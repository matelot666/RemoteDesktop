#include "MainWindow.h"
#include "ui/treeview/ConnectionTreeView.h"
#include "ui/treeview/ConnectionTreeModel.h"
#include "ui/tabview/SessionTabWidget.h"

#include <QMenuBar>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QShortcut>
#include <QKeySequence>
#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QCheckBox>
#include <QTimer>
#include "ui/dialogs/CredentialManagerDialog.h"
#include "ui/dialogs/SessionInfoDialog.h"
#include "ui/dialogs/QuickConnectDialog.h"
#include "ui/sessionview/RdpSessionWidget.h"
#include "ui/sessionview/SshSessionWidget.h"
#include "core/importexport/DevolutionsImporter.h"
#include "core/importexport/ConnectionExporter.h"
#include "app/Application.h"
#include "core/credentials/CredentialVault.h"
#include "core/connectiondb/ConnectionDatabase.h"
#include "core/userdb/UserDatabase.h"
#include "core/config/ConfigManager.h"

qint64 MainWindow::s_nextQuickConnectId = -1000;

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();
    setupMenuBar();
    setupShortcuts();

    QString title = QStringLiteral("Remote Desktop Manager");
    if (Application::instance()->isAdmin())
        title += QStringLiteral(" (Admin)");
    setWindowTitle(title);
    resize(1400, 900);

    // Poll shared DB for external changes every 30 seconds
    QString dbPath = Application::instance()->databasePath();
    if (!dbPath.isEmpty()) {
        m_lastDbModified = QFileInfo(dbPath).lastModified();
        m_dbPollTimer = new QTimer(this);
        m_dbPollTimer->setInterval(30000);
        connect(m_dbPollTimer, &QTimer::timeout, this, &MainWindow::checkSharedDbChanged);
        m_dbPollTimer->start();
    }
}

void MainWindow::setupUi()
{
    m_splitter = new QSplitter(Qt::Horizontal, this);
    setCentralWidget(m_splitter);

    // Left panel: search + tree
    m_leftPanel = new QWidget;
    auto *leftLayout = new QVBoxLayout(m_leftPanel);
    leftLayout->setContentsMargins(0, 4, 0, 0);
    leftLayout->setSpacing(4);

    m_searchBox = new QLineEdit;
    m_searchBox->setPlaceholderText(QStringLiteral("Search..."));
    m_searchBox->setClearButtonEnabled(true);
    leftLayout->addWidget(m_searchBox);

    connect(m_searchBox, &QLineEdit::textChanged, this, &MainWindow::filterTree);

    m_treeModel = new ConnectionTreeModel(this);
    m_treeView = new ConnectionTreeView;
    m_treeView->setModel(m_treeModel);
    leftLayout->addWidget(m_treeView);

    m_splitter->addWidget(m_leftPanel);

    // Right panel: tabs
    m_tabWidget = new SessionTabWidget;
    m_splitter->addWidget(m_tabWidget);

    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({280, 1120});

    // Status bar
    auto *app = Application::instance();
    QString dbInfo = QStringLiteral("Shared: %1  |  User: %2  |  %3")
                         .arg(app->databasePath(),
                              app->userDatabasePath(),
                              app->isAdmin() ? QStringLiteral("Admin") : QStringLiteral("Read-Only"));
    m_dbPathLabel = new QLabel(dbInfo);
    m_dbPathLabel->setStyleSheet(QStringLiteral("color: gray;"));
    statusBar()->addWidget(m_dbPathLabel, 1);

    m_statusLabel = new QLabel(QStringLiteral("Ready"));
    statusBar()->addPermanentWidget(m_statusLabel);
    updateStatusBar();

    connect(m_tabWidget, &SessionTabWidget::sessionCountChanged,
            this, &MainWindow::updateStatusBar);
}

void MainWindow::setupMenuBar()
{
    bool admin = Application::instance()->isAdmin();

    auto *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(QStringLiteral("&Quick Connect..."), this, &MainWindow::quickConnect,
                        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Q));
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("Manage &Credentials..."), this, [this]() {
        CredentialManagerDialog dlg(this);
        dlg.exec();
    });

    if (admin) {
        fileMenu->addSeparator();
        fileMenu->addAction(QStringLiteral("Import from Devolutions RDM..."), this, [this]() {
            QString filePath = QFileDialog::getOpenFileName(
                this, QStringLiteral("Import from Devolutions RDM"),
                QString(),
                QStringLiteral("Devolutions RDM (*.json *.rdm);;All Files (*)"));
            if (filePath.isEmpty())
                return;

            auto *app = Application::instance();
            auto result = DevolutionsImporter::importFromFile(
                filePath, app->database(), app->userDatabase(), app->vault());

            QString msg = QStringLiteral("Imported %1 connection(s), created %2 folder(s).")
                              .arg(result.connectionsImported)
                              .arg(result.foldersCreated);
            if (result.skipped > 0)
                msg += QStringLiteral("\nSkipped %1 unsupported entry(ies).").arg(result.skipped);
            if (!result.warnings.isEmpty())
                msg += QStringLiteral("\n\nWarnings:\n") + result.warnings.join(QStringLiteral("\n"));

            QMessageBox::information(this, QStringLiteral("Import Complete"), msg);
            m_treeModel->loadFromDatabase();
        });
        fileMenu->addAction(QStringLiteral("Export to Devolutions RDM..."), this, [this]() {
            QString filePath = QFileDialog::getSaveFileName(
                this, QStringLiteral("Export to Devolutions RDM"),
                QStringLiteral("connections.json"),
                QStringLiteral("Devolutions RDM JSON (*.json);;All Files (*)"));
            if (filePath.isEmpty())
                return;

            bool includeCreds = false;
            auto *vault = Application::instance()->vault();
            if (vault && vault->isUnlocked()) {
                auto answer = QMessageBox::question(
                    this, QStringLiteral("Include Credentials"),
                    QStringLiteral("Include decrypted credentials in the export?\n\n"
                                   "Warning: Passwords will be stored in plain text."),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                includeCreds = (answer == QMessageBox::Yes);
            }

            bool ok = ConnectionExporter::exportToFile(
                filePath, Application::instance()->database(),
                Application::instance()->userDatabase(), vault, includeCreds);

            if (ok) {
                QMessageBox::information(this, QStringLiteral("Export Complete"),
                                         QStringLiteral("Connections exported successfully."));
            } else {
                QMessageBox::warning(this, QStringLiteral("Export Failed"),
                                     QStringLiteral("Failed to write export file."));
            }
        });
    }

    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("&Quit"), qApp, &QApplication::quit,
                        QKeySequence::Quit);

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    viewMenu->addAction(QStringLiteral("&Refresh"), this, &MainWindow::refreshTree,
                        QKeySequence(Qt::Key_F5));
    viewMenu->addSeparator();
    viewMenu->addAction(QStringLiteral("Toggle &Sidebar"),
                        this, &MainWindow::toggleLeftPanel,
                        QKeySequence(Qt::CTRL | Qt::Key_B));

    // ── Options menu: RDP performance toggles ──
    auto *cfg = Application::instance()->config();
    auto *optionsMenu = menuBar()->addMenu(QStringLiteral("&Options"));
    auto *rdpMenu = optionsMenu->addMenu(QStringLiteral("RDP Performance"));

    auto *actRemoteFx = rdpMenu->addAction(QStringLiteral("RemoteFX Codec"));
    actRemoteFx->setCheckable(true);
    actRemoteFx->setChecked(cfg->rdpRemoteFx());
    connect(actRemoteFx, &QAction::toggled, this, [cfg](bool checked) {
        cfg->setRdpRemoteFx(checked);
        cfg->save();
    });

    rdpMenu->addSeparator();

    auto *actGfx = rdpMenu->addAction(QStringLiteral("GFX Graphics Pipeline"));
    actGfx->setCheckable(true);
    actGfx->setChecked(cfg->rdpGfxPipeline());
    actGfx->setToolTip(QStringLiteral("Experimental — may break connections if server doesn't support it"));
    connect(actGfx, &QAction::toggled, this, [cfg](bool checked) {
        cfg->setRdpGfxPipeline(checked);
        cfg->save();
    });

    auto *actH264 = rdpMenu->addAction(QStringLiteral("H.264/AVC (requires GFX)"));
    actH264->setCheckable(true);
    actH264->setChecked(cfg->rdpH264());
    actH264->setEnabled(cfg->rdpGfxPipeline());
    connect(actH264, &QAction::toggled, this, [cfg](bool checked) {
        cfg->setRdpH264(checked);
        cfg->save();
    });

    // H.264 only makes sense when GFX is on
    connect(actGfx, &QAction::toggled, actH264, [actH264](bool gfxOn) {
        actH264->setEnabled(gfxOn);
        if (!gfxOn)
            actH264->setChecked(false);
    });

    rdpMenu->addSeparator();

    auto *actFontSmoothing = rdpMenu->addAction(QStringLiteral("Font Smoothing"));
    actFontSmoothing->setCheckable(true);
    actFontSmoothing->setChecked(cfg->rdpFontSmoothing());
    connect(actFontSmoothing, &QAction::toggled, this, [cfg](bool checked) {
        cfg->setRdpFontSmoothing(checked);
        cfg->save();
    });

    rdpMenu->addSeparator();

    auto *actVerboseLog = rdpMenu->addAction(QStringLiteral("Verbose FreeRDP Logging"));
    actVerboseLog->setCheckable(true);
    actVerboseLog->setChecked(cfg->rdpVerboseLog());
    actVerboseLog->setToolTip(QStringLiteral("Enable detailed FreeRDP protocol logging to stderr (for debugging)"));
    connect(actVerboseLog, &QAction::toggled, this, [cfg](bool checked) {
        cfg->setRdpVerboseLog(checked);
        cfg->save();
    });

    // ── Help menu ──
    auto *helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    helpMenu->addAction(QStringLiteral("Session Info..."), this, [this]() {
        QWidget *current = m_tabWidget->currentWidget();
        auto *rdpWidget = qobject_cast<RdpSessionWidget *>(current);
        if (!rdpWidget || !rdpWidget->rdpSession()) {
            QMessageBox::information(this, QStringLiteral("Session Info"),
                                     QStringLiteral("No active RDP session selected."));
            return;
        }
        SessionInfoDialog dlg(rdpWidget->rdpSession(), this);
        dlg.exec();
    });
}

void MainWindow::setupShortcuts()
{
    // Ctrl+W (Win) / Cmd+W (Mac) -- close current tab
    auto *closeTab = new QShortcut(QKeySequence::Close, this);
    connect(closeTab, &QShortcut::activated, this, [this]() {
        int idx = m_tabWidget->currentIndex();
        if (idx >= 0)
            m_tabWidget->tabCloseRequested(idx);
    });

    // Ctrl+Shift+] -- next tab
    auto *nextTab = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketRight), this);
    connect(nextTab, &QShortcut::activated, this, [this]() {
        int count = m_tabWidget->count();
        if (count > 1) {
            m_tabWidget->setCurrentIndex((m_tabWidget->currentIndex() + 1) % count);
        }
    });

    // Ctrl+Shift+[ -- previous tab
    auto *prevTab = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketLeft), this);
    connect(prevTab, &QShortcut::activated, this, [this]() {
        int count = m_tabWidget->count();
        if (count > 1) {
            m_tabWidget->setCurrentIndex((m_tabWidget->currentIndex() - 1 + count) % count);
        }
    });
}

void MainWindow::toggleLeftPanel()
{
    m_leftPanelVisible = !m_leftPanelVisible;
    m_leftPanel->setVisible(m_leftPanelVisible);
}

void MainWindow::quickConnect()
{
    QuickConnectDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    ConnectionEntry entry = dlg.connectionEntry();
    qint64 quickId = s_nextQuickConnectId--;
    entry.id = quickId;

    QString username = dlg.username();
    QString password = dlg.password();
    QString privateKey = dlg.privateKey();

    // Tab label: hostname, or hostname:port if non-default
    QString tabLabel = entry.hostname;
    if (entry.port != entry.defaultPort())
        tabLabel += QStringLiteral(":%1").arg(entry.port);

    if (entry.protocol == Protocol::RDP) {
        auto *widget = new RdpSessionWidget(entry, username, password);

        connect(widget, &RdpSessionWidget::sessionDisconnected, this, [this, quickId]() {
            m_tabWidget->removeSessionTab(quickId);
            updateStatusBar();
        });
        connect(widget, &RdpSessionWidget::sessionError, this, [this, quickId](const QString &msg) {
            QMessageBox::warning(this, QStringLiteral("RDP Error"), msg);
            QTimer::singleShot(2000, this, [this, quickId]() {
                m_tabWidget->removeSessionTab(quickId);
                updateStatusBar();
            });
        });

        m_tabWidget->addSessionTab(widget, tabLabel, quickId);
        widget->connectSession();
    } else {
        auto *widget = new SshSessionWidget(entry, username, password, privateKey);

        connect(widget, &SshSessionWidget::sessionDisconnected, this, [this, quickId]() {
            m_tabWidget->removeSessionTab(quickId);
            updateStatusBar();
        });
        connect(widget, &SshSessionWidget::sessionError, this, [this, quickId](const QString &msg) {
            QMessageBox::warning(this, QStringLiteral("SSH Error"), msg);
            QTimer::singleShot(2000, this, [this, quickId]() {
                m_tabWidget->removeSessionTab(quickId);
                updateStatusBar();
            });
        });

        m_tabWidget->addSessionTab(widget, tabLabel, quickId);
        widget->connectSession();
    }

    updateStatusBar();
}

void MainWindow::updateStatusBar()
{
    int count = m_tabWidget->activeSessionCount();
    if (count == 0) {
        m_statusLabel->setText(QStringLiteral("Ready"));
    } else {
        m_statusLabel->setText(QStringLiteral("Connected: %1 session%2")
                                   .arg(count)
                                   .arg(count != 1 ? "s" : ""));
    }
}

void MainWindow::refreshTree()
{
    auto expanded = m_treeView->saveExpandedFolderIds();
    m_treeModel->loadFromDatabase();
    m_treeView->restoreExpandedFolderIds(expanded);
    // Update last-modified so the poll timer doesn't immediately reload again
    QString dbPath = Application::instance()->databasePath();
    if (!dbPath.isEmpty())
        m_lastDbModified = QFileInfo(dbPath).lastModified();
    // Re-apply search filter if active
    if (!m_searchBox->text().isEmpty())
        filterTree(m_searchBox->text());
}

void MainWindow::checkSharedDbChanged()
{
    QString dbPath = Application::instance()->databasePath();
    if (dbPath.isEmpty())
        return;
    QDateTime mod = QFileInfo(dbPath).lastModified();
    if (mod != m_lastDbModified) {
        m_lastDbModified = mod;
        auto expanded = m_treeView->saveExpandedFolderIds();
        m_treeModel->loadFromDatabase();
        m_treeView->restoreExpandedFolderIds(expanded);
        // Re-apply search filter if active
        if (!m_searchBox->text().isEmpty())
            filterTree(m_searchBox->text());
    }
}

void MainWindow::filterTree(const QString &text)
{
    if (text.isEmpty()) {
        clearTreeFilter(QModelIndex());
        return;
    }

    filterTreeRecursive(QModelIndex(), text);

    // Expand all visible folders so matches are visible
    std::function<void(const QModelIndex &)> expandVisible =
        [&](const QModelIndex &parent) {
            int rows = m_treeModel->rowCount(parent);
            for (int i = 0; i < rows; ++i) {
                QModelIndex idx = m_treeModel->index(i, 0, parent);
                if (!m_treeView->isRowHidden(i, parent)) {
                    auto *item = m_treeModel->itemFromIndex(idx);
                    if (item && item->nodeType() == TreeNodeType::Folder) {
                        m_treeView->setExpanded(idx, true);
                        expandVisible(idx);
                    }
                }
            }
        };
    expandVisible(QModelIndex());
}

bool MainWindow::filterTreeRecursive(const QModelIndex &parent, const QString &text)
{
    int rows = m_treeModel->rowCount(parent);
    bool anyChildVisible = false;

    for (int i = 0; i < rows; ++i) {
        QModelIndex idx = m_treeModel->index(i, 0, parent);
        auto *item = m_treeModel->itemFromIndex(idx);
        if (!item) {
            m_treeView->setRowHidden(i, parent, true);
            continue;
        }

        bool nameMatches = item->name().contains(text, Qt::CaseInsensitive);
        bool descendantMatches = false;

        if (item->nodeType() == TreeNodeType::Folder)
            descendantMatches = filterTreeRecursive(idx, text);

        bool visible = nameMatches || descendantMatches;
        m_treeView->setRowHidden(i, parent, !visible);

        if (visible)
            anyChildVisible = true;
    }

    return anyChildVisible;
}

void MainWindow::clearTreeFilter(const QModelIndex &parent)
{
    int rows = m_treeModel->rowCount(parent);
    for (int i = 0; i < rows; ++i) {
        m_treeView->setRowHidden(i, parent, false);
        QModelIndex idx = m_treeModel->index(i, 0, parent);
        auto *item = m_treeModel->itemFromIndex(idx);
        if (item && item->nodeType() == TreeNodeType::Folder)
            clearTreeFilter(idx);
    }
}
