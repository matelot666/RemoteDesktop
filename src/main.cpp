#include <QApplication>
#include <QMessageBox>
#include <QIcon>
#include <QStyleHints>
#include <QFileInfo>
#include <QTimer>
#include <QStandardPaths>
#include <libssh2.h>

#ifdef _WIN32
#include <winsock2.h>
#include <QFile>
#include <QTextStream>

static QFile *s_logFile = nullptr;

static void winMessageHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    Q_UNUSED(ctx)
    if (!s_logFile)
        return;
    QTextStream out(s_logFile);
    const char *label = "";
    switch (type) {
    case QtDebugMsg:    label = "DEBUG"; break;
    case QtInfoMsg:     label = "INFO"; break;
    case QtWarningMsg:  label = "WARN"; break;
    case QtCriticalMsg: label = "CRIT"; break;
    case QtFatalMsg:    label = "FATAL"; break;
    }
    out << label << ": " << msg << "\n";
    out.flush();
}
#endif

#include "app/Application.h"
#include "core/config/ConfigManager.h"
#include "core/credentials/CredentialVault.h"
#include "core/connectiondb/ConnectionDatabase.h"
#include "core/userdb/UserDatabase.h"
#include "core/migration/LegacyMigrator.h"
#include "ui/mainwindow/MainWindow.h"
#include "ui/treeview/ConnectionTreeModel.h"
#include "ui/treeview/ConnectionTreeView.h"
#include "ui/treeview/TreeItem.h"
#include "ui/tabview/SessionTabWidget.h"
#include "ui/dialogs/MasterPasswordDialog.h"
#include "ui/dialogs/SetupDialog.h"
#include "ui/dialogs/ConnectionDialog.h"
#include "ui/dialogs/FolderDialog.h"
#include "ui/dialogs/CredentialDialog.h"
#include "ui/sessionview/RdpSessionWidget.h"
#include "ui/sessionview/SshSessionWidget.h"

static void updateAppIcon(QApplication *app)
{
    app->setWindowIcon(QIcon(QStringLiteral(":/icons/appicon/appicon.png")));
}

static void connectSession(MainWindow *mainWindow, const QModelIndex &index)
{
    auto *model = mainWindow->treeModel();
    auto *item = model->itemFromIndex(index);
    if (!item || item->nodeType() != TreeNodeType::Connection)
        return;

    auto entry = item->connection();
    auto *tabWidget = mainWindow->tabWidget();

    // Check if already connected
    if (tabWidget->sessionForConnection(entry.id))
        return;

    // Get credentials from user DB if assigned
    QString username, password, privateKey;
    if (entry.credentialId > 0) {
        auto *vault = Application::instance()->vault();
        if (vault && vault->isUnlocked()) {
            auto *userDb = Application::instance()->userDatabase();
            auto cred = userDb->credentialById(entry.credentialId);
            cred = vault->decryptCredential(cred);
            username = cred.username;
            password = cred.password;
            privateKey = cred.privateKey;
        }
    }

    model->setConnectionState(index, ConnectionState::Connecting);

    if (entry.protocol == Protocol::RDP) {
        auto *widget = new RdpSessionWidget(entry, username, password);

        QObject::connect(widget, &RdpSessionWidget::sessionConnected, mainWindow, [=]() {
            model->setConnectionState(index, ConnectionState::Connected);
        });
        QObject::connect(widget, &RdpSessionWidget::sessionDisconnected, mainWindow, [=]() {
            model->setConnectionState(index, ConnectionState::Disconnected);
            tabWidget->removeSessionTab(entry.id);
        });
        QObject::connect(widget, &RdpSessionWidget::sessionError, mainWindow, [=](const QString &msg) {
            model->setConnectionState(index, ConnectionState::Error);
            QMessageBox::warning(mainWindow, QStringLiteral("RDP Error"), msg);
            QTimer::singleShot(2000, mainWindow, [=]() {
                tabWidget->removeSessionTab(entry.id);
                model->setConnectionState(index, ConnectionState::Disconnected);
                mainWindow->updateStatusBar();
            });
        });

        tabWidget->addSessionTab(widget, entry.name, entry.id);
        widget->connectSession();

    } else {
        auto *widget = new SshSessionWidget(entry, username, password, privateKey);

        QObject::connect(widget, &SshSessionWidget::sessionConnected, mainWindow, [=]() {
            model->setConnectionState(index, ConnectionState::Connected);
        });
        QObject::connect(widget, &SshSessionWidget::sessionDisconnected, mainWindow, [=]() {
            model->setConnectionState(index, ConnectionState::Disconnected);
            tabWidget->removeSessionTab(entry.id);
        });
        QObject::connect(widget, &SshSessionWidget::sessionError, mainWindow, [=](const QString &msg) {
            model->setConnectionState(index, ConnectionState::Error);
            QMessageBox::warning(mainWindow, QStringLiteral("SSH Error"), msg);
            QTimer::singleShot(2000, mainWindow, [=]() {
                tabWidget->removeSessionTab(entry.id);
                model->setConnectionState(index, ConnectionState::Disconnected);
                mainWindow->updateStatusBar();
            });
        });

        tabWidget->addSessionTab(widget, entry.name, entry.id);
        widget->connectSession();
    }

    mainWindow->updateStatusBar();
}

