#include <QApplication>
#include <QMessageBox>
#include <QIcon>
#include <QStyleHints>
#include <QFileInfo>
#include <QTimer>
#include <QStandardPaths>
#include <QProgressDialog>
#include <QtConcurrent>
#include <libssh2.h>
#include <openssl/provider.h>
#include <openssl/rand.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <signal.h>
#include <unistd.h>
#include <execinfo.h>
#include <sys/resource.h>
#endif

#include <QFile>
#include <QTextStream>
#include <QDateTime>

static QFile *s_logFile = nullptr;

#ifndef _WIN32
// Crash signal handler — logs stack trace to error.log before dying.
// Uses only async-signal-safe functions (write, backtrace).
static void crashSignalHandler(int sig)
{
    const char *sigName = "UNKNOWN";
    if (sig == SIGABRT) sigName = "SIGABRT";
    else if (sig == SIGSEGV) sigName = "SIGSEGV";
    else if (sig == SIGBUS) sigName = "SIGBUS";

    // Write to stderr
    const char prefix[] = "\n*** CRASH: ";
    write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
    write(STDERR_FILENO, sigName, strlen(sigName));
    write(STDERR_FILENO, " ***\n", 5);

    // Stack trace
    void *frames[64];
    int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);

    // Also write to log file if open
    if (s_logFile && s_logFile->isOpen()) {
        int fd = s_logFile->handle();
        write(fd, prefix, sizeof(prefix) - 1);
        write(fd, sigName, strlen(sigName));
        write(fd, " ***\n", 5);
        backtrace_symbols_fd(frames, count, fd);
        fsync(fd);
    }

    // Re-raise with default handler to produce a core dump / crash report
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

static void appMessageHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
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
    QString timestamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    QString category = (ctx.category && ctx.category[0] && qstrcmp(ctx.category, "default") != 0)
                            ? QStringLiteral(" [%1]").arg(QString::fromUtf8(ctx.category))
                            : QString();
    out << timestamp << " " << label << category << ": " << msg << "\n";
    out.flush();
}

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
#else
    // Raise file descriptor limit.  macOS defaults to 256 soft limit, but
    // FreeRDP's WinPR uses pipe() for every event (2 FDs each, no eventfd on
    // macOS).  Thread pools, channels, and H264 codec decode threads easily
    // exhaust 256 FDs — raise to the hard limit (typically 10240+).
    {
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
            rl.rlim_cur = rl.rlim_max;
            setrlimit(RLIMIT_NOFILE, &rl);
        }
    }

    // Install crash signal handlers for diagnostics
    signal(SIGABRT, crashSignalHandler);
    signal(SIGSEGV, crashSignalHandler);
    signal(SIGBUS, crashSignalHandler);
