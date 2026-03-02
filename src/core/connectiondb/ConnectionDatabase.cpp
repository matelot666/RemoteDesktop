#include "ConnectionDatabase.h"
#include "Schema.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QUuid>
#include <QDebug>

ConnectionDatabase::ConnectionDatabase()
{
    m_connectionName = QStringLiteral("remotedesktop_") +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
}

ConnectionDatabase::~ConnectionDatabase()
{
    close();
}

bool ConnectionDatabase::open(const QString &path)
{
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(path);

    if (!m_db.open()) {
        qWarning() << "Failed to open database:" << m_db.lastError().text();
        return false;
    }

    // Use DELETE journal mode — WAL doesn't work over network shares (SMB/CIFS)
    // because the -shm memory-mapped file requires shared locking that SMB can't provide.
    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=DELETE"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=5000"));

    if (!createTables())
        return false;

    migrate();
    return true;
}

void ConnectionDatabase::close()
{
    if (m_db.isOpen()) {
        m_db.close();
    }
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool ConnectionDatabase::createTables()
{
    QSqlQuery q(m_db);

    if (!q.exec(Schema::CreateMetadata)) {
        qWarning() << "Create metadata table failed:" << q.lastError().text();
        return false;
    }
    if (!q.exec(Schema::CreateFolders)) {
        qWarning() << "Create folders table failed:" << q.lastError().text();
        return false;
    }
    if (!q.exec(Schema::CreateConnections)) {
        qWarning() << "Create connections table failed:" << q.lastError().text();
        return false;
    }
    return true;
}

void ConnectionDatabase::migrate()
{
    QSqlQuery q(m_db);

    // Check connections table for legacy and new columns
    q.exec(QStringLiteral("PRAGMA table_info(connections)"));

    bool hasColorDepth = false;
    bool hasEnableClipboard = false;
    bool hasEnableCompression = false;
    bool hasWinKeyPassthrough = false;
    bool hasWinKeyCopyPaste = false;
    bool hasSecurityMode = false;
    bool hasCredentialId = false;

    while (q.next()) {
        const QString col = q.value(1).toString();
        if (col == QStringLiteral("color_depth"))          hasColorDepth = true;
        else if (col == QStringLiteral("enable_clipboard"))    hasEnableClipboard = true;
        else if (col == QStringLiteral("enable_compression"))  hasEnableCompression = true;
        else if (col == QStringLiteral("win_key_passthrough")) hasWinKeyPassthrough = true;
        else if (col == QStringLiteral("win_key_copy_paste"))  hasWinKeyCopyPaste = true;
        else if (col == QStringLiteral("security_mode"))       hasSecurityMode = true;
        else if (col == QStringLiteral("credential_id"))       hasCredentialId = true;
    }

    if (!hasColorDepth)
        q.exec(QStringLiteral("ALTER TABLE connections ADD COLUMN color_depth INTEGER NOT NULL DEFAULT 32"));
    if (!hasEnableClipboard)
        q.exec(QStringLiteral("ALTER TABLE connections ADD COLUMN enable_clipboard INTEGER NOT NULL DEFAULT 1"));
    if (!hasEnableCompression)
        q.exec(QStringLiteral("ALTER TABLE connections ADD COLUMN enable_compression INTEGER NOT NULL DEFAULT 1"));
    if (!hasWinKeyPassthrough)
        q.exec(QStringLiteral("ALTER TABLE connections ADD COLUMN win_key_passthrough INTEGER NOT NULL DEFAULT 1"));
    if (!hasWinKeyCopyPaste)
        q.exec(QStringLiteral("ALTER TABLE connections ADD COLUMN win_key_copy_paste INTEGER NOT NULL DEFAULT 1"));
    if (!hasSecurityMode)
        q.exec(QStringLiteral("ALTER TABLE connections ADD COLUMN security_mode INTEGER NOT NULL DEFAULT 0"));

    // Drop legacy credential columns if present
    if (hasCredentialId)
        q.exec(QStringLiteral("ALTER TABLE connections DROP COLUMN credential_id"));

    // Check folders table for legacy credential columns
    q.exec(QStringLiteral("PRAGMA table_info(folders)"));

    bool hasDefaultRdpCredentialId = false;
    bool hasDefaultSshCredentialId = false;

    while (q.next()) {
        const QString col = q.value(1).toString();
        if (col == QStringLiteral("default_rdp_credential_id")) hasDefaultRdpCredentialId = true;
        else if (col == QStringLiteral("default_ssh_credential_id")) hasDefaultSshCredentialId = true;
    }

    if (hasDefaultRdpCredentialId)
        q.exec(QStringLiteral("ALTER TABLE folders DROP COLUMN default_rdp_credential_id"));
    if (hasDefaultSshCredentialId)
        q.exec(QStringLiteral("ALTER TABLE folders DROP COLUMN default_ssh_credential_id"));

    // Drop legacy credentials table if present
    q.exec(QStringLiteral("DROP TABLE IF EXISTS credentials"));
}

// -- Folders ------------------------------------------------------------

qint64 ConnectionDatabase::insertFolder(const ConnectionFolder &folder)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO folders (name, parent_id, sort_order) VALUES (?, ?, ?)"));
    q.addBindValue(folder.name);
    q.addBindValue(folder.parentId);
    q.addBindValue(folder.sortOrder == 0
        ? nextSortOrder(QStringLiteral("folders"), QStringLiteral("parent_id"), folder.parentId)
        : folder.sortOrder);

    if (!q.exec()) {
        qWarning() << "Insert folder failed:" << q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

bool ConnectionDatabase::updateFolder(const ConnectionFolder &folder)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE folders SET name=?, parent_id=?, sort_order=? WHERE id=?"));
    q.addBindValue(folder.name);
    q.addBindValue(folder.parentId);
    q.addBindValue(folder.sortOrder);
    q.addBindValue(folder.id);
    return q.exec();
}

bool ConnectionDatabase::deleteFolder(qint64 id)
{
    m_db.transaction();
    if (!deleteFolderRecursive(id)) {
        m_db.rollback();
        return false;
    }
    return m_db.commit();
}

bool ConnectionDatabase::deleteFolderRecursive(qint64 id)
{
    QSqlQuery q(m_db);
    // Delete child connections
    q.prepare(QStringLiteral("DELETE FROM connections WHERE folder_id=?"));
    q.addBindValue(id);
    q.exec();

    // Delete child folders recursively
    auto children = foldersInParent(id);
    for (const auto &child : children) {
        if (!deleteFolderRecursive(child.id))
            return false;
    }

    q.prepare(QStringLiteral("DELETE FROM folders WHERE id=?"));
    q.addBindValue(id);
    return q.exec();
}

ConnectionFolder ConnectionDatabase::folderById(qint64 id) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, name, parent_id, sort_order FROM folders WHERE id=?"));
    q.addBindValue(id);

    ConnectionFolder f;
    if (q.exec() && q.next()) {
        f.id = q.value(0).toLongLong();
        f.name = q.value(1).toString();
        f.parentId = q.value(2).toLongLong();
        f.sortOrder = q.value(3).toInt();
    }
    return f;
}

