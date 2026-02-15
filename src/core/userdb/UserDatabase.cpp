#include "UserDatabase.h"
#include "UserSchema.h"
#include "core/connectiondb/ConnectionDatabase.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QUuid>
#include <QDebug>
#include <QSet>

UserDatabase::UserDatabase()
{
    m_connectionName = QStringLiteral("userdb_") +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
}

UserDatabase::~UserDatabase()
{
    close();
}

bool UserDatabase::open(const QString &path)
{
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(path);

    if (!m_db.open()) {
        qWarning() << "Failed to open user database:" << m_db.lastError().text();
        return false;
    }

    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));

    if (!createTables())
        return false;

    return true;
}

void UserDatabase::close()
{
    if (m_db.isOpen()) {
        m_db.close();
    }
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool UserDatabase::createTables()
{
    QSqlQuery q(m_db);

    if (!q.exec(UserSchema::CreateMetadata)) {
        qWarning() << "Create user metadata table failed:" << q.lastError().text();
        return false;
    }
    if (!q.exec(UserSchema::CreateCredentials)) {
        qWarning() << "Create user credentials table failed:" << q.lastError().text();
        return false;
    }
    if (!q.exec(UserSchema::CreateCredentialAssignments)) {
        qWarning() << "Create credential_assignments table failed:" << q.lastError().text();
        return false;
    }
    if (!q.exec(UserSchema::CreateFolderCredentialDefaults)) {
        qWarning() << "Create folder_credential_defaults table failed:" << q.lastError().text();
        return false;
    }
    return true;
}

// -- Credentials --------------------------------------------------------

qint64 UserDatabase::insertCredential(const Credential &cred)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO credentials (name, type, username_encrypted, password_encrypted, private_key_encrypted) "
        "VALUES (?, ?, ?, ?, ?)"));
    q.addBindValue(cred.name);
    q.addBindValue(static_cast<int>(cred.type));
    q.addBindValue(cred.usernameEncrypted);
    q.addBindValue(cred.passwordEncrypted);
    q.addBindValue(cred.privateKeyEncrypted);

    if (!q.exec()) {
        qWarning() << "Insert user credential failed:" << q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

bool UserDatabase::updateCredential(const Credential &cred)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE credentials SET name=?, type=?, username_encrypted=?, "
        "password_encrypted=?, private_key_encrypted=? WHERE id=?"));
    q.addBindValue(cred.name);
    q.addBindValue(static_cast<int>(cred.type));
    q.addBindValue(cred.usernameEncrypted);
    q.addBindValue(cred.passwordEncrypted);
    q.addBindValue(cred.privateKeyEncrypted);
    q.addBindValue(cred.id);
    return q.exec();
}

bool UserDatabase::deleteCredential(qint64 id)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM credentials WHERE id=?"));
    q.addBindValue(id);
    return q.exec();
}

Credential UserDatabase::credentialById(qint64 id) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, name, type, username_encrypted, password_encrypted, private_key_encrypted "
        "FROM credentials WHERE id=?"));
    q.addBindValue(id);

    Credential c;
    if (q.exec() && q.next()) {
        c.id = q.value(0).toLongLong();
        c.name = q.value(1).toString();
        c.type = static_cast<CredentialType>(q.value(2).toInt());
        c.usernameEncrypted = q.value(3).toByteArray();
        c.passwordEncrypted = q.value(4).toByteArray();
        c.privateKeyEncrypted = q.value(5).toByteArray();
    }
    return c;
}

QVector<Credential> UserDatabase::allCredentials() const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, name, type, username_encrypted, password_encrypted, private_key_encrypted "
        "FROM credentials"));

    QVector<Credential> result;
    if (q.exec()) {
        while (q.next()) {
            Credential c;
            c.id = q.value(0).toLongLong();
            c.name = q.value(1).toString();
            c.type = static_cast<CredentialType>(q.value(2).toInt());
            c.usernameEncrypted = q.value(3).toByteArray();
            c.passwordEncrypted = q.value(4).toByteArray();
            c.privateKeyEncrypted = q.value(5).toByteArray();
            result.append(c);
        }
    }
    return result;
}

// -- Metadata -----------------------------------------------------------

QByteArray UserDatabase::metadataValue(const QString &key) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT value FROM metadata WHERE key=?"));
    q.addBindValue(key);

    if (q.exec() && q.next()) {
        return q.value(0).toByteArray();
    }
    return {};
}

bool UserDatabase::setMetadataValue(const QString &key, const QByteArray &value)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO metadata (key, value) VALUES (?, ?)"));
    q.addBindValue(key);
    q.addBindValue(value);
    return q.exec();
}

// -- Credential Assignments ---------------------------------------------

qint64 UserDatabase::getCredentialAssignment(qint64 connectionId) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT credential_id FROM credential_assignments WHERE connection_id=?"));
    q.addBindValue(connectionId);

    if (q.exec() && q.next())
        return q.value(0).toLongLong();
    return -1;
}

