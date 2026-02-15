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
#include <QMessageBox>
#include <QCheckBox>
#include "ui/dialogs/CredentialManagerDialog.h"
#include "core/importexport/DevolutionsImporter.h"
#include "core/importexport/ConnectionExporter.h"
#include "app/Application.h"
#include "core/credentials/CredentialVault.h"
#include "core/connectiondb/ConnectionDatabase.h"
#include "core/userdb/UserDatabase.h"
#include "core/config/ConfigManager.h"

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