QVector<ConnectionFolder> ConnectionDatabase::foldersInParent(qint64 parentId) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, name, parent_id, sort_order "
        "FROM folders WHERE parent_id=? ORDER BY sort_order, name"));
    q.addBindValue(parentId);

    QVector<ConnectionFolder> result;
    if (q.exec()) {
        while (q.next()) {
            ConnectionFolder f;
            f.id = q.value(0).toLongLong();
            f.name = q.value(1).toString();
            f.parentId = q.value(2).toLongLong();
            f.sortOrder = q.value(3).toInt();
            result.append(f);
        }
    }
    return result;
}

// -- Connections --------------------------------------------------------

qint64 ConnectionDatabase::insertConnection(const ConnectionEntry &entry)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO connections (name, protocol, hostname, port, folder_id, sort_order, color_depth, "
        "enable_clipboard, enable_compression, win_key_passthrough, win_key_copy_paste, security_mode) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(entry.name);
    q.addBindValue(static_cast<int>(entry.protocol));
    q.addBindValue(entry.hostname);
    q.addBindValue(entry.port);
    q.addBindValue(entry.folderId);
    q.addBindValue(entry.sortOrder == 0
        ? nextSortOrder(QStringLiteral("connections"), QStringLiteral("folder_id"), entry.folderId)
        : entry.sortOrder);
    q.addBindValue(entry.colorDepth);
    q.addBindValue(entry.enableClipboard ? 1 : 0);
    q.addBindValue(entry.enableCompression ? 1 : 0);
    q.addBindValue(entry.winKeyPassthrough ? 1 : 0);
    q.addBindValue(entry.winKeyCopyPaste ? 1 : 0);
    q.addBindValue(static_cast<int>(entry.securityMode));

    if (!q.exec()) {
        qWarning() << "Insert connection failed:" << q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

bool ConnectionDatabase::updateConnection(const ConnectionEntry &entry)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE connections SET name=?, protocol=?, hostname=?, port=?, "
        "folder_id=?, sort_order=?, color_depth=?, "
        "enable_clipboard=?, enable_compression=?, win_key_passthrough=?, win_key_copy_paste=?, "
        "security_mode=? WHERE id=?"));
    q.addBindValue(entry.name);
    q.addBindValue(static_cast<int>(entry.protocol));
    q.addBindValue(entry.hostname);
    q.addBindValue(entry.port);
    q.addBindValue(entry.folderId);
    q.addBindValue(entry.sortOrder);
    q.addBindValue(entry.colorDepth);
    q.addBindValue(entry.enableClipboard ? 1 : 0);
    q.addBindValue(entry.enableCompression ? 1 : 0);
    q.addBindValue(entry.winKeyPassthrough ? 1 : 0);
    q.addBindValue(entry.winKeyCopyPaste ? 1 : 0);
    q.addBindValue(static_cast<int>(entry.securityMode));
    q.addBindValue(entry.id);
    return q.exec();
}