// Recursively collect all connection model indices under a tree item
static void collectConnectionIndices(ConnectionTreeModel *model, TreeItem *item,
                                      QVector<QModelIndex> &out)
{
    for (int i = 0; i < item->childCount(); ++i) {
        auto *child = item->child(i);
        if (child->nodeType() == TreeNodeType::Connection) {
            out.append(model->indexFromItem(child));
        } else if (child->nodeType() == TreeNodeType::Folder) {
            collectConnectionIndices(model, child, out);
        }
    }
}

static void disconnectSession(MainWindow *mainWindow, const QModelIndex &index)
{
    auto *model = mainWindow->treeModel();
    auto *item = model->itemFromIndex(index);
    if (!item || item->nodeType() != TreeNodeType::Connection)
        return;

    auto entry = item->connection();
    auto *tabWidget = mainWindow->tabWidget();

    auto *widget = tabWidget->sessionForConnection(entry.id);
    if (!widget)
        return;

    widget->disconnect();

    if (auto *rdp = qobject_cast<RdpSessionWidget *>(widget)) {
        rdp->disconnectSession();
    } else if (auto *ssh = qobject_cast<SshSessionWidget *>(widget)) {
        ssh->disconnectSession();
    }

    model->setConnectionState(index, ConnectionState::Disconnected);
    tabWidget->removeSessionTab(entry.id);
    mainWindow->updateStatusBar();
}