#endif

    // Use exact DPI scale factor (e.g. 1.25x, 1.5x) instead of rounding to
    // nearest integer.  Prevents blurry / aliased glyphs on Windows displays
    // running at fractional scaling (125%, 150%).
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);

    // Load OpenSSL 3 legacy provider (RC4, MD4) — required for RDP licensing
    // and NTLM authentication.  Must happen before any FreeRDP/OpenSSL calls.
    // Point OPENSSL_MODULES at the directory containing the legacy provider:
    //   macOS:   <app>/Contents/Frameworks/legacy.dylib
    //   Windows: <exe dir>/legacy.dll
    {
        QString exeDir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_MACOS
        QString modulesDir = exeDir + QStringLiteral("/../Frameworks");
#else
        QString modulesDir = exeDir;
#endif
        qputenv("OPENSSL_MODULES", modulesDir.toUtf8());
        OSSL_PROVIDER_load(nullptr, "default");
        if (!OSSL_PROVIDER_load(nullptr, "legacy"))
            qWarning("Failed to load OpenSSL legacy provider from %s", qPrintable(modulesDir));
    }

    // Ensure antialiased font rendering on all platforms
    {
        QFont f = app.font();
        f.setStyleStrategy(QFont::PreferAntialias);
        app.setFont(f);
    }

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

    // Install cross-platform log handler → ~/.remotedesktop/error.log
    {
        QString logPath = configDir + QStringLiteral("/error.log");
        s_logFile = new QFile(logPath);

        // Cap at ~2 MB: truncate if oversized before opening in append mode
        if (s_logFile->exists() && s_logFile->size() > 2 * 1024 * 1024)
            s_logFile->resize(0);

        if (s_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
            qInstallMessageHandler(appMessageHandler);
    }

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
    // PBKDF2 with 600k iterations takes 0.5-2s — run only the CPU work off-thread.
    // DB reads/writes and signal emission stay on the main thread.
    auto *vault = appInstance->vault();
    if (vault->hasBeenSetup()) {
        MasterPasswordDialog dlg(MasterPasswordDialog::Unlock);
        if (dlg.exec() != QDialog::Accepted) {
            return 0;
        }
        QString pw = dlg.password();

        // Read salt from DB on main thread
        QByteArray salt = vault->readSalt();
        if (salt.isEmpty()) {
            QMessageBox::critical(nullptr, QStringLiteral("Error"),
                                  QStringLiteral("Vault salt not found."));
            return 1;
        }

        // Run PBKDF2 on thread pool (pure CPU, no DB access)
        QProgressDialog progress(QStringLiteral("Unlocking vault\u2026"), QString(), 0, 0);
        progress.setWindowModality(Qt::ApplicationModal);
        progress.setMinimumDuration(0);
        progress.setCancelButton(nullptr);

        QFutureWatcher<QByteArray> watcher;
        QObject::connect(&watcher, &QFutureWatcher<QByteArray>::finished,
                         &progress, &QProgressDialog::close);
        watcher.setFuture(QtConcurrent::run([pw, salt]() {
            return CredentialVault::pbkdf2Derive(pw, salt);
        }));
        progress.exec();

        // Verify on main thread (reads check value from DB, decrypts, emits signal)
        if (!vault->completeUnlock(watcher.result())) {
            QMessageBox::critical(nullptr, QStringLiteral("Error"),
                                  QStringLiteral("Incorrect master password."));
            return 1;
        }
    } else {
        MasterPasswordDialog dlg(MasterPasswordDialog::Setup);
        if (dlg.exec() != QDialog::Accepted) {
            return 0;
        }
        QString pw = dlg.password();

        // Generate salt and store on main thread
        QByteArray salt(16, 0);
        if (RAND_bytes(reinterpret_cast<unsigned char *>(salt.data()), 16) != 1) {
            QMessageBox::critical(nullptr, QStringLiteral("Error"),
                                  QStringLiteral("Failed to generate random salt."));
            return 1;
        }
        appInstance->userDatabase()->setMetadataValue(QStringLiteral("vault_salt"), salt);

        // Run PBKDF2 on thread pool (pure CPU, no DB access)
        QProgressDialog progress(QStringLiteral("Setting up vault\u2026"), QString(), 0, 0);
        progress.setWindowModality(Qt::ApplicationModal);
        progress.setMinimumDuration(0);
        progress.setCancelButton(nullptr);

        QFutureWatcher<QByteArray> watcher;
        QObject::connect(&watcher, &QFutureWatcher<QByteArray>::finished,
                         &progress, &QProgressDialog::close);
        watcher.setFuture(QtConcurrent::run([pw, salt]() {
            return CredentialVault::pbkdf2Derive(pw, salt);
        }));
        progress.exec();

        // Complete setup on main thread (encrypts check value, stores in DB, emits signal)
        if (!vault->completeSetup(watcher.result())) {
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

        // Save parent folder ID before dialog — index may become stale
        // if the model reloads (e.g. 30s poll timer) while dialog is open.
        qint64 parentFolderId = -1;
        if (parentIndex.isValid()) {
            auto *parentItem = treeModel->itemFromIndex(parentIndex);
            if (parentItem && parentItem->nodeType() == TreeNodeType::Folder)
                parentFolderId = parentItem->folder().id;
        }

        FolderDialog dlg(&mainWindow);
        dlg.setWindowTitle(QStringLiteral("Add Folder"));
        if (dlg.exec() != QDialog::Accepted)
            return;

        ConnectionFolder folder = dlg.folder();
        if (folder.name.isEmpty())
            return;

        folder.parentId = parentFolderId;
        folder.id = db->insertFolder(folder);
        if (folder.id > 0) {
            if (folder.defaultRdpCredentialId > 0 || folder.defaultSshCredentialId > 0) {
                userDb->setFolderDefaults(folder.id,
                                           folder.defaultRdpCredentialId,
                                           folder.defaultSshCredentialId);
            }
            mainWindow.refreshTree();
        }
    });

    QObject::connect(treeView, &ConnectionTreeView::addConnectionRequested,
                     &mainWindow, [&](const QModelIndex &parentIndex) {
        if (!admin)
            return;

        // Save parent folder ID before dialog — index may become stale
        qint64 parentFolderId = -1;
        if (parentIndex.isValid()) {
            auto *parentItem = treeModel->itemFromIndex(parentIndex);
            if (parentItem && parentItem->nodeType() == TreeNodeType::Folder)
                parentFolderId = parentItem->folder().id;
        }

        ConnectionDialog dlg(&mainWindow);
        dlg.setWindowTitle(QStringLiteral("Add Connection"));
        if (dlg.exec() != QDialog::Accepted)
            return;

        ConnectionEntry entry = dlg.connection();
        if (entry.name.isEmpty() || entry.hostname.isEmpty())
            return;

        entry.folderId = parentFolderId;

        qint64 credentialId = entry.credentialId;
        entry.credentialId = -1; // shared DB has no credential_id

        entry.id = db->insertConnection(entry);
        if (entry.id > 0) {
            if (credentialId > 0)
                userDb->setCredentialAssignment(entry.id, credentialId);
            mainWindow.refreshTree();
        }
    });

    QObject::connect(treeView, &ConnectionTreeView::editRequested,
                     &mainWindow, [&](const QModelIndex &index) {
        auto *item = treeModel->itemFromIndex(index);
        if (!item)
            return;

        if (item->nodeType() == TreeNodeType::Folder) {
            // Save IDs before dialog — index may become stale
            qint64 folderId = item->folder().id;
            qint64 parentId = item->folder().parentId;
            ConnectionFolder folderSnapshot = item->folder();

            FolderDialog dlg(&mainWindow);
            dlg.setWindowTitle(QStringLiteral("Edit Folder"));
            dlg.setFolder(folderSnapshot);
            if (!admin)
                dlg.setReadOnlyName(true);
            if (dlg.exec() != QDialog::Accepted)
                return;

            ConnectionFolder folder = dlg.folder();
            folder.id = folderId;
            folder.parentId = parentId;

            if (admin)
                db->updateFolder(folder);

            userDb->setFolderDefaults(folder.id,
                                       folder.defaultRdpCredentialId,
                                       folder.defaultSshCredentialId);

            if (dlg.forceInheritance()) {
                userDb->propagateCredentials(folder.id,
                                              folder.defaultRdpCredentialId,
                                              folder.defaultSshCredentialId,
                                              db);
            }
            mainWindow.refreshTree();

        } else {
            // Save IDs before dialog — index may become stale
            qint64 connId = item->connection().id;
            qint64 folderId = item->connection().folderId;
            ConnectionEntry connSnapshot = item->connection();

            ConnectionDialog dlg(&mainWindow);
            dlg.setWindowTitle(QStringLiteral("Edit Connection"));
            dlg.setConnection(connSnapshot);
            if (!admin)
                dlg.setReadOnlySharedFields(true);
            if (dlg.exec() != QDialog::Accepted)
                return;

            ConnectionEntry entry = dlg.connection();
            entry.id = connId;
            entry.folderId = folderId;

            if (admin) {
                ConnectionEntry sharedEntry = entry;
                sharedEntry.credentialId = -1;
                db->updateConnection(sharedEntry);
            }

            if (entry.credentialId > 0)
                userDb->setCredentialAssignment(entry.id, entry.credentialId);
            else
                userDb->removeCredentialAssignment(entry.id);

            mainWindow.refreshTree();
        }
    });

    QObject::connect(treeView, &ConnectionTreeView::deleteRequested,
                     &mainWindow, [&](const QModelIndex &index) {
        if (!admin)
            return;
        auto *item = treeModel->itemFromIndex(index);
        if (!item)
            return;

        // Save IDs before message box — index may become stale
        TreeNodeType nodeType = item->nodeType();
        qint64 itemId = item->itemId();
        QString itemName = item->name();
        QString typeStr = nodeType == TreeNodeType::Folder
            ? QStringLiteral("folder") : QStringLiteral("connection");

        auto result = QMessageBox::question(&mainWindow, QStringLiteral("Delete"),
            QStringLiteral("Delete %1 \"%2\"?").arg(typeStr, itemName));
        if (result != QMessageBox::Yes)
            return;

        if (nodeType == TreeNodeType::Folder) {
            db->deleteFolder(itemId);
        } else {
            // Disconnect if session is active
            auto *widget = tabWidget->sessionForConnection(itemId);
            if (widget) {
                widget->disconnect();
                if (auto *rdp = qobject_cast<RdpSessionWidget *>(widget))
                    rdp->disconnectSession();
                else if (auto *ssh = qobject_cast<SshSessionWidget *>(widget))
                    ssh->disconnectSession();
                tabWidget->removeSessionTab(itemId);
            }
            db->deleteConnection(itemId);
            userDb->removeCredentialAssignment(itemId);
        }
        mainWindow.refreshTree();
        mainWindow.updateStatusBar();
    });

    // Tab close -> disconnect
    QObject::connect(tabWidget, &SessionTabWidget::tabCloseRequested,
                     &mainWindow, [&](qint64 connectionId) {
        auto *widget = tabWidget->sessionForConnection(connectionId);
        if (widget)
            widget->disconnect();  // Detach all Qt signals before tearing down
        if (auto *rdp = qobject_cast<RdpSessionWidget *>(widget)) {
            rdp->disconnectSession();
        } else if (auto *ssh = qobject_cast<SshSessionWidget *>(widget)) {
            ssh->disconnectSession();
        }
        // Update tree icon to disconnected
        auto *mdl = mainWindow.treeModel();
        auto *item = mdl->findItemById(TreeNodeType::Connection, connectionId);
        if (item) {
            QModelIndex idx = mdl->indexFromItem(item);
            mdl->setConnectionState(idx, ConnectionState::Disconnected);
        }
        tabWidget->removeSessionTab(connectionId);
        mainWindow.updateStatusBar();
    });

    int result = app.exec();

    libssh2_exit();
    return result;
}
