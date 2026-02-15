#include "Application.h"
#include "core/config/ConfigManager.h"
#include "core/connectiondb/ConnectionDatabase.h"
#include "core/userdb/UserDatabase.h"
#include "core/credentials/CredentialVault.h"

#include <QDir>

Application *Application::s_instance = nullptr;

Application::Application(QObject *parent)
    : QObject(parent)
{
    m_configDir = QDir::homePath() + QStringLiteral("/.remotedesktop");
}

Application::~Application() = default;

Application *Application::instance()
{
    if (!s_instance) {
        s_instance = new Application;
    }
    return s_instance;
}

bool Application::init(int argc, char *argv[])
{
    QDir dir;
    if (!dir.mkpath(m_configDir)) {
        return false;
    }

    // Create and load config manager
    m_config = std::make_unique<ConfigManager>();
    m_config->load(m_configDir);

    // Parse CLI overrides
    for (int i = 1; i < argc; ++i) {
        QString arg = QString::fromUtf8(argv[i]);
        if (arg.startsWith(QStringLiteral("--shared-db="))) {
            m_config->setSharedDbOverride(arg.mid(12));
        } else if (arg == QStringLiteral("--admin")) {
            m_config->setAdminOverride(true);
        }
    }

    // Open shared database (path comes from config)
    QString sharedPath = databasePath();
    if (sharedPath.isEmpty())
        return false;

    m_database = std::make_unique<ConnectionDatabase>();
    if (!m_database->open(sharedPath)) {
        return false;
    }

    // Open user database
    m_userDb = std::make_unique<UserDatabase>();
    if (!m_userDb->open(userDatabasePath())) {
        return false;
    }

    // Cleanup orphaned assignments
    m_userDb->cleanupOrphanedAssignments(m_database.get());

    // Create vault (uses user DB for metadata)
    m_vault = std::make_unique<CredentialVault>(m_userDb.get());

    return true;
}

QString Application::databasePath() const
{
    if (m_config)
        return m_config->sharedDatabasePath();
    return {};
}

QString Application::userDatabasePath() const
{
    return m_configDir + QStringLiteral("/user.db");
}

bool Application::isAdmin() const
{
    if (m_config)
        return m_config->isAdmin();
    return false;
}
