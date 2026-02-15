#pragma once

#include <QSqlDatabase>
#include <QVector>
#include <QHash>
#include "core/Credential.h"
#include "core/Folder.h"

class ConnectionDatabase;

struct FolderDefaults {
    qint64 folderId = -1;
    qint64 defaultRdpCredentialId = -1;
    qint64 defaultSshCredentialId = -1;
};

class UserDatabase {
public:
    UserDatabase();
    ~UserDatabase();

    bool open(const QString &path);
    void close();

    // Credentials
    qint64 insertCredential(const Credential &cred);
    bool updateCredential(const Credential &cred);
    bool deleteCredential(qint64 id);
    Credential credentialById(qint64 id) const;
    QVector<Credential> allCredentials() const;

    // Metadata (for vault salt, etc.)
    QByteArray metadataValue(const QString &key) const;
    bool setMetadataValue(const QString &key, const QByteArray &value);

    // Credential assignments (connection_id → credential_id)
    qint64 getCredentialAssignment(qint64 connectionId) const;
    bool setCredentialAssignment(qint64 connectionId, qint64 credentialId);
    bool removeCredentialAssignment(qint64 connectionId);
    QHash<qint64, qint64> allCredentialAssignments() const;

    // Folder credential defaults
    FolderDefaults getFolderDefaults(qint64 folderId) const;
    bool setFolderDefaults(qint64 folderId, qint64 rdpCredId, qint64 sshCredId);
    QHash<qint64, FolderDefaults> allFolderDefaults() const;

    // Propagate credentials through shared DB folder tree, writing to user DB tables
    bool propagateCredentials(qint64 folderId, qint64 rdpCredId, qint64 sshCredId,
                              ConnectionDatabase *sharedDb);

    // Remove stale assignments for deleted connections/folders
    void cleanupOrphanedAssignments(ConnectionDatabase *sharedDb);

private:
    bool createTables();
    QSqlDatabase m_db;
    QString m_connectionName;
};
