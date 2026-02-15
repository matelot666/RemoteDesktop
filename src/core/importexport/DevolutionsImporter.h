#pragma once

#include <QString>
#include <QStringList>

class ConnectionDatabase;
class UserDatabase;
class CredentialVault;

struct ImportResult {
    int connectionsImported = 0;
    int foldersCreated = 0;
    int skipped = 0;
    QStringList warnings;
};

class DevolutionsImporter {
public:
    static ImportResult importFromFile(const QString &filePath,
                                       ConnectionDatabase *db,
                                       UserDatabase *userDb,
                                       CredentialVault *vault);
};
