#include "DevolutionsImporter.h"
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
static constexpr int RDM_TYPE_TERMINAL = 77;

ImportResult DevolutionsImporter::importFromFile(const QString &filePath,
                                                  ConnectionDatabase *db,
                                                  UserDatabase *userDb,
                                                  CredentialVault *vault)
{
    ImportResult result;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.warnings.append(QStringLiteral("Could not open file: %1").arg(filePath));
        return result;
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();

    if (parseError.error != QJsonParseError::NoError) {
        result.warnings.append(QStringLiteral("JSON parse error: %1").arg(parseError.errorString()));
        return result;
    }

    QJsonArray connections = doc.object().value(QStringLiteral("Connections")).toArray();
    if (connections.isEmpty()) {
        result.warnings.append(QStringLiteral("No 'Connections' array found in JSON."));
        return result;
    }

    // First pass: collect unique group paths and create folders (in shared DB)
    QMap<QString, qint64> folderPathMap;
    folderPathMap[QString()] = -1; // root

    for (const auto &val : connections) {
        QJsonObject obj = val.toObject();
        QString group = obj.value(QStringLiteral("Group")).toString();
        if (group.isEmpty())
            continue;

        QStringList segments = group.split(QStringLiteral("\\"), Qt::SkipEmptyParts);
        QString currentPath;
        qint64 parentId = -1;

        for (const auto &segment : segments) {
            if (!currentPath.isEmpty())
                currentPath += QStringLiteral("\\");
            currentPath += segment;

            if (folderPathMap.contains(currentPath)) {
                parentId = folderPathMap[currentPath];
                continue;
            }

            auto existingFolders = db->foldersInParent(parentId);
            qint64 existingId = -1;
            for (const auto &f : existingFolders) {
                if (f.name.compare(segment, Qt::CaseInsensitive) == 0) {
                    existingId = f.id;
                    break;
                }
            }

            if (existingId > 0) {
                folderPathMap[currentPath] = existingId;
                parentId = existingId;
            } else {
                ConnectionFolder folder;
                folder.name = segment;
                folder.parentId = parentId;
                folder.id = db->insertFolder(folder);
                if (folder.id > 0) {
                    folderPathMap[currentPath] = folder.id;
                    parentId = folder.id;
                    result.foldersCreated++;
                }
            }
        }
    }

    // Second pass: import connections (to shared DB) and credentials (to user DB)
    for (const auto &val : connections) {
        QJsonObject obj = val.toObject();
        int connType = obj.value(QStringLiteral("ConnectionType")).toInt(-1);

        Protocol protocol;
        QString hostname;
        int port = 0;

        if (connType == RDM_TYPE_RDP) {
            protocol = Protocol::RDP;
            hostname = obj.value(QStringLiteral("Url")).toString();
            port = obj.value(QStringLiteral("Port")).toInt(3389);
        } else if (connType == RDM_TYPE_SSH) {
            protocol = Protocol::SSH;
            QJsonObject putty = obj.value(QStringLiteral("Putty")).toObject();
            hostname = putty.value(QStringLiteral("Host")).toString();
            port = putty.value(QStringLiteral("SessionPort")).toInt(
                putty.value(QStringLiteral("Port")).toInt(22));
        } else if (connType == RDM_TYPE_TERMINAL) {
            protocol = Protocol::SSH;
            QJsonObject terminal = obj.value(QStringLiteral("Terminal")).toObject();
            hostname = terminal.value(QStringLiteral("Host")).toString();
            port = terminal.value(QStringLiteral("Port")).toInt(22);
        } else if (connType == RDM_TYPE_GROUP) {
            continue;
        } else {
            result.skipped++;
            continue;
        }

        if (hostname.isEmpty()) {
            result.skipped++;
            result.warnings.append(QStringLiteral("Skipped (no host): %1")
                                       .arg(obj.value(QStringLiteral("Name")).toString()));
            continue;
        }

        ConnectionEntry entry;
        entry.name = obj.value(QStringLiteral("Name")).toString();
        entry.hostname = hostname;
        entry.protocol = protocol;
        entry.port = port;

        // Resolve folder
        QString group = obj.value(QStringLiteral("Group")).toString();
        entry.folderId = folderPathMap.value(group, -1);

        if (entry.name.isEmpty())
            entry.name = entry.hostname;

        // Insert connection into shared DB (no credential_id)
        entry.id = db->insertConnection(entry);
        if (entry.id <= 0)
            continue;

        result.connectionsImported++;

        // Handle inline credentials -> insert to user DB + create assignment
        QJsonObject creds = obj.value(QStringLiteral("Credentials")).toObject();
        if (!creds.isEmpty() && userDb && vault && vault->isUnlocked()) {
            QString username = creds.value(QStringLiteral("UserName")).toString();
            QString password = creds.value(QStringLiteral("Password")).toString();
            QString domain = creds.value(QStringLiteral("Domain")).toString();

            if (!username.isEmpty()) {
                Credential cred;
                cred.name = entry.name + QStringLiteral(" (imported)");
                cred.type = CredentialType::UsernamePassword;
                if (!domain.isEmpty())
                    cred.username = domain + QStringLiteral("\\") + username;
                else
                    cred.username = username;
                cred.password = password;
                cred = vault->encryptCredential(cred);
                cred.id = userDb->insertCredential(cred);
                if (cred.id > 0) {
                    userDb->setCredentialAssignment(entry.id, cred.id);
                }
            }
        }
    }

    return result;
}
