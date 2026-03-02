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
#include <QXmlStreamReader>
#include <QMap>

namespace {

struct ConnRecord {
    QString name;
    QString group;
    QString connType;
    QString hostname;
    int port = 0;
    QString credUser;
    QString credPass;
    QString credDomain;
};

// --- XML parsing -----------------------------------------------------------

// Skip the current element and all its descendants.
static void skipElement(QXmlStreamReader &xml)
{
    int depth = 1;
    while (!xml.atEnd() && depth > 0) {
        xml.readNext();
        if (xml.isStartElement())
            ++depth;
        else if (xml.isEndElement())
            --depth;
    }
}

// Read all child elements of a <Connection> into a flat string map.
// Simple children: <Url>text</Url> → "Url" = "text"
// One-level nesting: <Putty><Host>text</Host></Putty> → "Putty/Host" = "text"
// Deeper nesting (e.g. <Security><Permissions>...) is skipped.
static QMap<QString, QString> readConnectionElement(QXmlStreamReader &xml)
{
    QMap<QString, QString> fields;

    // We are positioned on <Connection>. Read until its closing tag.
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement())
            break; // </Connection>
        if (!xml.isStartElement())
            continue;

        // Depth-1 child of <Connection>
        QString tag = xml.name().toString();

        // Use readElementText with SkipChildElements for leaf nodes.
        // But first peek to see if this has child elements — if so, read them
        // as "Parent/Child" keys instead.
        xml.readNext();

        if (xml.isEndElement()) {
            // Empty element like <Foo/>
            continue;
        }

        if (xml.isCharacters() || xml.isWhitespace()) {
            // Could be a simple text element, or whitespace before a child element.
            // Accumulate text, then check what follows.
            QString text = xml.text().toString();
            xml.readNext();

            if (xml.isEndElement()) {
                // Simple text element: <Tag>text</Tag>
                text = text.trimmed();
                if (!text.isEmpty())
                    fields[tag] = text;
                continue;
            }

            if (xml.isStartElement()) {
                // Had whitespace then a child — fall through to nested handling
            } else {
                // Something unexpected — skip the rest
                skipElement(xml);
                continue;
            }
        }

        if (xml.isStartElement()) {
            // Nested element: read depth-2 children as "Parent/Child" keys.
            // We're already positioned on the first child start element.
            do {
                QString childTag = xml.name().toString();
                // readElementText handles text content and skips any deeper nesting
                QString childText = xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                if (!childText.isEmpty())
                    fields[tag + QStringLiteral("/") + childTag] = childText;

                // Advance to next sibling or parent end
                while (!xml.atEnd()) {
                    xml.readNext();
                    if (xml.isStartElement())
                        break;        // next sibling
                    if (xml.isEndElement())
                        goto doneTag; // </Parent>
                }
            } while (xml.isStartElement());
            doneTag:;
        }
    }
    return fields;
}

static ConnRecord recordFromXmlFields(const QMap<QString, QString> &fields)
{
    ConnRecord rec;
    rec.name = fields.value(QStringLiteral("Name"));
    rec.group = fields.value(QStringLiteral("Group"));
    rec.connType = fields.value(QStringLiteral("ConnectionType"));

    if (rec.connType == QStringLiteral("RDPConfigured")) {
        rec.hostname = fields.value(QStringLiteral("Url"));
        rec.port = fields.value(QStringLiteral("Port"), QStringLiteral("3389")).toInt();
    } else if (rec.connType == QStringLiteral("SSHShell")) {
        rec.hostname = fields.value(QStringLiteral("Terminal/Host"));
        rec.port = fields.value(QStringLiteral("Terminal/Port"), QStringLiteral("22")).toInt();
    } else if (rec.connType == QStringLiteral("Putty")) {
        rec.hostname = fields.value(QStringLiteral("Putty/Host"));
        rec.port = fields.value(QStringLiteral("Putty/SessionPort"),
                                fields.value(QStringLiteral("Putty/Port"), QStringLiteral("22"))).toInt();
    }

    rec.credUser = fields.value(QStringLiteral("Credentials/UserName"));
    rec.credPass = fields.value(QStringLiteral("Credentials/Password"));
    rec.credDomain = fields.value(QStringLiteral("Credentials/Domain"));
    return rec;
}

static bool parseXml(const QByteArray &data, QVector<ConnRecord> &records, QStringList &warnings)
{
    QXmlStreamReader xml(data);

    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QStringLiteral("Connection"))
            records.append(recordFromXmlFields(readConnectionElement(xml)));
    }

    if (xml.hasError() && records.isEmpty()) {
        warnings.append(QStringLiteral("XML parse error: %1").arg(xml.errorString()));
        return false;
    }
    return true;
}

// --- JSON parsing ----------------------------------------------------------

// Devolutions RDM JSON uses integer ConnectionType values
static constexpr int RDM_JSON_TYPE_RDP = 1;
static constexpr int RDM_JSON_TYPE_SSH = 8;
static constexpr int RDM_JSON_TYPE_GROUP = 25;
static constexpr int RDM_JSON_TYPE_TERMINAL = 77;

static QString connTypeStringFromInt(int type)
{
    switch (type) {
    case RDM_JSON_TYPE_RDP:      return QStringLiteral("RDPConfigured");
    case RDM_JSON_TYPE_SSH:      return QStringLiteral("Putty");
    case RDM_JSON_TYPE_GROUP:    return QStringLiteral("Group");
    case RDM_JSON_TYPE_TERMINAL: return QStringLiteral("SSHShell");
    default:                     return QString::number(type);
    }
}