bool ConnectionDatabase::deleteConnection(qint64 id)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM connections WHERE id=?"));
    q.addBindValue(id);
    return q.exec();
}

ConnectionEntry ConnectionDatabase::connectionById(qint64 id) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, name, protocol, hostname, port, folder_id, sort_order, color_depth, "
        "enable_clipboard, enable_compression, win_key_passthrough, win_key_copy_paste, security_mode "
        "FROM connections WHERE id=?"));
    q.addBindValue(id);

    ConnectionEntry e;
    if (q.exec() && q.next()) {
        e.id = q.value(0).toLongLong();
        e.name = q.value(1).toString();
        e.protocol = static_cast<Protocol>(q.value(2).toInt());
        e.hostname = q.value(3).toString();
        e.port = q.value(4).toInt();
        e.folderId = q.value(5).toLongLong();
        e.sortOrder = q.value(6).toInt();
        e.colorDepth = q.value(7).toInt();
        e.enableClipboard = q.value(8).toBool();
        e.enableCompression = q.value(9).toBool();
        e.winKeyPassthrough = q.value(10).toBool();
        e.winKeyCopyPaste = q.value(11).toBool();
        e.securityMode = static_cast<RdpSecurity>(q.value(12).toInt());
    }
    return e;
}

QVector<ConnectionEntry> ConnectionDatabase::connectionsInFolder(qint64 folderId) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, name, protocol, hostname, port, folder_id, sort_order, color_depth, "
        "enable_clipboard, enable_compression, win_key_passthrough, win_key_copy_paste, security_mode "
        "FROM connections WHERE folder_id=? ORDER BY sort_order, name"));
    q.addBindValue(folderId);

    QVector<ConnectionEntry> result;
    if (q.exec()) {
        while (q.next()) {
            ConnectionEntry e;
            e.id = q.value(0).toLongLong();
            e.name = q.value(1).toString();
            e.protocol = static_cast<Protocol>(q.value(2).toInt());
            e.hostname = q.value(3).toString();
            e.port = q.value(4).toInt();
            e.folderId = q.value(5).toLongLong();
            e.sortOrder = q.value(6).toInt();
            e.colorDepth = q.value(7).toInt();
            e.enableClipboard = q.value(8).toBool();
            e.enableCompression = q.value(9).toBool();
            e.winKeyPassthrough = q.value(10).toBool();
            e.winKeyCopyPaste = q.value(11).toBool();
            e.securityMode = static_cast<RdpSecurity>(q.value(12).toInt());
            result.append(e);
        }
    }
    return result;
}

QVector<ConnectionEntry> ConnectionDatabase::allConnections() const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, name, protocol, hostname, port, folder_id, sort_order, color_depth, "
        "enable_clipboard, enable_compression, win_key_passthrough, win_key_copy_paste, security_mode "
        "FROM connections ORDER BY sort_order, name"));

    QVector<ConnectionEntry> result;
    if (q.exec()) {
        while (q.next()) {
            ConnectionEntry e;
            e.id = q.value(0).toLongLong();
            e.name = q.value(1).toString();
            e.protocol = static_cast<Protocol>(q.value(2).toInt());
            e.hostname = q.value(3).toString();
            e.port = q.value(4).toInt();
            e.folderId = q.value(5).toLongLong();
            e.sortOrder = q.value(6).toInt();
            e.colorDepth = q.value(7).toInt();
            e.enableClipboard = q.value(8).toBool();
            e.enableCompression = q.value(9).toBool();
            e.winKeyPassthrough = q.value(10).toBool();
            e.winKeyCopyPaste = q.value(11).toBool();
            e.securityMode = static_cast<RdpSecurity>(q.value(12).toInt());
            result.append(e);
        }
    }
    return result;
}