bool UserDatabase::setCredentialAssignment(qint64 connectionId, qint64 credentialId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO credential_assignments (connection_id, credential_id) "
        "VALUES (?, ?)"));
    q.addBindValue(connectionId);
    q.addBindValue(credentialId);
    return q.exec();
}

bool UserDatabase::removeCredentialAssignment(qint64 connectionId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "DELETE FROM credential_assignments WHERE connection_id=?"));
    q.addBindValue(connectionId);
    return q.exec();
}

QHash<qint64, qint64> UserDatabase::allCredentialAssignments() const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT connection_id, credential_id FROM credential_assignments"));

    QHash<qint64, qint64> result;
    if (q.exec()) {
        while (q.next())
            result.insert(q.value(0).toLongLong(), q.value(1).toLongLong());
    }
    return result;
}

// -- Folder Credential Defaults -----------------------------------------

FolderDefaults UserDatabase::getFolderDefaults(qint64 folderId) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT folder_id, default_rdp_credential_id, default_ssh_credential_id "
        "FROM folder_credential_defaults WHERE folder_id=?"));
    q.addBindValue(folderId);

    FolderDefaults fd;
    fd.folderId = folderId;
    if (q.exec() && q.next()) {
        fd.defaultRdpCredentialId = q.value(1).toLongLong();
        fd.defaultSshCredentialId = q.value(2).toLongLong();
    }
    return fd;
}

bool UserDatabase::setFolderDefaults(qint64 folderId, qint64 rdpCredId, qint64 sshCredId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO folder_credential_defaults "
        "(folder_id, default_rdp_credential_id, default_ssh_credential_id) "
        "VALUES (?, ?, ?)"));
    q.addBindValue(folderId);
    q.addBindValue(rdpCredId);
    q.addBindValue(sshCredId);
    return q.exec();
}

QHash<qint64, FolderDefaults> UserDatabase::allFolderDefaults() const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT folder_id, default_rdp_credential_id, default_ssh_credential_id "
        "FROM folder_credential_defaults"));

    QHash<qint64, FolderDefaults> result;
    if (q.exec()) {
        while (q.next()) {
            FolderDefaults fd;
            fd.folderId = q.value(0).toLongLong();
            fd.defaultRdpCredentialId = q.value(1).toLongLong();
            fd.defaultSshCredentialId = q.value(2).toLongLong();
            result.insert(fd.folderId, fd);
        }
    }
    return result;
}

// -- Propagate Credentials ----------------------------------------------

bool UserDatabase::propagateCredentials(qint64 folderId, qint64 rdpCredId, qint64 sshCredId,
                                         ConnectionDatabase *sharedDb)
{
    if (!sharedDb)
        return false;

    m_db.transaction();

    std::function<bool(qint64)> recurse = [&](qint64 currentFolderId) -> bool {
        // Update credential assignments for connections in this folder
        auto connections = sharedDb->connectionsInFolder(currentFolderId);
        for (const auto &conn : connections) {
            if (rdpCredId >= 0 && conn.protocol == Protocol::RDP) {
                if (!setCredentialAssignment(conn.id, rdpCredId))
                    return false;
            }
            if (sshCredId >= 0 && conn.protocol == Protocol::SSH) {
                if (!setCredentialAssignment(conn.id, sshCredId))
                    return false;
            }
        }

        // Recurse into child folders
        auto children = sharedDb->foldersInParent(currentFolderId);
        for (const auto &child : children) {
            if (!setFolderDefaults(child.id, rdpCredId, sshCredId))
                return false;
            if (!recurse(child.id))
                return false;
        }
        return true;
    };

    bool ok = recurse(folderId);
    if (ok)
        m_db.commit();
    else
        m_db.rollback();
    return ok;
}

// -- Cleanup Orphans ----------------------------------------------------

void UserDatabase::cleanupOrphanedAssignments(ConnectionDatabase *sharedDb)
{
    if (!sharedDb)
        return;

    // Get all connection IDs from shared DB
    auto allConns = sharedDb->allConnections();
    QSet<qint64> validConnIds;
    for (const auto &c : allConns)
        validConnIds.insert(c.id);

    // Remove assignments for connections that no longer exist
    auto assignments = allCredentialAssignments();
    for (auto it = assignments.constBegin(); it != assignments.constEnd(); ++it) {
        if (!validConnIds.contains(it.key()))
            removeCredentialAssignment(it.key());
    }

    // Get all folder IDs from shared DB
    auto allFolders = sharedDb->allFolders();
    QSet<qint64> validFolderIds;
    for (const auto &f : allFolders)
        validFolderIds.insert(f.id);

    // Remove folder defaults for folders that no longer exist
    auto defaults = allFolderDefaults();
    for (auto it = defaults.constBegin(); it != defaults.constEnd(); ++it) {
        if (!validFolderIds.contains(it.key())) {
            QSqlQuery q(m_db);
            q.prepare(QStringLiteral(
                "DELETE FROM folder_credential_defaults WHERE folder_id=?"));
            q.addBindValue(it.key());
            q.exec();
        }
    }
}
