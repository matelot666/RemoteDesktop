#include "ConnectionExporter.h"
#include "core/connectiondb/ConnectionDatabase.h"
#include "core/userdb/UserDatabase.h"
#include "core/credentials/CredentialVault.h"
#include "core/ConnectionEntry.h"
#include "core/Folder.h"
#include "core/Credential.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>

// Devolutions RDM ConnectionType integer values
static constexpr int RDM_TYPE_RDP = 1;
static constexpr int RDM_TYPE_SSH = 8;
static constexpr int RDM_TYPE_GROUP = 25;

bool ConnectionExporter::exportToFile(const QString &filePath,
                                       ConnectionDatabase *db,
                                       UserDatabase *userDb,
                                       CredentialVault *vault,
                                       bool includeCredentials)
{
    // Build folder ID -> full path map
    auto allFolders = db->allFolders();
    QMap<qint64, QString> folderIdToName;
    QMap<qint64, qint64> folderIdToParent;
    for (const auto &f : allFolders) {
        folderIdToName[f.id] = f.name;
        folderIdToParent[f.id] = f.parentId;
    }

    // Resolve full path for each folder
    QMap<qint64, QString> folderPathMap;
    for (const auto &f : allFolders) {
        QStringList parts;
        qint64 current = f.id;
        int safety = 100;
        while (current > 0 && safety-- > 0) {
            parts.prepend(folderIdToName.value(current));
            current = folderIdToParent.value(current, -1);
        }
        folderPathMap[f.id] = parts.join(QStringLiteral("\\"));
    }

    // Pre-load credential assignments from user DB
    QHash<qint64, qint64> credAssignments;
    if (userDb)
        credAssignments = userDb->allCredentialAssignments();

    // Export folders as Group entries
    QJsonArray jsonConnections;
    for (const auto &f : allFolders) {
        QJsonObject obj;
        obj[QStringLiteral("ConnectionType")] = RDM_TYPE_GROUP;
        obj[QStringLiteral("Name")] = f.name;
        QString parentPath = folderPathMap.value(f.parentId);
        obj[QStringLiteral("Group")] = parentPath;
        jsonConnections.append(obj);
    }

    // Export connections
    auto allConns = db->allConnections();
    for (const auto &conn : allConns) {
        QJsonObject obj;

        obj[QStringLiteral("Name")] = conn.name;
        obj[QStringLiteral("Group")] = folderPathMap.value(conn.folderId);

        switch (conn.protocol) {
        case Protocol::RDP:
            obj[QStringLiteral("ConnectionType")] = RDM_TYPE_RDP;
            obj[QStringLiteral("Url")] = conn.hostname;
            if (conn.port != 3389)
                obj[QStringLiteral("Port")] = conn.port;
            break;
        case Protocol::SSH: {
            obj[QStringLiteral("ConnectionType")] = RDM_TYPE_SSH;
            QJsonObject putty;
            putty[QStringLiteral("Host")] = conn.hostname;
            putty[QStringLiteral("SessionPort")] = conn.port;
            putty[QStringLiteral("ProtocolType")] = 1; // SSH
            obj[QStringLiteral("Putty")] = putty;
            break;
        }
        }

        // Credentials (from user DB assignments)
        qint64 credentialId = credAssignments.value(conn.id, -1);
        if (includeCredentials && credentialId > 0 && userDb && vault && vault->isUnlocked()) {
            auto cred = userDb->credentialById(credentialId);
            cred = vault->decryptCredential(cred);

            QJsonObject credObj;
            QString username = cred.username;
            QString domain;

            int backslash = username.indexOf(QStringLiteral("\\"));
            if (backslash >= 0) {
                domain = username.left(backslash);
                username = username.mid(backslash + 1);
            }

            credObj[QStringLiteral("UserName")] = username;
            if (!domain.isEmpty())
                credObj[QStringLiteral("Domain")] = domain;
            credObj[QStringLiteral("Password")] = cred.password;
            obj[QStringLiteral("Credentials")] = credObj;
        }

        jsonConnections.append(obj);
    }

    QJsonObject root;
    root[QStringLiteral("Connections")] = jsonConnections;

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
    return true;
}