// -- Metadata -----------------------------------------------------------

QByteArray ConnectionDatabase::metadataValue(const QString &key) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT value FROM metadata WHERE key=?"));
    q.addBindValue(key);

    if (q.exec() && q.next()) {
        return q.value(0).toByteArray();
    }
    return {};
}

bool ConnectionDatabase::setMetadataValue(const QString &key, const QByteArray &value)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO metadata (key, value) VALUES (?, ?)"));
    q.addBindValue(key);
    q.addBindValue(value);
    return q.exec();
}

QVector<ConnectionFolder> ConnectionDatabase::allFolders() const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, name, parent_id, sort_order "
        "FROM folders ORDER BY sort_order, name"));

    QVector<ConnectionFolder> result;
    if (q.exec()) {
        while (q.next()) {
            ConnectionFolder f;
            f.id = q.value(0).toLongLong();
            f.name = q.value(1).toString();
            f.parentId = q.value(2).toLongLong();
            f.sortOrder = q.value(3).toInt();
            result.append(f);
        }
    }
    return result;
}

bool ConnectionDatabase::moveConnection(qint64 connectionId, qint64 newFolderId)
{
    int newOrder = nextSortOrder(QStringLiteral("connections"),
                                 QStringLiteral("folder_id"), newFolderId);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE connections SET folder_id=?, sort_order=? WHERE id=?"));
    q.addBindValue(newFolderId);
    q.addBindValue(newOrder);
    q.addBindValue(connectionId);
    return q.exec();
}

bool ConnectionDatabase::moveFolder(qint64 folderId, qint64 newParentId)
{
    int newOrder = nextSortOrder(QStringLiteral("folders"),
                                 QStringLiteral("parent_id"), newParentId);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE folders SET parent_id=?, sort_order=? WHERE id=?"));
    q.addBindValue(newParentId);
    q.addBindValue(newOrder);
    q.addBindValue(folderId);
    return q.exec();
}

bool ConnectionDatabase::reorderConnections(qint64 folderId, const QVector<qint64> &idsInOrder)
{
    Q_UNUSED(folderId)
    m_db.transaction();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE connections SET sort_order=? WHERE id=?"));
    for (int i = 0; i < idsInOrder.size(); ++i) {
        q.addBindValue(i);
        q.addBindValue(idsInOrder[i]);
        if (!q.exec()) {
            m_db.rollback();
            return false;
        }
    }
    return m_db.commit();
}

bool ConnectionDatabase::reorderFolders(qint64 parentId, const QVector<qint64> &idsInOrder)
{
    Q_UNUSED(parentId)
    m_db.transaction();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE folders SET sort_order=? WHERE id=?"));
    for (int i = 0; i < idsInOrder.size(); ++i) {
        q.addBindValue(i);
        q.addBindValue(idsInOrder[i]);
        if (!q.exec()) {
            m_db.rollback();
            return false;
        }
    }
    return m_db.commit();
}

bool ConnectionDatabase::sortChildrenByName(qint64 parentFolderId)
{
    m_db.transaction();

    auto folders = foldersInParent(parentFolderId);
    auto connections = connectionsInFolder(parentFolderId);

    std::sort(folders.begin(), folders.end(), [](const ConnectionFolder &a, const ConnectionFolder &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    std::sort(connections.begin(), connections.end(),
              [](const ConnectionEntry &a, const ConnectionEntry &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE folders SET sort_order=? WHERE id=?"));
    for (int i = 0; i < folders.size(); ++i) {
        q.addBindValue(i);
        q.addBindValue(folders[i].id);
        if (!q.exec()) {
            m_db.rollback();
            return false;
        }
    }

    q.prepare(QStringLiteral("UPDATE connections SET sort_order=? WHERE id=?"));
    for (int i = 0; i < connections.size(); ++i) {
        q.addBindValue(i);
        q.addBindValue(connections[i].id);
        if (!q.exec()) {
            m_db.rollback();
            return false;
        }
    }

    return m_db.commit();
}

int ConnectionDatabase::nextSortOrder(const QString &table, const QString &parentColumn,
                                      qint64 parentId) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT COALESCE(MAX(sort_order), 0) + 1 FROM %1 WHERE %2=?")
                  .arg(table, parentColumn));
    q.addBindValue(parentId);

    if (q.exec() && q.next()) {
        return q.value(0).toInt();
    }
    return 1;
}
