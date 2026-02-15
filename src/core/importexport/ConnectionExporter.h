#pragma once

#include <QString>

class ConnectionDatabase;
class UserDatabase;
class CredentialVault;

class ConnectionExporter {
public:
    static bool exportToFile(const QString &filePath,
                             ConnectionDatabase *db,
                             UserDatabase *userDb,
                             CredentialVault *vault,
                             bool includeCredentials);
};