int main(int argc, char *argv[])
{
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    QApplication app(argc, argv);

#ifdef _WIN32
    // Log qDebug/qWarning to a file since Windows GUI apps have no console
    s_logFile = new QFile(QStringLiteral("C:/remotedesktop/debug.log"));
    if (s_logFile->open(QIODevice::WriteOnly | QIODevice::Truncate))
        qInstallMessageHandler(winMessageHandler);
#endif
    app.setApplicationName(QStringLiteral("Remote Desktop Manager"));
    app.setOrganizationName(QStringLiteral("RemoteDesktop"));

    // Set application icon (theme-aware)
    updateAppIcon(&app);
    QObject::connect(app.styleHints(), &QStyleHints::colorSchemeChanged,
                     &app, [&]() { updateAppIcon(&app); });

    // Initialize libssh2
    libssh2_init(0);

    auto *appInstance = Application::instance();
    QString configDir = appInstance->configDir();

    // Step 1: Parse CLI overrides early (applied inside init())
    // Step 2: Check if config.ini exists
    {
        ConfigManager tempConfig;
        tempConfig.load(configDir);

        if (!tempConfig.exists()) {
            // Check for legacy connections.db
            QString legacyPath = configDir + QStringLiteral("/connections.db");
            bool hasLegacy = LegacyMigrator::isLegacyDatabase(legacyPath);

            if (hasLegacy) {
                // Offer migration
                auto answer = QMessageBox::question(
                    nullptr, QStringLiteral("Upgrade Required"),
                    QStringLiteral("An existing connections database was found.\n\n"
                                   "Would you like to migrate to the new multi-user format?\n"
                                   "(Your credentials will be moved to a private per-user database)"),
                    QMessageBox::Yes | QMessageBox::No);

                if (answer == QMessageBox::Yes) {
                    // Ask if admin
                    auto adminAnswer = QMessageBox::question(
                        nullptr, QStringLiteral("Admin Setup"),
                        QStringLiteral("Are you the administrator who manages the shared connection tree?"),
                        QMessageBox::Yes | QMessageBox::No);

                    bool isAdmin = (adminAnswer == QMessageBox::Yes);

                    // Set config: use legacy path as shared DB path for admin
                    tempConfig.setSharedDatabasePath(isAdmin ? legacyPath : QString());
                    tempConfig.setAdmin(isAdmin);
                    tempConfig.save();

                    if (isAdmin) {
                        // Admin migration: init Application, then migrate
                        if (!appInstance->init(argc, argv)) {
                            QMessageBox::critical(nullptr, QStringLiteral("Error"),
                                QStringLiteral("Failed to initialize application."));
                            return 1;
                        }
                        auto migResult = LegacyMigrator::migrateAdmin(
                            legacyPath, appInstance->database(), appInstance->userDatabase());
                        if (!migResult.success) {
                            QMessageBox::warning(nullptr, QStringLiteral("Migration"),
                                QStringLiteral("Migration completed with warnings:\n") +
                                migResult.warnings.join(QStringLiteral("\n")));
                        } else {
                            QMessageBox::information(nullptr, QStringLiteral("Migration Complete"),
                                QStringLiteral("Migrated %1 credential(s), %2 assignment(s), %3 folder default(s).")
                                    .arg(migResult.credentialsMigrated)
                                    .arg(migResult.assignmentsMigrated)
                                    .arg(migResult.folderDefaultsMigrated));
                        }
                    } else {
                        // Non-admin: need shared DB path
                        SetupDialog setupDlg;
                        if (setupDlg.exec() != QDialog::Accepted)
                            return 0;

                        tempConfig.setSharedDatabasePath(setupDlg.sharedDatabasePath());
                        tempConfig.save();

                        if (!appInstance->init(argc, argv)) {
                            QMessageBox::critical(nullptr, QStringLiteral("Error"),
                                QStringLiteral("Failed to initialize application."));
                            return 1;
                        }

                        auto migResult = LegacyMigrator::migrateNonAdmin(
                            legacyPath, appInstance->userDatabase());
                        if (migResult.credentialsMigrated > 0) {
                            QMessageBox::information(nullptr, QStringLiteral("Migration Complete"),
                                QStringLiteral("Migrated %1 credential(s) to your private database.")
                                    .arg(migResult.credentialsMigrated));
                        }
                    }
                } else {
                    // User declined migration, show setup dialog
                    SetupDialog setupDlg;
                    if (setupDlg.exec() != QDialog::Accepted)
                        return 0;

                    tempConfig.setSharedDatabasePath(setupDlg.sharedDatabasePath());
                    tempConfig.setAdmin(setupDlg.isAdmin());
                    tempConfig.save();

                    if (!appInstance->init(argc, argv)) {
                        QMessageBox::critical(nullptr, QStringLiteral("Error"),
                            QStringLiteral("Failed to initialize application."));
                        return 1;
                    }
                }
            } else {
                // No legacy DB, fresh setup
                SetupDialog setupDlg;
                if (setupDlg.exec() != QDialog::Accepted)
                    return 0;

                tempConfig.setSharedDatabasePath(setupDlg.sharedDatabasePath());
                tempConfig.setAdmin(setupDlg.isAdmin());
                tempConfig.save();

                if (!appInstance->init(argc, argv)) {
                    QMessageBox::critical(nullptr, QStringLiteral("Error"),
                        QStringLiteral("Failed to initialize application."));
                    return 1;
                }
            }
        } else {
            // Config exists, normal startup
            if (!appInstance->init(argc, argv)) {
                QMessageBox::critical(nullptr, QStringLiteral("Error"),
                    QStringLiteral("Failed to initialize application database."));
                return 1;
            }
        }
    }

    // Master password handling (vault uses user DB)
    auto *vault = appInstance->vault();
    if (vault->hasBeenSetup()) {
        MasterPasswordDialog dlg(MasterPasswordDialog::Unlock);
        if (dlg.exec() != QDialog::Accepted) {
            return 0;
        }
        if (!vault->unlock(dlg.password())) {
            QMessageBox::critical(nullptr, QStringLiteral("Error"),
                                  QStringLiteral("Incorrect master password."));
            return 1;
        }
    } else {
        MasterPasswordDialog dlg(MasterPasswordDialog::Setup);
        if (dlg.exec() != QDialog::Accepted) {
            return 0;
        }
        if (!vault->setupMasterPassword(dlg.password())) {
            QMessageBox::critical(nullptr, QStringLiteral("Error"),
                                  QStringLiteral("Failed to set up credential vault."));
            return 1;
        }
    }

    MainWindow mainWindow;
    mainWindow.treeModel()->loadFromDatabase();
    mainWindow.show();

    auto *treeView = mainWindow.treeView();
    auto *treeModel = mainWindow.treeModel();
    auto *tabWidget = mainWindow.tabWidget();
    auto *db = appInstance->database();
    auto *userDb = appInstance->userDatabase();
    bool admin = appInstance->isAdmin();

    // Single-click on a connected server switches to its tab
    QObject::connect(treeView, &QTreeView::clicked,
                     &mainWindow, [&](const QModelIndex &index) {
        auto *item = treeModel->itemFromIndex(index);
        if (!item || item->nodeType() != TreeNodeType::Connection)
            return;
        auto *widget = tabWidget->sessionForConnection(item->connection().id);
        if (widget)
            tabWidget->setCurrentWidget(widget);
    });

    // Connect tree signals
    QObject::connect(treeView, &ConnectionTreeView::connectRequested,
                     &mainWindow, [&](const QModelIndex &index) {
        connectSession(&mainWindow, index);
    });

    QObject::connect(treeView, &ConnectionTreeView::connectAllRequested,
                     &mainWindow, [&](const QModelIndex &folderIndex) {
        auto *item = treeModel->itemFromIndex(folderIndex);
        if (!item || item->nodeType() != TreeNodeType::Folder)
            return;

        QVector<QModelIndex> indices;
        collectConnectionIndices(treeModel, item, indices);

        // Connect each server with a 500ms stagger to avoid overwhelming
        for (int i = 0; i < indices.size(); ++i) {
            QTimer::singleShot(i * 3000, &mainWindow, [&mainWindow, idx = indices[i]]() {
                connectSession(&mainWindow, idx);
            });
        }
    });

    QObject::connect(treeView, &ConnectionTreeView::disconnectAllRequested,
                     &mainWindow, [&](const QModelIndex &folderIndex) {
        auto *item = treeModel->itemFromIndex(folderIndex);
        if (!item || item->nodeType() != TreeNodeType::Folder)
            return;

        QVector<QModelIndex> indices;
        collectConnectionIndices(treeModel, item, indices);

        for (const auto &idx : indices)
            disconnectSession(&mainWindow, idx);
    });

    QObject::connect(treeView, &ConnectionTreeView::disconnectRequested,
                     &mainWindow, [&](const QModelIndex &index) {
        disconnectSession(&mainWindow, index);
    });

    QObject::connect(treeView, &ConnectionTreeView::reconnectRequested,
                     &mainWindow, [&](const QModelIndex &index) {
        disconnectSession(&mainWindow, index);
        connectSession(&mainWindow, index);
    });

    QObject::connect(treeView, &ConnectionTreeView::sortByNameRequested,
                     &mainWindow, [&](const QModelIndex &index) {
        if (!admin)
            return;
        qint64 folderId = -1;
        if (index.isValid()) {
            auto *item = treeModel->itemFromIndex(index);
            if (item && item->nodeType() == TreeNodeType::Folder)
                folderId = item->folder().id;
        }
        db->sortChildrenByName(folderId);
        treeModel->loadFromDatabase();
    });

    QObject::connect(treeView, &ConnectionTreeView::addFolderRequested,
                     &mainWindow, [&](const QModelIndex &parentIndex) {
        if (!admin)
            return;
        FolderDialog dlg(&mainWindow);
        dlg.setWindowTitle(QStringLiteral("Add Folder"));
        if (dlg.exec() != QDialog::Accepted)
            return;

        ConnectionFolder folder = dlg.folder();
        if (folder.name.isEmpty())
            return;

        if (parentIndex.isValid()) {
            auto *parentItem = treeModel->itemFromIndex(parentIndex);
            if (parentItem && parentItem->nodeType() == TreeNodeType::Folder) {
                folder.parentId = parentItem->folder().id;
            }
        }

        folder.id = db->insertFolder(folder);
        if (folder.id > 0) {
            // Write folder credential defaults to user DB
            if (folder.defaultRdpCredentialId > 0 || folder.defaultSshCredentialId > 0) {
                userDb->setFolderDefaults(folder.id,
                                           folder.defaultRdpCredentialId,
                                           folder.defaultSshCredentialId);
            }
            treeModel->addFolder(folder, parentIndex);
        }
    });

    QObject::connect(treeView, &ConnectionTreeView::addConnectionRequested,
                     &mainWindow, [&](const QModelIndex &parentIndex) {
        if (!admin)
            return;
        ConnectionDialog dlg(&mainWindow);
        dlg.setWindowTitle(QStringLiteral("Add Connection"));
        if (dlg.exec() != QDialog::Accepted)
            return;

        ConnectionEntry entry = dlg.connection();
        if (entry.name.isEmpty() || entry.hostname.isEmpty())
            return;

        if (parentIndex.isValid()) {
            auto *parentItem = treeModel->itemFromIndex(parentIndex);
            if (parentItem && parentItem->nodeType() == TreeNodeType::Folder) {
                entry.folderId = parentItem->folder().id;
            }
        }

        qint64 credentialId = entry.credentialId;
        entry.credentialId = -1; // shared DB has no credential_id

        entry.id = db->insertConnection(entry);
        if (entry.id > 0) {
            // Write credential assignment to user DB
            if (credentialId > 0)
                userDb->setCredentialAssignment(entry.id, credentialId);
            entry.credentialId = credentialId; // restore for tree display
            treeModel->addConnection(entry, parentIndex);
        }
    });

    QObject::connect(treeView, &ConnectionTreeView::editRequested,
                     &mainWindow, [&](const QModelIndex &index) {
        auto *item = treeModel->itemFromIndex(index);
        if (!item)
            return;

        if (item->nodeType() == TreeNodeType::Folder) {
            FolderDialog dlg(&mainWindow);
            dlg.setWindowTitle(QStringLiteral("Edit Folder"));
            dlg.setFolder(item->folder());
            if (!admin)
                dlg.setReadOnlyName(true);
            if (dlg.exec() != QDialog::Accepted)
                return;

            ConnectionFolder folder = dlg.folder();
            folder.id = item->folder().id;
            folder.parentId = item->folder().parentId;

            // Admin updates shared fields
            if (admin)
                db->updateFolder(folder);

            // Everyone updates their own folder defaults in user DB
            userDb->setFolderDefaults(folder.id,
                                       folder.defaultRdpCredentialId,
                                       folder.defaultSshCredentialId);

            if (dlg.forceInheritance()) {
                userDb->propagateCredentials(folder.id,
                                              folder.defaultRdpCredentialId,
                                              folder.defaultSshCredentialId,
                                              db);
                treeModel->loadFromDatabase();
            } else {
                item->folder() = folder;
                emit treeModel->dataChanged(index, index);
            }

        } else {
            ConnectionDialog dlg(&mainWindow);
            dlg.setWindowTitle(QStringLiteral("Edit Connection"));
            dlg.setConnection(item->connection());
            if (!admin)
                dlg.setReadOnlySharedFields(true);
            if (dlg.exec() != QDialog::Accepted)
                return;

            ConnectionEntry entry = dlg.connection();
            entry.id = item->connection().id;
            entry.folderId = item->connection().folderId;

            // Admin updates shared fields
            if (admin) {
                ConnectionEntry sharedEntry = entry;
                sharedEntry.credentialId = -1; // shared DB has no credential_id
                db->updateConnection(sharedEntry);
            }

            // Everyone updates their own credential assignment in user DB
            if (entry.credentialId > 0)
                userDb->setCredentialAssignment(entry.id, entry.credentialId);
            else
                userDb->removeCredentialAssignment(entry.id);

            item->connection() = entry;
            emit treeModel->dataChanged(index, index);
        }
    });

    QObject::connect(treeView, &ConnectionTreeView::deleteRequested,
                     &mainWindow, [&](const QModelIndex &index) {
        if (!admin)
            return;
        auto *item = treeModel->itemFromIndex(index);
        if (!item)
            return;

        QString typeStr = item->nodeType() == TreeNodeType::Folder
            ? QStringLiteral("folder") : QStringLiteral("connection");

        auto result = QMessageBox::question(&mainWindow, QStringLiteral("Delete"),
            QStringLiteral("Delete %1 \"%2\"?").arg(typeStr, item->name()));
        if (result != QMessageBox::Yes)
            return;

        if (item->nodeType() == TreeNodeType::Folder) {
            db->deleteFolder(item->folder().id);
        } else {
            auto *widget = tabWidget->sessionForConnection(item->connection().id);
            if (widget) {
                disconnectSession(&mainWindow, index);
            }
            qint64 connId = item->connection().id;
            db->deleteConnection(connId);
            userDb->removeCredentialAssignment(connId);
        }
        treeModel->removeItem(index);
    });

    // Tab close -> disconnect
    QObject::connect(tabWidget, &SessionTabWidget::tabCloseRequested,
                     &mainWindow, [&](qint64 connectionId) {
        auto *widget = tabWidget->sessionForConnection(connectionId);
        if (widget)
            widget->disconnect();
        if (auto *rdp = qobject_cast<RdpSessionWidget *>(widget)) {
            rdp->disconnectSession();
        } else if (auto *ssh = qobject_cast<SshSessionWidget *>(widget)) {
            ssh->disconnectSession();
        }
        tabWidget->removeSessionTab(connectionId);
        mainWindow.updateStatusBar();
    });

    int result = app.exec();

    libssh2_exit();
    return result;
}
