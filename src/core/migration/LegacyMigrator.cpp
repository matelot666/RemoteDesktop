#include "LegacyMigrator.h"
#include "core/connectiondb/ConnectionDatabase.h"
#include "core/userdb/UserDatabase.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QUuid>
#include <QDebug>

bool LegacyMigrator::isLegacyDatabase(const QString &dbPath)
{
    QString connName = QStringLiteral("legacy_check_") +
        QUuid::createUuid().toString(QUuid::WithoutBraces);

    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        if (!db.open())
            return false;

        QSqlQuery q(db);
        // Check if credentials table exists
        q.exec(QStringLiteral(
            "SELECT name FROM sqlite_master WHERE type='table' AND name='credentials'"));
        bool hasCredentials = q.next();

        // Check if connections table has credential_id column
        bool hasCredentialId = false;
        q.exec(QStringLiteral("PRAGMA table_info(connections)"));
        while (q.next()) {
            if (q.value(1).toString() == QStringLiteral("credential_id")) {
                hasCredentialId = true;
                break;
            }
        }

        db.close();
        bool result = hasCredentials && hasCredentialId;
        QSqlDatabase::removeDatabase(connName);
        return result;
    }
}

MigrationResult LegacyMigrator::migrateAdmin(const QString &legacyDbPath,
                                               ConnectionDatabase *sharedDb,
                                               UserDatabase *userDb)
{
    MigrationResult result;

    // Open the legacy DB directly to read credentials and assignments
    QString connName = QStringLiteral("legacy_migrate_") +
        QUuid::createUuid().toString(QUuid::WithoutBraces);

    {
        QSqlDatabase legacyDb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        legacyDb.setDatabaseName(legacyDbPath);
        if (!legacyDb.open()) {
            result.warnings.append(QStringLiteral("Could not open legacy database."));
            QSqlDatabase::removeDatabase(connName);
            return result;
        }

        QSqlQuery q(legacyDb);

        // 1. Migrate credentials to user DB
        // Old credential IDs → new credential IDs
        QHash<qint64, qint64> credIdMap;

        q.exec(QStringLiteral(
            "SELECT id, name, type, username_encrypted, password_encrypted, private_key_encrypted "
            "FROM credentials"));
        while (q.next()) {
            Credential cred;
            qint64 oldId = q.value(0).toLongLong();
            cred.name = q.value(1).toString();
            cred.type = static_cast<CredentialType>(q.value(2).toInt());
            cred.usernameEncrypted = q.value(3).toByteArray();
            cred.passwordEncrypted = q.value(4).toByteArray();
            cred.privateKeyEncrypted = q.value(5).toByteArray();

            qint64 newId = userDb->insertCredential(cred);
            if (newId > 0) {
                credIdMap[oldId] = newId;
                result.credentialsMigrated++;
            }
        }

        // 2. Migrate credential assignments (connection.credential_id → credential_assignments)
        q.exec(QStringLiteral("SELECT id, credential_id FROM connections WHERE credential_id > 0"));
        while (q.next()) {
            qint64 connId = q.value(0).toLongLong();
            qint64 oldCredId = q.value(1).toLongLong();
            qint64 newCredId = credIdMap.value(oldCredId, -1);
            if (newCredId > 0) {
                userDb->setCredentialAssignment(connId, newCredId);
                result.assignmentsMigrated++;
            }
        }

        // 3. Migrate folder default credentials
        q.exec(QStringLiteral(
            "SELECT id, default_rdp_credential_id, default_ssh_credential_id FROM folders "
            "WHERE default_rdp_credential_id > 0 OR default_ssh_credential_id > 0"));
        while (q.next()) {
            qint64 folderId = q.value(0).toLongLong();
            qint64 oldRdp = q.value(1).toLongLong();
            qint64 oldSsh = q.value(2).toLongLong();
            qint64 newRdp = credIdMap.value(oldRdp, -1);
            qint64 newSsh = credIdMap.value(oldSsh, -1);
            if (newRdp > 0 || newSsh > 0) {
                userDb->setFolderDefaults(folderId, newRdp, newSsh);
                result.folderDefaultsMigrated++;
            }
        }

        // 4. Migrate vault metadata (salt + check) to user DB
        q.exec(QStringLiteral("SELECT key, value FROM metadata"));
        while (q.next()) {
            QString key = q.value(0).toString();
            QByteArray value = q.value(1).toByteArray();
            userDb->setMetadataValue(key, value);
        }

        legacyDb.close();
    }
    QSqlDatabase::removeDatabase(connName);

    result.success = true;
    return result;
}

MigrationResult LegacyMigrator::migrateNonAdmin(const QString &legacyDbPath,
                                                  UserDatabase *userDb)
{
    MigrationResult result;

    QString connName = QStringLiteral("legacy_nonadmin_") +
        QUuid::createUuid().toString(QUuid::WithoutBraces);

    {
        QSqlDatabase legacyDb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        legacyDb.setDatabaseName(legacyDbPath);
        if (!legacyDb.open()) {
            result.warnings.append(QStringLiteral("Could not open legacy database."));
            QSqlDatabase::removeDatabase(connName);
            return result;
        }

        QSqlQuery q(legacyDb);

        // Migrate credentials only
        q.exec(QStringLiteral(
            "SELECT id, name, type, username_encrypted, password_encrypted, private_key_encrypted "
            "FROM credentials"));
        while (q.next()) {
            Credential cred;
            cred.name = q.value(1).toString();
            cred.type = static_cast<CredentialType>(q.value(2).toInt());
            cred.usernameEncrypted = q.value(3).toByteArray();
            cred.passwordEncrypted = q.value(4).toByteArray();
            cred.privateKeyEncrypted = q.value(5).toByteArray();

            qint64 newId = userDb->insertCredential(cred);
            if (newId > 0)
                result.credentialsMigrated++;
        }

        // Migrate vault metadata
        q.exec(QStringLiteral("SELECT key, value FROM metadata"));
        while (q.next()) {
            userDb->setMetadataValue(q.value(0).toString(), q.value(1).toByteArray());
        }

        legacyDb.close();
    }
    QSqlDatabase::removeDatabase(connName);

    result.success = true;
    return result;
}
