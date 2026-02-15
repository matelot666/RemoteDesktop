#pragma once

#include <QString>
#include <QByteArray>
#include "Enums.h"

struct Credential {
    qint64 id = -1;
    QString name;
    CredentialType type = CredentialType::UsernamePassword;

    // Encrypted fields (stored as ciphertext in DB)
    QByteArray usernameEncrypted;
    QByteArray passwordEncrypted;
    QByteArray privateKeyEncrypted;

    // Decrypted fields (in memory only, never stored)
    QString username;
    QString password;
    QString privateKey;
};
