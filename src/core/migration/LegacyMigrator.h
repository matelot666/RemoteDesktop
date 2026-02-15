#pragma once

#include <QString>
#include <QStringList>

class ConnectionDatabase;
class UserDatabase;
class CredentialVault;

struct MigrationResult {
    bool success = false;
    int credentialsMigrated = 0;
    int assignmentsMigrated = 0;
    int folderDefaultsMigrated = 0;
    QStringList warnings;
};

class LegacyMigrator {
public:
    // Admin migration: extracts credentials from old connections.db into user.db,
    // then strips credential columns from the shared DB.
    static MigrationResult migrateAdmin(const QString &legacyDbPath,
                                         ConnectionDatabase *sharedDb,
                                         UserDatabase *userDb);

    // Non-admin migration: extracts credentials from a separate old connections.db
    // into user.db (does not modify the shared DB).
    static MigrationResult migrateNonAdmin(const QString &legacyDbPath,
                                            UserDatabase *userDb);

    // Check if a database has legacy credential tables/columns
    static bool isLegacyDatabase(const QString &dbPath);
};
