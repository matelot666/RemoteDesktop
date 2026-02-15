#pragma once

#include <QObject>
#include <QString>
#include <memory>

class ConfigManager;
class ConnectionDatabase;
class UserDatabase;
class CredentialVault;

class Application : public QObject {
    Q_OBJECT
public:
    ~Application() override;
    static Application *instance();

    bool init(int argc, char *argv[]);

    QString configDir() const { return m_configDir; }
    QString databasePath() const;
    QString userDatabasePath() const;

    ConfigManager *config() const { return m_config.get(); }
    ConnectionDatabase *database() const { return m_database.get(); }
    UserDatabase *userDatabase() const { return m_userDb.get(); }
    CredentialVault *vault() const { return m_vault.get(); }

    bool isAdmin() const;

private:
    explicit Application(QObject *parent = nullptr);

    static Application *s_instance;
    QString m_configDir;
    std::unique_ptr<ConfigManager> m_config;
    std::unique_ptr<ConnectionDatabase> m_database;
    std::unique_ptr<UserDatabase> m_userDb;
    std::unique_ptr<CredentialVault> m_vault;
};