static bool parseJson(const QByteArray &data, QVector<ConnRecord> &records, QStringList &warnings)
{
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        warnings.append(QStringLiteral("JSON parse error: %1").arg(parseError.errorString()));
        return false;
    }

    QJsonArray connections = doc.object().value(QStringLiteral("Connections")).toArray();
    if (connections.isEmpty()) {
        warnings.append(QStringLiteral("No 'Connections' array found in JSON."));
        return false;
    }

    for (const auto &val : connections) {
        QJsonObject obj = val.toObject();

        ConnRecord rec;
        rec.name = obj.value(QStringLiteral("Name")).toString();
        rec.group = obj.value(QStringLiteral("Group")).toString();

        // JSON format uses integer ConnectionType — normalise to XML string names
        int connTypeInt = obj.value(QStringLiteral("ConnectionType")).toInt(-1);
        rec.connType = connTypeStringFromInt(connTypeInt);

        if (rec.connType == QStringLiteral("RDPConfigured")) {
            rec.hostname = obj.value(QStringLiteral("Url")).toString();
            rec.port = obj.value(QStringLiteral("Port")).toInt(3389);
        } else if (rec.connType == QStringLiteral("SSHShell")) {
            QJsonObject terminal = obj.value(QStringLiteral("Terminal")).toObject();
            rec.hostname = terminal.value(QStringLiteral("Host")).toString();
            rec.port = terminal.value(QStringLiteral("Port")).toInt(22);
        } else if (rec.connType == QStringLiteral("Putty")) {
            QJsonObject putty = obj.value(QStringLiteral("Putty")).toObject();
            rec.hostname = putty.value(QStringLiteral("Host")).toString();
            rec.port = putty.value(QStringLiteral("SessionPort")).toInt(
                putty.value(QStringLiteral("Port")).toInt(22));
        }

        QJsonObject creds = obj.value(QStringLiteral("Credentials")).toObject();
        rec.credUser = creds.value(QStringLiteral("UserName")).toString();
        rec.credPass = creds.value(QStringLiteral("Password")).toString();
        rec.credDomain = creds.value(QStringLiteral("Domain")).toString();

        records.append(rec);
    }
    return true;
}

// --- Common import logic ---------------------------------------------------

static void importRecords(const QVector<ConnRecord> &records,
                          ConnectionDatabase *db,
                          UserDatabase *userDb,
                          CredentialVault *vault,
                          ImportResult &result)
{
    // First pass: create folder hierarchy
    QMap<QString, qint64> folderPathMap;
    folderPathMap[QString()] = -1; // root

    for (const auto &rec : records) {
        if (rec.group.isEmpty())
            continue;

        QStringList segments = rec.group.split(QStringLiteral("\\"), Qt::SkipEmptyParts);
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

    // Second pass: import connections and credentials
    for (const auto &rec : records) {
        Protocol protocol;
        QString hostname = rec.hostname;
        int port = rec.port;

        if (rec.connType == QStringLiteral("RDPConfigured")) {
            protocol = Protocol::RDP;
            if (port == 0) port = 3389;
        } else if (rec.connType == QStringLiteral("SSHShell") ||
                   rec.connType == QStringLiteral("Putty")) {
            protocol = Protocol::SSH;
            if (port == 0) port = 22;
        } else if (rec.connType == QStringLiteral("Group")) {
            continue;
        } else {
            result.skipped++;
            continue;
        }

        if (hostname.isEmpty()) {
            result.skipped++;
            result.warnings.append(QStringLiteral("Skipped (no host): %1").arg(rec.name));
            continue;
        }

        ConnectionEntry entry;
        entry.name = rec.name.isEmpty() ? hostname : rec.name;
        entry.hostname = hostname;
        entry.protocol = protocol;
        entry.port = port;
        entry.folderId = folderPathMap.value(rec.group, -1);

        entry.id = db->insertConnection(entry);
        if (entry.id <= 0)
            continue;

        result.connectionsImported++;

        // Handle inline credentials
        if (!rec.credUser.isEmpty() && userDb && vault && vault->isUnlocked()) {
            Credential cred;
            cred.name = entry.name + QStringLiteral(" (imported)");
            cred.type = CredentialType::UsernamePassword;
            if (!rec.credDomain.isEmpty())
                cred.username = rec.credDomain + QStringLiteral("\\") + rec.credUser;
            else
                cred.username = rec.credUser;
            cred.password = rec.credPass;
            cred = vault->encryptCredential(cred);
            cred.id = userDb->insertCredential(cred);
            if (cred.id > 0) {
                userDb->setCredentialAssignment(entry.id, cred.id);
            }
        }
    }
}

} // anonymous namespace

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

    QByteArray data = file.readAll();
    file.close();

    if (data.isEmpty()) {
        result.warnings.append(QStringLiteral("File is empty."));
        return result;
    }

    // Detect format: XML starts with '<', JSON starts with '{'
    QVector<ConnRecord> records;
    char first = '\0';
    for (int i = 0; i < data.size(); ++i) {
        char c = data.at(i);
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            first = c;
            break;
        }
    }

    bool ok = false;
    if (first == '<') {
        ok = parseXml(data, records, result.warnings);
    } else if (first == '{' || first == '[') {
        ok = parseJson(data, records, result.warnings);
    } else {
        result.warnings.append(QStringLiteral("Unrecognised file format (expected XML or JSON)."));
        return result;
    }

    if (!ok)
        return result;

    if (records.isEmpty()) {
        result.warnings.append(QStringLiteral("No connections found in file."));
        return result;
    }

    importRecords(records, db, userDb, vault, result);
    return result;
}
